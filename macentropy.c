/*
 * MacShell entropy. See macentropy.h for the overview.
 *
 * Credit comes from the seed file and from the user's mouse and
 * keystrokes. The sound input and the clock jitter are sampled and mixed
 * into the pool too, and health-tested, but not credited: an idle sound
 * input can carry mains hum, a slow periodic signal (about 184 samples per
 * cycle for 60 Hz at this sampling rate), which simple per-sample tests
 * can't tell from noise. The health-test results still show in the entropy
 * command, so a stuck or silent input is reported.
 *
 * Estimation follows the spirit of NIST SP 800-90B:
 *   - repetition count test: a value repeated too many times in a row fails
 *   - adaptive proportion test: one value too common in a 512-sample window fails
 *   - min-entropy estimate (99% upper bound) from the worse of:
 *       most common value: how often the commonest value appears
 *       prediction: how often a sample equals the one 1..32 steps back
 *         (short-period signals only) or continues the last step (counters)
 * It returns 0 for silence, stuck values, counters and ramps.
 */
#include "macentropy.h"

#include <Events.h>
#include <Files.h>
#include <Folders.h>
#include <Fonts.h>
#include <Gestalt.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Quickdraw.h>
#include <Script.h>
#include <Sound.h>
#include <TextUtils.h>
#include <Threads.h>
#include <Timer.h>
#include <Windows.h>
#include <ToolUtils.h>

#include <string.h>

#include "mbedtls/sha256.h"

#define SOUND_BYTES        8192         /* ~0.75 s at 11 kHz, 8-bit mono */
#define JITTER_SAMPLES     2048
#define JITTER_LOOP        64           /* work per jitter sample */
#define RCT_CUTOFF         32           /* identical samples in a row = failure */
#define APT_WINDOW         512
#define APT_CUTOFF         460          /* > 90% one value in a window = failure */
#define CREDIT_DIVISOR     4            /* credit 1/4 of the estimate */
#define PRED_MAX_LAG       32           /* repeat predictors at lags 1..32 */

#define SEED_MAGIC         0x4D535253UL /* 'MSRS' */
#define SEED_VERSION       1
#define SEED_FLAG_FULL     1

static unsigned char g_pool[32];
static unsigned long g_counter;
static macentropy_stats g_stats;
static mbedtls_sha256_context g_ctx;     /* gathering context, finished into g_pool */

/* ------------------------------------------------------------------ */
/* Estimation and health tests                                        */
/* ------------------------------------------------------------------ */

/* Integer square root of a 64-bit value. */
static unsigned long isqrt64(unsigned long long v)
{
    unsigned long long r = 0, bit = 1ULL << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (unsigned long)r;
}

/* -log2(x) for 0 < x < 1, with x and the result in 16.16 fixed point.
   No floating point: many 68k Macs (and Mac OS 9's 68k emulator) have no FPU. */
static unsigned long neglog2_q16(unsigned long x)
{
    unsigned long k = 0, frac = 0, i;
    unsigned long long z = x;
    while (z < 65536UL) {               /* normalise z into [1, 2) */
        z <<= 1;
        k++;
    }
    for (i = 0; i < 16; i++) {          /* fractional bits of log2(z) by squaring */
        z = (z * z) >> 16;
        frac <<= 1;
        if (z >= 131072UL) {
            z >>= 1;
            frac |= 1;
        }
    }
    return (k << 16) - frac;            /* -log2(x) = k - log2(z) */
}

/* Returns credited bits for n byte samples, or 0 if a health test fails. */
static long assess(const unsigned char *s, long n)
{
    long counts[256];
    long i, run = 1, maxcount = 0;
    unsigned long p, pu, h, margin;

    if (n < APT_WINDOW)
        return 0;

    /* repetition count test */
    for (i = 1; i < n; i++) {
        run = (s[i] == s[i - 1]) ? run + 1 : 1;
        if (run >= RCT_CUTOFF)
            return 0;
    }

    /* adaptive proportion test, on consecutive windows */
    for (i = 0; i + APT_WINDOW <= n; i += APT_WINDOW) {
        long w, same = 0;
        unsigned char first = s[i];
        for (w = 0; w < APT_WINDOW; w++)
            if (s[i + w] == first)
                same++;
        if (same > APT_CUTOFF)
            return 0;
    }

    /* most common value estimate: how often the commonest value appears */
    memset(counts, 0, sizeof counts);
    for (i = 0; i < n; i++)
        counts[s[i]]++;
    for (i = 0; i < 256; i++)
        if (counts[i] > maxcount)
            maxcount = counts[i];
    p = (unsigned long)(((unsigned long long)maxcount * 65536UL + (unsigned long)n - 1) / (unsigned long)n);

    /* prediction estimate: how often the next sample can be predicted from
       earlier ones. Catches patterns the frequency count can't see:
       repeats at lags 1..PRED_MAX_LAG (periodic signals, mains hum) and
       "continue the last step" (counters, ramps). */
    {
        long lag, correct, best = 0, trials = n - PRED_MAX_LAG;
        for (lag = 1; lag <= PRED_MAX_LAG; lag++) {
            correct = 0;
            for (i = PRED_MAX_LAG; i < n; i++)
                if (s[i] == s[i - lag])
                    correct++;
            if (correct > best)
                best = correct;
        }
        correct = 0;
        for (i = PRED_MAX_LAG; i < n; i++)
            if (s[i] == (unsigned char)(2 * s[i - 1] - s[i - 2]))
                correct++;
        if (correct > best)
            best = correct;
        {
            unsigned long pp = (unsigned long)(((unsigned long long)best * 65536UL + (unsigned long)trials - 1) / (unsigned long)trials);
            if (pp > p)
                p = pp;
        }
    }

    /* 99% upper bound: pu = p + 2.576 * sqrt(p(1-p)/(n-1)), all in 16.16, rounded up */
    margin = isqrt64(((unsigned long long)p * (65536UL - (p > 65536UL ? 65536UL : p))) / (unsigned long)(n - 1));
    margin = (unsigned long)(((unsigned long long)margin * 168821UL + 65535UL) >> 16) + 1;   /* 2.576 = 168821/65536 */
    pu = p + margin;
    if (pu >= 65536UL)
        return 0;
    h = neglog2_q16(pu);                                    /* bits per sample, 16.16 */
    return (long)(((unsigned long long)h * (unsigned long)n) / (65536UL * CREDIT_DIVISOR));
}

/* ------------------------------------------------------------------ */
/* Sources                                                            */
/* ------------------------------------------------------------------ */

static void gather_sound(void)
{
    long resp = 0, refNum = 0;
    short sampleSize = 8, channels = 1;
    OSType comp = 'NONE';
    Ptr buf;
    SPB spb;
    OSErr err;

    if (Gestalt(gestaltSoundAttr, &resp) != noErr || !(resp & (1L << gestaltHasSoundInputDevice)))
        return;
    g_stats.sound_available = 1;

    buf = NewPtrClear(SOUND_BYTES);
    if (!buf)
        return;
    if (SPBOpenDevice(NULL, siWritePermission, &refNum) == noErr) {
        SPBSetDeviceInfo(refNum, siSampleSize, (Ptr)&sampleSize);
        SPBSetDeviceInfo(refNum, siNumberChannels, (Ptr)&channels);
        SPBSetDeviceInfo(refNum, siCompressionType, (Ptr)&comp);

        memset(&spb, 0, sizeof spb);
        spb.inRefNum = refNum;
        spb.count = SOUND_BYTES;
        spb.bufferLength = SOUND_BYTES;
        spb.bufferPtr = buf;
        err = SPBRecord(&spb, false);
        SPBCloseDevice(refNum);

        if (err == noErr && spb.count > 0) {
            long n = (long)spb.count;
            long bits;
            /* keep the raw samples in the pool regardless; credit only if healthy */
            mbedtls_sha256_update(&g_ctx, (const unsigned char *)buf, (size_t)n);
            /* health verdict only: the samples are already in the pool
               above, but they are credited 0 bits (see the file header). */
            bits = assess((const unsigned char *)buf, n);
            if (bits > 0)
                g_stats.sound_passed = 1;
            g_stats.sound_bits = 0;
        }
    }
    /* wipe the samples: they are noise, but there is no reason to keep them */
    memset(buf, 0, SOUND_BYTES);
    DisposePtr(buf);
}

static void gather_jitter(void)
{
    unsigned char *samples = (unsigned char *)NewPtr(JITTER_SAMPLES);
    UnsignedWide t0, t1;
    volatile unsigned long acc = 0;
    long i, k, bits;

    if (!samples)
        return;
    for (i = 0; i < JITTER_SAMPLES; i++) {
        Microseconds(&t0);
        for (k = 0; k < JITTER_LOOP; k++)
            acc = (acc << 1) ^ (acc >> 3) ^ (unsigned long)k;
        Microseconds(&t1);
        samples[i] = (unsigned char)((t1.lo - t0.lo) & 0xFF);
        mbedtls_sha256_update(&g_ctx, (const unsigned char *)&t1, sizeof t1);
    }
    /* health verdict only; credited 0 bits (see the file header) */
    bits = assess(samples, JITTER_SAMPLES);
    if (bits > 0)
        g_stats.jitter_passed = 1;
    g_stats.jitter_bits = 0;
    memset(samples, 0, JITTER_SAMPLES);
    DisposePtr((Ptr)samples);
    (void)acc;
}

/* ------------------------------------------------------------------ */
/* Seed file                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long magic;
    unsigned short version;
    unsigned short flags;
    unsigned char seed[32];
} seed_file;

static OSErr seed_spec(FSSpec *spec, Boolean create)
{
    short vRefNum;
    long dirID;
    OSErr err = FindFolder(kOnSystemDisk, kPreferencesFolderType, create, &vRefNum, &dirID);
    if (err)
        return err;
    return FSMakeFSSpec(vRefNum, dirID, "\pMacShell Random Seed", spec);
}

/* Reads the seed file and uses it up: the full-strength flag is cleared
   on disk before the seed is credited, so each stored seed counts once
   (also if MacShell is quit before the next seed is written, or the Mac
   stops). macentropy_save_seed() writes a fresh seed at the end of a
   successful start. If the file can't be written (a locked disk), the
   seed is still mixed in but not credited, and the user is asked for
   mouse and keyboard input instead. */
static void read_seed(void)
{
    FSSpec spec;
    short ref;
    long len = sizeof(seed_file);
    seed_file sf;
    int writable = 1;

    if (seed_spec(&spec, false) != noErr)
        return;
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) {
        /* locked file or volume: still mix it in, but credit nothing */
        writable = 0;
        if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr)
            return;
    }
    if (FSRead(ref, &len, &sf) == noErr && len == sizeof sf
        && sf.magic == SEED_MAGIC && sf.version == SEED_VERSION) {
        if ((sf.flags & SEED_FLAG_FULL) && writable) {
            long wlen = sizeof(seed_file);
            seed_file spent = sf;
            spent.flags = (unsigned short)(spent.flags & ~SEED_FLAG_FULL);
            if (SetFPos(ref, fsFromStart, 0) == noErr
                && FSWrite(ref, &wlen, &spent) == noErr
                && wlen == (long)sizeof(seed_file))
                g_stats.seed_valid = 1;
            memset(&spent, 0, sizeof spent);
        }
        mbedtls_sha256_update(&g_ctx, sf.seed, sizeof sf.seed);
    }
    FSClose(ref);
    memset(&sf, 0, sizeof sf);
}

int macentropy_save_seed(void)
{
    FSSpec spec;
    short ref;
    long len = sizeof(seed_file);
    seed_file sf;
    mbedtls_sha256_context c;
    static const unsigned char label[] = "MacShell seed v1";
    OSErr err;

    if (!g_stats.ready)
        return -1;
    sf.magic = SEED_MAGIC;
    sf.version = SEED_VERSION;
    sf.flags = SEED_FLAG_FULL;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, label, sizeof label - 1);
    mbedtls_sha256_update(&c, g_pool, sizeof g_pool);
    mbedtls_sha256_finish(&c, sf.seed);
    mbedtls_sha256_free(&c);

    err = seed_spec(&spec, true);
    if (err == fnfErr) {
        err = FSpCreate(&spec, 'MShl', 'MSrs', smSystemScript);
    }
    if (err == noErr)
        err = FSpOpenDF(&spec, fsWrPerm, &ref);
    if (err == noErr) {
        err = FSWrite(ref, &len, &sf);
        if (err == noErr)
            err = SetEOF(ref, len);
        FSClose(ref);
    }
    memset(&sf, 0, sizeof sf);
    return err == noErr ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Fallback: mouse and keyboard timing                                 */
/* ------------------------------------------------------------------ */

static void draw_progress(WindowPtr w, long have, long need)
{
    Rect bar, fill;
    SetPort(w);
    SetRect(&bar, 20, 78, 340, 92);
    FrameRect(&bar);
    fill = bar;
    InsetRect(&fill, 1, 1);
    fill.right = fill.left + (short)((long)(fill.right - fill.left) * (have > need ? need : have) / need);
    PaintRect(&fill);
}

/* Returns credited bits, or -1 if the user canceled (Command-period). */
static long gather_human(long need)
{
    Rect r;
    WindowPtr w;
    EventRecord ev;
    Point last = {0, 0}, now;
    UnsignedWide t, lastCredit = {0, 0};
    long have = 0;

    SetRect(&r, 0, 0, 360, 110);
    OffsetRect(&r, (qd.screenBits.bounds.right - 360) / 2, 80);
    w = NewWindow(NULL, &r, "\p", true, dBoxProc, (WindowPtr)-1, false, 0);
    if (!w)
        return -1;
    SetPort(w);
    TextFont(systemFont);
    MoveTo(20, 26);
    DrawString("\pMacShell needs some randomness to keep connections secure.");
    MoveTo(20, 46);
    DrawString("\pPlease move the mouse around and type a few keys.");
    MoveTo(20, 66);
    DrawString("\p(This happens only once.)");
    draw_progress(w, 0, need);
    GetMouse(&last);

    while (have < need) {
        if (WaitNextEvent(everyEvent, &ev, 1, NULL)) {
            if (ev.what == keyDown) {
                if ((ev.modifiers & cmdKey) && (ev.message & charCodeMask) == '.') {
                    DisposeWindow(w);
                    return -1;
                }
                Microseconds(&t);
                mbedtls_sha256_update(&g_ctx, (const unsigned char *)&t, sizeof t);
                mbedtls_sha256_update(&g_ctx, (const unsigned char *)&ev, sizeof ev);
                have += 2;
            } else if (ev.what == updateEvt && (WindowPtr)ev.message == w) {
                BeginUpdate(w);
                EndUpdate(w);
            }
        }
        GetMouse(&now);
        if (now.h != last.h || now.v != last.v) {
            Microseconds(&t);
            mbedtls_sha256_update(&g_ctx, (const unsigned char *)&t, sizeof t);
            mbedtls_sha256_update(&g_ctx, (const unsigned char *)&now, sizeof now);
            /* credit at most once per 20 ms of movement */
            if (t.lo - lastCredit.lo > 20000) {
                have += 1;
                lastCredit = t;
            }
            last = now;
        }
        draw_progress(w, have, need);
    }
    DisposeWindow(w);
    return have;
}

/* ------------------------------------------------------------------ */

int macentropy_init(void)
{
    static const unsigned char label[] = "MacShell entropy v1";
    UnsignedWide t;
    long fresh;

    memset(&g_stats, 0, sizeof g_stats);
    mbedtls_sha256_init(&g_ctx);
    mbedtls_sha256_starts(&g_ctx, 0);
    mbedtls_sha256_update(&g_ctx, label, sizeof label - 1);

    read_seed();
    gather_sound();
    gather_jitter();

    fresh = g_stats.sound_bits + g_stats.jitter_bits;
    g_stats.total_bits = fresh + (g_stats.seed_valid ? MACENTROPY_REQUIRED_BITS : 0);

    if (g_stats.total_bits < MACENTROPY_REQUIRED_BITS) {
        long human = gather_human(MACENTROPY_REQUIRED_BITS - g_stats.total_bits);
        if (human < 0) {
            mbedtls_sha256_free(&g_ctx);
            return -1;
        }
        g_stats.human_bits = human;
        g_stats.total_bits += human;
    }

    Microseconds(&t);
    mbedtls_sha256_update(&g_ctx, (const unsigned char *)&t, sizeof t);
    mbedtls_sha256_finish(&g_ctx, g_pool);
    mbedtls_sha256_free(&g_ctx);
    g_stats.ready = 1;

    macentropy_save_seed();         /* roll the seed forward for next time */
    return 0;
}

const macentropy_stats *macentropy_get_stats(void)
{
    return &g_stats;
}

/* ------------------------------------------------------------------ */
/* mbedTLS hooks                                                      */
/* ------------------------------------------------------------------ */

/* Output = SHA-256(pool || counter || time). The pool itself is never output. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    size_t done = 0;
    (void)data;

    *olen = 0;
    if (!g_stats.ready)
        return 0;                   /* no entropy yet: mbedTLS will refuse to seed */

    while (done < len) {
        unsigned char block[32];
        size_t n = len - done < sizeof block ? len - done : sizeof block;
        mbedtls_sha256_context c;
        UnsignedWide t;

        g_counter++;
        Microseconds(&t);
        mbedtls_sha256_init(&c);
        mbedtls_sha256_starts(&c, 0);
        mbedtls_sha256_update(&c, g_pool, sizeof g_pool);
        mbedtls_sha256_update(&c, (const unsigned char *)&g_counter, sizeof g_counter);
        mbedtls_sha256_update(&c, (const unsigned char *)&t, sizeof t);
        mbedtls_sha256_finish(&c, block);
        mbedtls_sha256_free(&c);
        memcpy(output + done, block, n);
        memset(block, 0, sizeof block);
        done += n;
    }
    *olen = done;
    return 0;
}

/* Called from mbedTLS's long public-key loops: let other threads run. */
void mbedtls_platform_yield(void)
{
    static unsigned short n;
    if ((++n & 3) == 0)
        YieldToAnyThread();
}
