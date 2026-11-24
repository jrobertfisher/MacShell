/*
 * MacShell SSH server.
 *
 * A small SSH-2 server built on mbedTLS, so another computer can log in to
 * MacShell with a key. Deliberately minimal:
 *   key exchange  ecdh-sha2-nistp256, with kex-strict-s-v00@openssh.com
 *                 (protection against the Terrapin attack)
 *   host key      ecdsa-sha2-nistp256, made on first start and kept in
 *                 Preferences:MacShell SSH Host Key
 *   cipher        aes128-ctr with hmac-sha2-256-etm@openssh.com
 *   login         public keys only (ecdsa-sha2-nistp256, rsa-sha2-256/512)
 *                 listed in ~/.ssh/authorized_keys; no passwords
 *   channels      one "session" channel: pty-req, shell, exec, env,
 *                 window-change
 * One connection at a time. Each connection gets its own MacShell tab; the
 * network work runs in a cooperative thread and the shell itself runs on
 * the main thread (sshd_tick), as with keyboard input.
 */

#include "app.h"
#include "console.h"
#include "shell.h"
#include "sshd.h"
#include "macentropy.h"

#include <Files.h>
#include <Folders.h>
#include <Script.h>
#include <DateTimeUtils.h>
#include <stdarg.h>
#include <Threads.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/rsa.h>
#include <mbedtls/pk.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform_util.h>

/* from shell.c */
void shell_run_line(int idx, char* line);
int shell_is_busy(int idx);
void shell_input(int session_idx, unsigned char c, int modifiers, unsigned char vkeycode);

#define SSH_MSG_DISCONNECT              1
#define SSH_MSG_IGNORE                  2
#define SSH_MSG_UNIMPLEMENTED           3
#define SSH_MSG_DEBUG                   4
#define SSH_MSG_SERVICE_REQUEST         5
#define SSH_MSG_SERVICE_ACCEPT          6
#define SSH_MSG_EXT_INFO                7
#define SSH_MSG_KEXINIT                 20
#define SSH_MSG_NEWKEYS                 21
#define SSH_MSG_KEX_ECDH_INIT           30
#define SSH_MSG_KEX_ECDH_REPLY          31
#define SSH_MSG_USERAUTH_REQUEST        50
#define SSH_MSG_USERAUTH_FAILURE        51
#define SSH_MSG_USERAUTH_SUCCESS        52
#define SSH_MSG_USERAUTH_PK_OK          60
#define SSH_MSG_GLOBAL_REQUEST          80
#define SSH_MSG_REQUEST_FAILURE         82
#define SSH_MSG_CHANNEL_OPEN            90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE    92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST   93
#define SSH_MSG_CHANNEL_DATA            94
#define SSH_MSG_CHANNEL_EOF             96
#define SSH_MSG_CHANNEL_CLOSE           97
#define SSH_MSG_CHANNEL_REQUEST         98
#define SSH_MSG_CHANNEL_SUCCESS         99
#define SSH_MSG_CHANNEL_FAILURE         100

#define MAX_PACKET      35000L
#define RBUF_SIZE       (MAX_PACKET + 64)
#define OUTQ_SIZE       8192
#define INQ_SIZE        2048
#define OUR_WINDOW      65536UL
#define OUR_MAXPKT      16384UL

static const char SERVER_VERSION[] = "SSH-2.0-MacShell_1.0";
static const char KEX_ALGS[] = "ecdh-sha2-nistp256,kex-strict-s-v00@openssh.com";
static const char HOSTKEY_ALG[] = "ecdsa-sha2-nistp256";
static const char CIPHER[] = "aes128-ctr";
static const char MAC_ALG[] = "hmac-sha2-256-etm@openssh.com";

struct cipher_state
{
	int on;
	mbedtls_aes_context aes;
	unsigned char ctr[16], stream[16];
	size_t nc_off;
	unsigned char mac_key[32];
};

struct conn
{
	EndpointRef ep;
	char peer[32];
	int alive;

	/* transport */
	unsigned char rbuf[RBUF_SIZE];
	long rlen;
	unsigned char pkt[RBUF_SIZE];       /* decrypted payload of the current packet */
	unsigned char sbuf[RBUF_SIZE];      /* outgoing packet */
	unsigned char obuf[OUR_MAXPKT + 16]; /* flush_output's channel data message
	                                       (kept here, not on the thread's stack) */
	unsigned long seq_in, seq_out;
	struct cipher_state in, out, next_in, next_out;
	int have_next;
	char v_c[256];
	unsigned char i_c[2048], i_s[512];
	long i_c_len, i_s_len;
	unsigned char session_id[32];
	int have_session_id;
	int strict, first_kex_done, in_kex, packets_in;

	/* login */
	int authed, auth_failures;
	char user[64];

	/* channel */
	int chan_open;
	unsigned long peer_chan, peer_window, peer_maxpkt, our_window;
	int want_session;                   /* 1 = shell, 2 = exec */
	char exec_cmd[512];
	int eof_sent, close_sent;

	/* shared with the main thread (cooperative threads, so no locking) */
	int session_idx;                    /* MacShell tab, -1 = none yet */
	int session_failed, exec_started, exec_done, shell_closed;
	int exit_status;
	char outq[OUTQ_SIZE];
	long outq_len;
	unsigned char inq[INQ_SIZE];
	long inq_len;
	int esc_state;
	char esc_num[8];
	int esc_len;
};

/* server state */
static struct conn* g_conn = NULL;          /* current connection */
static int g_running = 0, g_stop = 0;
static ThreadID g_thread = kNoThreadID;
static unsigned short g_port = 22;
static int g_msg_idx = -1;                  /* tab for status messages */
static int g_claim_pending = 0;             /* new tab belongs to g_conn */
static long g_connections = 0;

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_pk_context g_hostkey;
static unsigned char g_hostkey_blob[128];
static long g_hostkey_blob_len = 0;

/* ------------------------------------------------------------------ */
/* messages to the tab that started the server                        */
/* ------------------------------------------------------------------ */

/* folder dirID of name inside parent, created if missing */
static int log_folder(short vref, long parent, const char* name, long* dir)
{
	CInfoPBRec pb;
	Str255 pn;
	OSErr err;
	pn[0] = strlen(name);
	memcpy(pn + 1, name, pn[0]);
	memset(&pb, 0, sizeof pb);
	pb.dirInfo.ioNamePtr = pn;
	pb.dirInfo.ioVRefNum = vref;
	pb.dirInfo.ioDrDirID = parent;
	err = PBGetCatInfoSync(&pb);
	if (err == noErr)
	{
		if (!(pb.dirInfo.ioFlAttrib & 0x10)) return 0;
		*dir = pb.dirInfo.ioDrDirID;
		return 1;
	}
	return err == fnfErr && DirCreate(vref, parent, pn, dir) == noErr;
}

/* Append a line to ~/.sshd/logs/sshd.log (Documents:.sshd:logs on the
   startup disk), with the date and time. At 256 KB it becomes sshd.log.1. */
#define LOG_MAX 262144L
#define LOG_MIN_FREE 1048576L           /* stop logging below 1 MB free */
static int g_log_paused = 0;

static long disk_free_bytes(short vref)
{
	HParamBlockRec pb;
	memset(&pb, 0, sizeof pb);
	pb.volumeParam.ioVRefNum = vref;
	if (PBHGetVInfoSync(&pb) != noErr) return 0x7FFFFFFFL;
	return (long)(unsigned short)pb.volumeParam.ioVFrBlk * (long)pb.volumeParam.ioVAlBlkSiz;
}

static void log_line(const char* text)
{
	short vref, ref;
	long sys, docs, sshd_dir, logs, eof, n;
	FSSpec spec;
	unsigned long secs;
	DateTimeRec dt;
	char line[320];
	int i, k;

	if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &vref, &sys) != noErr) return;

	/* never fill the disk: pause below 1 MB free (noting it once) */
	{
		long free_b = disk_free_bytes(vref);
		if (free_b < LOG_MIN_FREE)
		{
			if (g_log_paused) return;
			g_log_paused = 1;
			text = "log paused: less than 1 MB free on the startup disk";
			if (g_msg_idx >= 0 && sessions[g_msg_idx].in_use)
				printf_s(g_msg_idx, "sshd: %s\r\n", text);
			if (free_b < 4096) return;
		}
		else if (g_log_paused)
		{
			g_log_paused = 0;
			log_line("log resumed: disk space is available again");
		}
	}

	if (!log_folder(vref, fsRtDirID, "Documents", &docs) ||
	    !log_folder(vref, docs, ".sshd", &sshd_dir) ||
	    !log_folder(vref, sshd_dir, "logs", &logs))
		return;

	if (FSMakeFSSpec(vref, logs, "\psshd.log", &spec) == fnfErr)
		FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript);
	if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return;
	GetEOF(ref, &eof);
	if (eof > LOG_MAX)
	{
		FSSpec old;
		FSClose(ref);
		if (FSMakeFSSpec(vref, logs, "\psshd.log.1", &old) == noErr) FSpDelete(&old);
		FSpRename(&spec, "\psshd.log.1");
		FSMakeFSSpec(vref, logs, "\psshd.log", &spec);
		FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript);
		if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) return;
		eof = 0;
	}

	GetDateTime(&secs);
	SecondsToDate(secs, &dt);
	n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d ",
	             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
	/* the message without its "sshd: " prefix and line ending; Mac line endings */
	if (strncmp(text, "sshd: ", 6) == 0) text += 6;
	for (i = 0, k = n; text[i] && k < (int)sizeof line - 2; i++)
		if (text[i] != '\r' && text[i] != '\n')
			line[k++] = text[i];
	line[k++] = '\r';
	n = k;
	SetFPos(ref, fsFromStart, eof);
	FSWrite(ref, &n, line);
	FSClose(ref);
}

/* a message for the sshd tab and the log */
static void say(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	log_line(buf);
	if (g_msg_idx >= 0 && g_msg_idx < MAX_SESSIONS && sessions[g_msg_idx].in_use)
		printf_s(g_msg_idx, "%s", buf);
}

/* a message for the log only */
static void log_only(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	log_line(buf);
}

/* "SHA256:..." of a public key blob */
static void blob_fingerprint(const unsigned char* blob, long len, char* out, int size)
{
	unsigned char h[32], b64[64];
	size_t n;
	mbedtls_sha256(blob, len, h, 0);
	mbedtls_base64_encode(b64, sizeof b64, &n, h, 32);
	while (n > 0 && b64[n - 1] == '=') n--;
	b64[n] = 0;
	snprintf(out, size, "SHA256:%s", (char*)b64);
}

/* ------------------------------------------------------------------ */
/* SSH data encoding                                                  */
/* ------------------------------------------------------------------ */

struct wr { unsigned char* p; long n, max; int bad; };
struct rd { const unsigned char* p; long n; int bad; };

static void w_bytes(struct wr* w, const void* d, long n)
{
	if (w->bad || n < 0 || w->n + n > w->max) { w->bad = 1; return; }
	memcpy(w->p + w->n, d, n);
	w->n += n;
}
static void w_u8(struct wr* w, unsigned char v) { w_bytes(w, &v, 1); }
static void w_u32(struct wr* w, unsigned long v)
{
	unsigned char b[4];
	b[0] = (unsigned char)(v >> 24); b[1] = (unsigned char)(v >> 16);
	b[2] = (unsigned char)(v >> 8);  b[3] = (unsigned char)v;
	w_bytes(w, b, 4);
}
static void w_str(struct wr* w, const void* d, long n) { w_u32(w, n); w_bytes(w, d, n); }
static void w_cstr(struct wr* w, const char* s) { w_str(w, s, strlen(s)); }
/* mpint from a big-endian unsigned number */
static void w_mpint(struct wr* w, const unsigned char* d, long n)
{
	while (n > 0 && d[0] == 0) { d++; n--; }
	if (n > 0 && (d[0] & 0x80))
	{
		w_u32(w, n + 1);
		w_u8(w, 0);
		w_bytes(w, d, n);
	}
	else
		w_str(w, d, n);
}

static unsigned long r_u32(struct rd* r)
{
	unsigned long v;
	if (r->bad || r->n < 4) { r->bad = 1; return 0; }
	v = ((unsigned long)r->p[0] << 24) | ((unsigned long)r->p[1] << 16) |
	    ((unsigned long)r->p[2] << 8) | r->p[3];
	r->p += 4; r->n -= 4;
	return v;
}
static int r_u8(struct rd* r)
{
	if (r->bad || r->n < 1) { r->bad = 1; return 0; }
	r->n--;
	return *r->p++;
}
static const unsigned char* r_str(struct rd* r, long* len)
{
	unsigned long n = r_u32(r);
	const unsigned char* p;
	if (r->bad || n > (unsigned long)r->n) { r->bad = 1; *len = 0; return (const unsigned char*)""; }
	p = r->p;
	r->p += n; r->n -= n;
	*len = (long)n;
	return p;
}
static int str_is(const unsigned char* p, long n, const char* s)
{
	return (long)strlen(s) == n && memcmp(p, s, n) == 0;
}
/* is name in the comma-separated list? */
static int list_has(const unsigned char* list, long n, const char* name)
{
	long i = 0, nl = strlen(name);
	while (i < n)
	{
		long j = i;
		while (j < n && list[j] != ',') j++;
		if (j - i == nl && memcmp(list + i, name, nl) == 0) return 1;
		i = j + 1;
	}
	return 0;
}

static int rng(void* ctx, unsigned char* out, size_t len)
{
	return mbedtls_ctr_drbg_random(ctx, out, len);
}

/* ------------------------------------------------------------------ */
/* host key                                                           */
/* ------------------------------------------------------------------ */

static int prefs_file(FSSpec* spec)
{
	short vref;
	long dir;
	if (FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &vref, &dir) != noErr)
		return 0;
	FSMakeFSSpec(vref, dir, "\pMacShell SSH Host Key", spec);
	return 1;
}

static int hostkey_blob_make(void)
{
	unsigned char q[65];
	size_t qlen;
	struct wr w;
	if (mbedtls_ecp_write_public_key(mbedtls_pk_ec(g_hostkey), MBEDTLS_ECP_PF_UNCOMPRESSED,
	                                 &qlen, q, sizeof q) != 0 || qlen != 65)
		return 0;
	w.p = g_hostkey_blob; w.n = 0; w.max = sizeof g_hostkey_blob; w.bad = 0;
	w_cstr(&w, HOSTKEY_ALG);
	w_cstr(&w, "nistp256");
	w_str(&w, q, 65);
	g_hostkey_blob_len = w.n;
	return !w.bad;
}

static int hostkey_load_or_make(int idx)
{
	FSSpec spec;
	short ref;
	unsigned char der[256];
	long n = sizeof der;
	int ret;

	mbedtls_pk_free(&g_hostkey);
	mbedtls_pk_init(&g_hostkey);
	if (!prefs_file(&spec)) return 0;

	if (FSpOpenDF(&spec, fsRdPerm, &ref) == noErr)
	{
		OSErr re = FSRead(ref, &n, der);       /* eofErr: file shorter than der, n is right */
		FSClose(ref);
		if (re != noErr && re != eofErr) n = 0;
		ret = mbedtls_pk_parse_key(&g_hostkey, der, n, NULL, 0, rng, &g_drbg);
		mbedtls_platform_zeroize(der, sizeof der);
		if (ret == 0 && mbedtls_pk_get_type(&g_hostkey) == MBEDTLS_PK_ECKEY)
			return hostkey_blob_make();
		printf_s(idx, "sshd: the host key file is damaged; delete Preferences:MacShell SSH Host Key\r\n");
		return 0;
	}

	/* first start: make an ECDSA P-256 host key */
	printf_s(idx, "sshd: making this Mac's host key... ");
	if (mbedtls_pk_setup(&g_hostkey, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
	    mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(g_hostkey), rng, &g_drbg) != 0)
	{
		printf_s(idx, "failed\r\n");
		return 0;
	}
	ret = mbedtls_pk_write_key_der(&g_hostkey, der, sizeof der);
	if (ret <= 0) { printf_s(idx, "failed\r\n"); return 0; }
	n = ret;
	FSpCreate(&spec, 'MShl', 'MShk', smSystemScript);
	if (FSpOpenDF(&spec, fsRdWrPerm, &ref) != noErr) { printf_s(idx, "cannot save it\r\n"); return 0; }
	SetEOF(ref, 0);
	FSWrite(ref, &n, der + sizeof der - ret);
	FSClose(ref);
	mbedtls_platform_zeroize(der, sizeof der);
	printf_s(idx, "done.\r\n");
	return hostkey_blob_make();
}

static void hostkey_fingerprint(char* out, int size)
{
	unsigned char h[32], b64[64];
	size_t n;
	mbedtls_sha256(g_hostkey_blob, g_hostkey_blob_len, h, 0);
	mbedtls_base64_encode(b64, sizeof b64, &n, h, 32);
	while (n > 0 && b64[n - 1] == '=') n--;
	b64[n] = 0;
	snprintf(out, size, "SHA256:%s", (char*)b64);
}

/* ------------------------------------------------------------------ */
/* network                                                            */
/* ------------------------------------------------------------------ */

static int net_poll(struct conn* c);

static int net_send(struct conn* c, const unsigned char* d, long n)
{
	unsigned long deadline = TickCount() + 1800;
	while (n > 0)
	{
		OTResult r;
		if (!c->alive) return 0;
		r = OTSnd(c->ep, (void*)d, n, 0);
		if (r == kOTFlowErr || r == kOTNoDataErr) { YieldToAnyThread(); if (TickCount() > deadline) return 0; continue; }
		if (r == kOTLookErr)
		{
			/* an event is waiting. "Sending may continue" (T_GODATA) and
			   "data arrived" (T_DATA) just mean: read the data in and try again.
			   A disconnect ends the connection. */
			OTResult ev = OTLook(c->ep);
			if (ev == T_GODATA || ev == T_GOEXDATA) continue;
			if ((ev == T_DATA || ev == T_EXDATA) && net_poll(c))
			{
				YieldToAnyThread();
				if (TickCount() > deadline) return 0;
				continue;
			}
			if (ev == T_ORDREL) OTRcvOrderlyDisconnect(c->ep);
			else if (ev == T_DISCONNECT) OTRcvDisconnect(c->ep, NULL);
			c->alive = 0;
			return 0;
		}
		if (r < 0) { c->alive = 0; return 0; }
		d += r; n -= r;
		deadline = TickCount() + 1800;
	}
	return 1;
}

/* read whatever has arrived; returns 0 if the connection has gone */
static int net_poll(struct conn* c)
{
	OTFlags flags = 0;
	OTResult r;
	if (c->rlen >= RBUF_SIZE) return 1;
	r = OTRcv(c->ep, c->rbuf + c->rlen, RBUF_SIZE - c->rlen, &flags);
	if (r > 0) { c->rlen += r; return 1; }
	if (r == kOTNoDataErr) return 1;
	if (r == kOTLookErr)
	{
		OTResult ev = OTLook(c->ep);
		if (ev == T_ORDREL) OTRcvOrderlyDisconnect(c->ep);
		else if (ev == T_DISCONNECT) OTRcvDisconnect(c->ep, NULL);
		else return 1;
	}
	c->alive = 0;
	return 0;
}

/* ------------------------------------------------------------------ */
/* packets                                                            */
/* ------------------------------------------------------------------ */

static void mac_compute(struct cipher_state* cs, unsigned long seq, const unsigned char* data, long n,
                        unsigned char out[32])
{
	mbedtls_md_context_t md;
	unsigned char s[4];
	s[0] = (unsigned char)(seq >> 24); s[1] = (unsigned char)(seq >> 16);
	s[2] = (unsigned char)(seq >> 8);  s[3] = (unsigned char)seq;
	mbedtls_md_init(&md);
	mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
	mbedtls_md_hmac_starts(&md, cs->mac_key, 32);
	mbedtls_md_hmac_update(&md, s, 4);
	mbedtls_md_hmac_update(&md, data, n);
	mbedtls_md_hmac_finish(&md, out);
	mbedtls_md_free(&md);
}

static int send_packet(struct conn* c, const unsigned char* payload, long len)
{
	long block = c->out.on ? 16 : 8, pad, total, enc_len;
	unsigned char* p = c->sbuf;

	if (len + 64 > RBUF_SIZE) return 0;
	if (c->out.on)
	{
		/* EtM: the encrypted part (1 + payload + padding) is a multiple of 16 */
		pad = block - ((1 + len) % block);
		if (pad < 4) pad += block;
	}
	else
	{
		pad = block - ((5 + len) % block);
		if (pad < 4) pad += block;
	}
	enc_len = 1 + len + pad;
	p[0] = (unsigned char)(enc_len >> 24); p[1] = (unsigned char)(enc_len >> 16);
	p[2] = (unsigned char)(enc_len >> 8);  p[3] = (unsigned char)enc_len;
	p[4] = (unsigned char)pad;
	memcpy(p + 5, payload, len);
	mbedtls_ctr_drbg_random(&g_drbg, p + 5 + len, pad);
	total = 4 + enc_len;
	if (c->out.on)
	{
		mbedtls_aes_crypt_ctr(&c->out.aes, enc_len, &c->out.nc_off, c->out.ctr, c->out.stream, p + 4, p + 4);
		mac_compute(&c->out, c->seq_out, p, total, p + total);
		total += 32;
	}
	c->seq_out = (c->seq_out + 1) & 0xFFFFFFFFUL;
	return net_send(c, p, total);
}

/* if a whole packet has arrived, decrypt it into c->pkt; returns the payload
   length, 0 if not complete yet, -1 on a bad packet */
static long take_packet(struct conn* c)
{
	unsigned long n;
	long need, pad, plen;
	if (c->rlen < 4) return 0;
	n = ((unsigned long)c->rbuf[0] << 24) | ((unsigned long)c->rbuf[1] << 16) |
	    ((unsigned long)c->rbuf[2] << 8) | c->rbuf[3];
	if (n < 5 || n > (unsigned long)MAX_PACKET) return -1;
	if (c->in.on && (n % 16) != 0) return -1;
	need = 4 + (long)n + (c->in.on ? 32 : 0);
	if (c->rlen < need) return 0;
	if (c->in.on)
	{
		unsigned char mac[32], diff = 0;
		int i;
		mac_compute(&c->in, c->seq_in, c->rbuf, 4 + n, mac);
		for (i = 0; i < 32; i++) diff |= mac[i] ^ c->rbuf[4 + n + i];
		if (diff) return -1;
		mbedtls_aes_crypt_ctr(&c->in.aes, n, &c->in.nc_off, c->in.ctr, c->in.stream, c->rbuf + 4, c->pkt);
	}
	else
		memcpy(c->pkt, c->rbuf + 4, n);
	pad = c->pkt[0];
	plen = (long)n - pad - 1;
	if (pad < 4 || plen < 1) return -1;
	memmove(c->pkt, c->pkt + 1, plen);
	memmove(c->rbuf, c->rbuf + need, c->rlen - need);
	c->rlen -= need;
	c->seq_in = (c->seq_in + 1) & 0xFFFFFFFFUL;
	c->packets_in++;
	return plen;
}

static void send_simple(struct conn* c, int type, unsigned long a, int with_a)
{
	unsigned char b[8];
	struct wr w;
	w.p = b; w.n = 0; w.max = sizeof b; w.bad = 0;
	w_u8(&w, (unsigned char)type);
	if (with_a) w_u32(&w, a);
	send_packet(c, b, w.n);
}

static void send_disconnect(struct conn* c, int reason, const char* why)
{
	unsigned char b[256];
	struct wr w;
	w.p = b; w.n = 0; w.max = sizeof b; w.bad = 0;
	w_u8(&w, SSH_MSG_DISCONNECT);
	w_u32(&w, reason);
	w_cstr(&w, why);
	w_cstr(&w, "");
	send_packet(c, b, w.n);
	c->alive = 0;
}

/* ------------------------------------------------------------------ */
/* key exchange                                                       */
/* ------------------------------------------------------------------ */

static void send_kexinit(struct conn* c)
{
	struct wr w;
	unsigned char cookie[16];
	w.p = c->i_s; w.n = 0; w.max = sizeof c->i_s; w.bad = 0;
	mbedtls_ctr_drbg_random(&g_drbg, cookie, 16);
	w_u8(&w, SSH_MSG_KEXINIT);
	w_bytes(&w, cookie, 16);
	w_cstr(&w, KEX_ALGS);
	w_cstr(&w, HOSTKEY_ALG);
	w_cstr(&w, CIPHER); w_cstr(&w, CIPHER);
	w_cstr(&w, MAC_ALG); w_cstr(&w, MAC_ALG);
	w_cstr(&w, "none"); w_cstr(&w, "none");
	w_cstr(&w, ""); w_cstr(&w, "");
	w_u8(&w, 0);
	w_u32(&w, 0);
	c->i_s_len = w.n;
	c->in_kex = 1;
	send_packet(c, c->i_s, c->i_s_len);
}

static int handle_kexinit(struct conn* c, const unsigned char* p, long n)
{
	struct rd r;
	const unsigned char* l;
	long ln;
	int i;

	if (n > (long)sizeof c->i_c) return 0;
	memcpy(c->i_c, p, n);
	c->i_c_len = n;
	r.p = p + 17; r.n = n - 17; r.bad = n < 17;

	l = r_str(&r, &ln);                                     /* kex */
	if (!c->first_kex_done && list_has(l, ln, "kex-strict-c-v00@openssh.com"))
	{
		c->strict = 1;
		if (c->packets_in != 1) return 0;                   /* must be the first packet */
	}
	if (!list_has(l, ln, "ecdh-sha2-nistp256")) { say("sshd: the client has no ecdh-sha2-nistp256\r\n"); return 0; }
	l = r_str(&r, &ln);
	if (!list_has(l, ln, HOSTKEY_ALG)) { say("sshd: the client won't accept an ECDSA host key\r\n"); return 0; }
	for (i = 0; i < 2; i++)
	{
		l = r_str(&r, &ln);
		if (!list_has(l, ln, CIPHER)) { say("sshd: the client has no aes128-ctr\r\n"); return 0; }
	}
	for (i = 0; i < 2; i++)
	{
		l = r_str(&r, &ln);
		if (!list_has(l, ln, MAC_ALG)) { say("sshd: the client has no hmac-sha2-256-etm\r\n"); return 0; }
	}
	for (i = 0; i < 2; i++)
	{
		l = r_str(&r, &ln);
		if (!list_has(l, ln, "none")) return 0;
	}
	r_str(&r, &ln); r_str(&r, &ln);
	if (r_u8(&r)) return 0;                                 /* first_kex_packet_follows */
	if (r.bad) return 0;
	if (!c->in_kex) send_kexinit(c);                        /* client started a re-key */
	return 1;
}

static void derive(const unsigned char* k_mp, long k_len, const unsigned char* h, char letter,
                   const unsigned char* sid, unsigned char out[32])
{
	mbedtls_sha256_context s;
	mbedtls_sha256_init(&s);
	mbedtls_sha256_starts(&s, 0);
	mbedtls_sha256_update(&s, k_mp, k_len);
	mbedtls_sha256_update(&s, h, 32);
	mbedtls_sha256_update(&s, (const unsigned char*)&letter, 1);
	mbedtls_sha256_update(&s, sid, 32);
	mbedtls_sha256_finish(&s, out);
	mbedtls_sha256_free(&s);
}

static void cipher_setup(struct cipher_state* cs, const unsigned char iv[32], const unsigned char key[32],
                         const unsigned char mac[32])
{
	mbedtls_aes_free(&cs->aes);
	mbedtls_aes_init(&cs->aes);
	mbedtls_aes_setkey_enc(&cs->aes, key, 128);
	memcpy(cs->ctr, iv, 16);
	memset(cs->stream, 0, 16);
	cs->nc_off = 0;
	memcpy(cs->mac_key, mac, 32);
	cs->on = 1;
}

static int handle_ecdh_init(struct conn* c, const unsigned char* p, long n)
{
	struct rd r;
	const unsigned char* qc;
	long qc_len;
	mbedtls_ecp_group grp;
	mbedtls_ecp_point qcp, qs;
	mbedtls_mpi d, z, hd, sr, ss;
	unsigned char qs_bin[65], zbin[32], h[32], k_mp[40], tmp[600], sig[128];
	size_t qs_len;
	long k_len;
	struct wr w;
	mbedtls_sha256_context sh;
	int ok = 0;
	unsigned char iv_c[32], iv_s[32], k_c[32], k_s[32], m_c[32], m_s[32];
	mbedtls_ecp_group hgrp;
	mbedtls_ecp_point hq;

	r.p = p + 1; r.n = n - 1; r.bad = 0;
	qc = r_str(&r, &qc_len);
	if (r.bad || qc_len != 65) return 0;

	mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&qcp); mbedtls_ecp_point_init(&qs);
	mbedtls_mpi_init(&d); mbedtls_mpi_init(&z); mbedtls_mpi_init(&hd);
	mbedtls_mpi_init(&sr); mbedtls_mpi_init(&ss);
	mbedtls_ecp_group_init(&hgrp); mbedtls_ecp_point_init(&hq);

	if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) goto out;
	if (mbedtls_ecp_point_read_binary(&grp, &qcp, qc, qc_len) != 0) goto out;
	if (mbedtls_ecp_check_pubkey(&grp, &qcp) != 0) goto out;
	if (mbedtls_ecp_gen_keypair(&grp, &d, &qs, rng, &g_drbg) != 0) goto out;
	if (mbedtls_ecdh_compute_shared(&grp, &z, &qcp, &d, rng, &g_drbg) != 0) goto out;
	if (mbedtls_ecp_point_write_binary(&grp, &qs, MBEDTLS_ECP_PF_UNCOMPRESSED, &qs_len, qs_bin, sizeof qs_bin) != 0) goto out;
	if (mbedtls_mpi_write_binary(&z, zbin, 32) != 0) goto out;

	/* K as an mpint */
	w.p = k_mp; w.n = 0; w.max = sizeof k_mp; w.bad = 0;
	w_mpint(&w, zbin, 32);
	k_len = w.n;

	/* exchange hash H */
	mbedtls_sha256_init(&sh);
	mbedtls_sha256_starts(&sh, 0);
#define HSTR(d_, n_) do { struct wr t_; t_.p = tmp; t_.n = 0; t_.max = sizeof tmp; t_.bad = 0; \
		w_u32(&t_, (n_)); mbedtls_sha256_update(&sh, tmp, 4); mbedtls_sha256_update(&sh, (d_), (n_)); } while (0)
	HSTR((const unsigned char*)c->v_c, strlen(c->v_c));
	HSTR((const unsigned char*)SERVER_VERSION, strlen(SERVER_VERSION));
	HSTR(c->i_c, c->i_c_len);
	HSTR(c->i_s, c->i_s_len);
	HSTR(g_hostkey_blob, g_hostkey_blob_len);
	HSTR(qc, qc_len);
	HSTR(qs_bin, qs_len);
	mbedtls_sha256_update(&sh, k_mp, k_len);
#undef HSTR
	mbedtls_sha256_finish(&sh, h);
	mbedtls_sha256_free(&sh);
	if (!c->have_session_id)
	{
		memcpy(c->session_id, h, 32);
		c->have_session_id = 1;
	}

	/* sign H with the host key (ECDSA over SHA-256(H)) */
	{
		unsigned char hh[32], rb[32], sb[32];
		struct wr sw;
		unsigned char inner[80];
		struct wr iw;
		mbedtls_sha256(h, 32, hh, 0);
		if (mbedtls_ecp_export(mbedtls_pk_ec(g_hostkey), &hgrp, &hd, &hq) != 0) goto out;
		if (mbedtls_ecdsa_sign(&hgrp, &sr, &ss, &hd, hh, 32, rng, &g_drbg) != 0) goto out;
		mbedtls_mpi_write_binary(&sr, rb, 32);
		mbedtls_mpi_write_binary(&ss, sb, 32);
		iw.p = inner; iw.n = 0; iw.max = sizeof inner; iw.bad = 0;
		w_mpint(&iw, rb, 32);
		w_mpint(&iw, sb, 32);
		sw.p = sig; sw.n = 0; sw.max = sizeof sig; sw.bad = 0;
		w_cstr(&sw, HOSTKEY_ALG);
		w_str(&sw, inner, iw.n);
		if (iw.bad || sw.bad) goto out;

		w.p = tmp; w.n = 0; w.max = sizeof tmp; w.bad = 0;
		w_u8(&w, SSH_MSG_KEX_ECDH_REPLY);
		w_str(&w, g_hostkey_blob, g_hostkey_blob_len);
		w_str(&w, qs_bin, qs_len);
		w_str(&w, sig, sw.n);
		if (w.bad) goto out;
		send_packet(c, tmp, w.n);
	}

	/* new keys: client to server A/C/E, server to client B/D/F */
	derive(k_mp, k_len, h, 'A', c->session_id, iv_c);
	derive(k_mp, k_len, h, 'B', c->session_id, iv_s);
	derive(k_mp, k_len, h, 'C', c->session_id, k_c);
	derive(k_mp, k_len, h, 'D', c->session_id, k_s);
	derive(k_mp, k_len, h, 'E', c->session_id, m_c);
	derive(k_mp, k_len, h, 'F', c->session_id, m_s);
	cipher_setup(&c->next_in, iv_c, k_c, m_c);
	cipher_setup(&c->next_out, iv_s, k_s, m_s);
	c->have_next = 1;

	send_simple(c, SSH_MSG_NEWKEYS, 0, 0);
	c->out = c->next_out;
	if (c->strict) c->seq_out = 0;
	ok = 1;

out:
	mbedtls_platform_zeroize(zbin, sizeof zbin);
	mbedtls_platform_zeroize(k_mp, sizeof k_mp);
	mbedtls_platform_zeroize(k_c, sizeof k_c); mbedtls_platform_zeroize(k_s, sizeof k_s);
	mbedtls_platform_zeroize(m_c, sizeof m_c); mbedtls_platform_zeroize(m_s, sizeof m_s);
	mbedtls_ecp_group_free(&grp); mbedtls_ecp_point_free(&qcp); mbedtls_ecp_point_free(&qs);
	mbedtls_mpi_free(&d); mbedtls_mpi_free(&z); mbedtls_mpi_free(&hd);
	mbedtls_mpi_free(&sr); mbedtls_mpi_free(&ss);
	mbedtls_ecp_group_free(&hgrp); mbedtls_ecp_point_free(&hq);
	return ok;
}

/* ------------------------------------------------------------------ */
/* login: public keys in ~/.ssh/authorized_keys                       */
/* ------------------------------------------------------------------ */

static int key_is_authorized(const unsigned char* blob, long blob_len)
{
	short vref;
	long sys, docs, ssh;
	FSSpec spec;
	short ref;
	long size, i, line_no = 1;
	char* text;
	int found = 0;
	CInfoPBRec pb;
	Str255 nm;

	if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &vref, &sys) != noErr) return 0;
	/* ~/.ssh = Documents:.ssh on the startup disk */
	memset(&pb, 0, sizeof pb);
	nm[0] = 9; memcpy(nm + 1, "Documents", 9);
	pb.dirInfo.ioNamePtr = nm; pb.dirInfo.ioVRefNum = vref; pb.dirInfo.ioDrDirID = fsRtDirID;
	if (PBGetCatInfoSync(&pb) != noErr) return 0;
	docs = pb.dirInfo.ioDrDirID;
	memset(&pb, 0, sizeof pb);
	nm[0] = 4; memcpy(nm + 1, ".ssh", 4);
	pb.dirInfo.ioNamePtr = nm; pb.dirInfo.ioVRefNum = vref; pb.dirInfo.ioDrDirID = docs;
	if (PBGetCatInfoSync(&pb) != noErr) return 0;
	ssh = pb.dirInfo.ioDrDirID;
	if (FSMakeFSSpec(vref, ssh, "\pauthorized_keys", &spec) != noErr) return 0;
	if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return 0;
	GetEOF(ref, &size);
	if (size > 65536) size = 65536;
	text = NewPtr(size + 1);
	if (text == NULL) { FSClose(ref); return 0; }
	FSRead(ref, &size, text);
	FSClose(ref);
	text[size] = 0;

	/* Each line is "type base64 [comment]", with the key type first.
	   A line starting with '#' is a comment. MacShell doesn't support
	   OpenSSH's options before the key type (restrict, command=, from=,
	   expiry-time=, no-pty...), so a key on such a line is refused, and the
	   log says which line. */
	i = 0;
	while (i < size && !found)
	{
		long e = i, ts, te;
		while (e < size && text[e] != '\n' && text[e] != '\r') e++;

		ts = i;
		while (ts < e && (text[ts] == ' ' || text[ts] == '\t')) ts++;
		te = ts;
		while (te < e && text[te] != ' ' && text[te] != '\t') te++;

		if (ts < e && text[ts] != '#')
		{
			int is_type = (te - ts == 19 && memcmp(text + ts, "ecdsa-sha2-nistp256", 19) == 0) ||
			              (te - ts == 7  && memcmp(text + ts, "ssh-rsa", 7) == 0);
			long b, be;
			unsigned char dec[1100];
			size_t dlen;

			b = te;
			if (!is_type)
			{
				/* an options line: find the key type further on, so a
				   matching key can be reported as refused (below) */
				long q = te;
				while (q < e)
				{
					long qe;
					while (q < e && (text[q] == ' ' || text[q] == '\t')) q++;
					qe = q;
					while (qe < e && text[qe] != ' ' && text[qe] != '\t') qe++;
					if ((qe - q == 19 && memcmp(text + q, "ecdsa-sha2-nistp256", 19) == 0) ||
					    (qe - q == 7 && memcmp(text + q, "ssh-rsa", 7) == 0))
					{
						b = qe;
						break;
					}
					q = qe;
				}
			}
			while (b < e && (text[b] == ' ' || text[b] == '\t')) b++;
			be = b;
			while (be < e && text[be] != ' ' && text[be] != '\t') be++;

			if (be > b &&
			    mbedtls_base64_decode(dec, sizeof dec, &dlen, (unsigned char*)text + b, be - b) == 0 &&
			    (long)dlen == blob_len && memcmp(dec, blob, dlen) == 0)
			{
				if (is_type)
					found = 1;
				else
					log_only("authorized_keys line %ld: this key matches but the "
					         "line has options, which MacShell does not support; "
					         "key refused", line_no);
			}
		}
		i = e + 1;
		line_no++;
	}
	DisposePtr(text);
	return found;
}

/* does sig verify (as alg) over data, for the public key in blob? */
static int verify_sig(const char* alg, const unsigned char* blob, long blob_len,
                      const unsigned char* sigb, long sig_len, const unsigned char* data, long data_len)
{
	struct rd kb, sr;
	const unsigned char* s_alg;
	const unsigned char* s;
	long s_alg_len, s_len;
	int ok = 0;

	kb.p = blob; kb.n = blob_len; kb.bad = 0;
	sr.p = sigb; sr.n = sig_len; sr.bad = 0;
	s_alg = r_str(&sr, &s_alg_len);
	s = r_str(&sr, &s_len);
	if (sr.bad || !str_is(s_alg, s_alg_len, alg)) return 0;

	if (strcmp(alg, "ecdsa-sha2-nistp256") == 0)
	{
		const unsigned char *kt, *cv, *q, *rb, *sb;
		long ktl, cvl, ql, rl, sl;
		mbedtls_ecp_group grp;
		mbedtls_ecp_point qp;
		mbedtls_mpi rr, ss;
		unsigned char hash[32];
		struct rd ir;
		kt = r_str(&kb, &ktl); cv = r_str(&kb, &cvl); q = r_str(&kb, &ql);
		if (kb.bad || !str_is(kt, ktl, alg) || !str_is(cv, cvl, "nistp256")) return 0;
		ir.p = s; ir.n = s_len; ir.bad = 0;
		rb = r_str(&ir, &rl); sb = r_str(&ir, &sl);
		if (ir.bad || rl > 33 || sl > 33) return 0;
		mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&qp);
		mbedtls_mpi_init(&rr); mbedtls_mpi_init(&ss);
		mbedtls_sha256(data, data_len, hash, 0);
		if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
		    mbedtls_ecp_point_read_binary(&grp, &qp, q, ql) == 0 &&
		    mbedtls_ecp_check_pubkey(&grp, &qp) == 0 &&
		    mbedtls_mpi_read_binary(&rr, rb, rl) == 0 &&
		    mbedtls_mpi_read_binary(&ss, sb, sl) == 0 &&
		    mbedtls_ecdsa_verify(&grp, hash, 32, &qp, &rr, &ss) == 0)
			ok = 1;
		mbedtls_ecp_group_free(&grp); mbedtls_ecp_point_free(&qp);
		mbedtls_mpi_free(&rr); mbedtls_mpi_free(&ss);
	}
	else
	{
		/* rsa-sha2-256 / rsa-sha2-512 with an ssh-rsa key */
		const unsigned char *kt, *e, *nn;
		long ktl, el, nl;
		mbedtls_rsa_context rsa;
		unsigned char hash[64], padded[1024];
		int is512 = strcmp(alg, "rsa-sha2-512") == 0;
		size_t klen;
		kt = r_str(&kb, &ktl); e = r_str(&kb, &el); nn = r_str(&kb, &nl);
		if (kb.bad || !str_is(kt, ktl, "ssh-rsa")) return 0;
		while (nl > 0 && nn[0] == 0) { nn++; nl--; }
		if (nl < 256 || nl > 1024) return 0;                /* 2048 to 8192 bits */
		mbedtls_rsa_init(&rsa);
		if (mbedtls_rsa_import_raw(&rsa, nn, nl, NULL, 0, NULL, 0, NULL, 0, e, el) == 0 &&
		    mbedtls_rsa_complete(&rsa) == 0)
		{
			klen = mbedtls_rsa_get_len(&rsa);
			if (s_len <= (long)klen && klen <= sizeof padded)
			{
				memset(padded, 0, klen - s_len);
				memcpy(padded + klen - s_len, s, s_len);
				if (is512) mbedtls_sha512(data, data_len, hash, 0);
				else mbedtls_sha256(data, data_len, hash, 0);
				if (mbedtls_rsa_pkcs1_verify(&rsa, is512 ? MBEDTLS_MD_SHA512 : MBEDTLS_MD_SHA256,
				                             is512 ? 64 : 32, hash, padded) == 0)
					ok = 1;
			}
		}
		mbedtls_rsa_free(&rsa);
	}
	return ok;
}

static void send_auth_failure(struct conn* c)
{
	unsigned char b[32];
	struct wr w;
	w.p = b; w.n = 0; w.max = sizeof b; w.bad = 0;
	w_u8(&w, SSH_MSG_USERAUTH_FAILURE);
	w_cstr(&w, "publickey");
	w_u8(&w, 0);
	send_packet(c, b, w.n);
	if (++c->auth_failures >= 10)
	{
		log_only("%s: too many rejected keys, disconnected", c->peer);
		send_disconnect(c, 14, "too many authentication failures");
	}
}

static void handle_userauth(struct conn* c, const unsigned char* p, long n)
{
	struct rd r;
	const unsigned char *user, *service, *method, *alg, *blob, *sig;
	long ul, sl, ml, al, bl, sgl;
	int has_sig;
	char algs[24];

	r.p = p + 1; r.n = n - 1; r.bad = 0;
	user = r_str(&r, &ul);
	service = r_str(&r, &sl);
	method = r_str(&r, &ml);
	if (r.bad || !str_is(service, sl, "ssh-connection") || ul >= (long)sizeof c->user)
	{
		send_auth_failure(c);
		return;
	}
	if (!str_is(method, ml, "publickey"))
	{
		send_auth_failure(c);                               /* "none" etc.: tell it to use keys */
		return;
	}
	has_sig = r_u8(&r);
	alg = r_str(&r, &al);
	blob = r_str(&r, &bl);
	if (r.bad || al >= (long)sizeof algs) { send_auth_failure(c); return; }
	memcpy(algs, alg, al);
	algs[al] = 0;
	if (strcmp(algs, "ecdsa-sha2-nistp256") && strcmp(algs, "rsa-sha2-256") && strcmp(algs, "rsa-sha2-512"))
	{
		send_auth_failure(c);
		return;
	}
	if (!key_is_authorized(blob, bl))
	{
		char fp[64], u[64];
		long n2 = ul < (long)sizeof u - 1 ? ul : (long)sizeof u - 1;
		memcpy(u, user, n2);
		u[n2] = 0;
		blob_fingerprint(blob, bl, fp, sizeof fp);
		log_only("rejected key for %s from %s: %s %s (not in authorized_keys)", u, c->peer, algs, fp);
		send_auth_failure(c);
		return;
	}
	if (!has_sig)
	{
		/* the client asks whether this key would be accepted */
		unsigned char b[1200];
		struct wr w;
		w.p = b; w.n = 0; w.max = sizeof b; w.bad = 0;
		w_u8(&w, SSH_MSG_USERAUTH_PK_OK);
		w_str(&w, alg, al);
		w_str(&w, blob, bl);
		if (!w.bad) send_packet(c, b, w.n); else send_auth_failure(c);
		return;
	}
	sig = r_str(&r, &sgl);
	if (r.bad) { send_auth_failure(c); return; }
	{
		/* signed data: session id, then the request up to the signature */
		long req_len = (long)(sig - p) - 4;
		unsigned char* data = NewPtr(4 + 32 + req_len);
		struct wr w;
		int ok;
		if (data == NULL) { send_auth_failure(c); return; }
		w.p = data; w.n = 0; w.max = 4 + 32 + req_len; w.bad = 0;
		w_str(&w, c->session_id, 32);
		w_bytes(&w, p, req_len);
		ok = !w.bad && verify_sig(algs, blob, bl, sig, sgl, data, w.n);
		DisposePtr((Ptr)data);
		if (!ok)
		{
			log_only("bad signature from %s (%s key)", c->peer, algs);
			send_auth_failure(c);
			return;
		}
	}
	memcpy(c->user, user, ul);
	c->user[ul] = 0;
	c->authed = 1;
	send_simple(c, SSH_MSG_USERAUTH_SUCCESS, 0, 0);
	{
		char fp[64];
		blob_fingerprint(blob, bl, fp, sizeof fp);
		say("sshd: %s logged in from %s with an %s key\r\n", c->user, c->peer,
		    algs[0] == 'e' ? "ECDSA" : "RSA");
		log_only("  key %s", fp);
	}
}

/* ------------------------------------------------------------------ */
/* channel                                                            */
/* ------------------------------------------------------------------ */

static void chan_reply(struct conn* c, int ok)
{
	send_simple(c, ok ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE, c->peer_chan, 1);
}

static void handle_channel_open(struct conn* c, const unsigned char* p, long n)
{
	struct rd r;
	const unsigned char* type;
	long tl;
	unsigned long sender, window, maxpkt;
	unsigned char b[64];
	struct wr w;

	r.p = p + 1; r.n = n - 1; r.bad = 0;
	type = r_str(&r, &tl);
	sender = r_u32(&r); window = r_u32(&r); maxpkt = r_u32(&r);
	w.p = b; w.n = 0; w.max = sizeof b; w.bad = 0;
	if (r.bad || !str_is(type, tl, "session") || c->chan_open)
	{
		w_u8(&w, SSH_MSG_CHANNEL_OPEN_FAILURE);
		w_u32(&w, sender);
		w_u32(&w, 1);                                       /* administratively prohibited */
		w_cstr(&w, "only one session channel");
		w_cstr(&w, "");
		send_packet(c, b, w.n);
		return;
	}
	c->chan_open = 1;
	c->peer_chan = sender;
	c->peer_window = window;
	c->peer_maxpkt = maxpkt < 512 ? 512 : maxpkt;
	c->our_window = OUR_WINDOW;
	w_u8(&w, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
	w_u32(&w, sender);
	w_u32(&w, 0);
	w_u32(&w, OUR_WINDOW);
	w_u32(&w, OUR_MAXPKT);
	send_packet(c, b, w.n);
}

static void handle_channel_request(struct conn* c, const unsigned char* p, long n)
{
	struct rd r;
	const unsigned char* type;
	long tl;
	int want_reply;

	r.p = p + 1; r.n = n - 1; r.bad = 0;
	r_u32(&r);
	type = r_str(&r, &tl);
	want_reply = r_u8(&r);
	if (r.bad) return;

	if (str_is(type, tl, "pty-req") || str_is(type, tl, "env") || str_is(type, tl, "window-change"))
	{
		if (want_reply) chan_reply(c, 1);
	}
	else if ((str_is(type, tl, "shell") || str_is(type, tl, "exec")) && !c->want_session)
	{
		if (str_is(type, tl, "exec"))
		{
			long cl;
			const unsigned char* cmd = r_str(&r, &cl);
			if (r.bad || cl >= (long)sizeof c->exec_cmd) { if (want_reply) chan_reply(c, 0); return; }
			memcpy(c->exec_cmd, cmd, cl);
			c->exec_cmd[cl] = 0;
			c->want_session = 2;
			say("sshd: %s runs: %s\r\n", c->user, c->exec_cmd);
		}
		else
			c->want_session = 1;
		if (want_reply) chan_reply(c, 1);
	}
	else if (want_reply)
		chan_reply(c, 0);
}

/* send queued shell output as channel data */
static void flush_output(struct conn* c)
{
	while (c->chan_open && !c->close_sent && c->outq_len > 0 && c->peer_window > 0 && c->alive)
	{
		unsigned char* b = c->obuf;
		long n = c->outq_len;
		struct wr w;
		if (n > (long)c->peer_window) n = c->peer_window;
		if (n > (long)c->peer_maxpkt - 64) n = c->peer_maxpkt - 64;
		if (n > (long)OUR_MAXPKT) n = OUR_MAXPKT;
		if (n <= 0) break;
		w.p = b; w.n = 0; w.max = sizeof c->obuf; w.bad = 0;
		w_u8(&w, SSH_MSG_CHANNEL_DATA);
		w_u32(&w, c->peer_chan);
		w_str(&w, c->outq, n);
		send_packet(c, b, w.n);
		c->peer_window -= n;
		memmove(c->outq, c->outq + n, c->outq_len - n);
		c->outq_len -= n;
	}
}

static void close_channel(struct conn* c, int exit_status)
{
	unsigned char b[64];
	struct wr w;
	if (!c->chan_open || c->close_sent) return;
	flush_output(c);
	w.p = b; w.n = 0; w.max = sizeof b; w.bad = 0;
	w_u8(&w, SSH_MSG_CHANNEL_REQUEST);
	w_u32(&w, c->peer_chan);
	w_cstr(&w, "exit-status");
	w_u8(&w, 0);
	w_u32(&w, exit_status);
	send_packet(c, b, w.n);
	if (!c->eof_sent) { send_simple(c, SSH_MSG_CHANNEL_EOF, c->peer_chan, 1); c->eof_sent = 1; }
	send_simple(c, SSH_MSG_CHANNEL_CLOSE, c->peer_chan, 1);
	c->close_sent = 1;
}

/* ------------------------------------------------------------------ */
/* message dispatch                                                   */
/* ------------------------------------------------------------------ */

static void handle_packet(struct conn* c, const unsigned char* p, long n)
{
	int type = p[0];
	struct rd r;

	/* strict key exchange: nothing else is allowed during the first one */
	if (c->strict && !c->first_kex_done &&
	    type != SSH_MSG_KEXINIT && type != SSH_MSG_KEX_ECDH_INIT && type != SSH_MSG_NEWKEYS)
	{
		send_disconnect(c, 3, "unexpected message during key exchange");
		return;
	}

	switch (type)
	{
	case SSH_MSG_KEXINIT:
		if (!handle_kexinit(c, p, n)) { log_only("%s: key exchange failed", c->peer); send_disconnect(c, 3, "key exchange failed"); }
		break;
	case SSH_MSG_KEX_ECDH_INIT:
		if (!c->in_kex || !handle_ecdh_init(c, p, n)) { log_only("%s: key exchange failed", c->peer); send_disconnect(c, 3, "key exchange failed"); }
		break;
	case SSH_MSG_NEWKEYS:
		if (!c->have_next) { send_disconnect(c, 2, "unexpected NEWKEYS"); break; }
		c->in = c->next_in;
		c->have_next = 0;
		c->in_kex = 0;
		if (c->strict) c->seq_in = 0;
		c->first_kex_done = 1;
		break;
	case SSH_MSG_SERVICE_REQUEST:
	{
		const unsigned char* s;
		long sl;
		r.p = p + 1; r.n = n - 1; r.bad = 0;
		s = r_str(&r, &sl);
		if (!r.bad && str_is(s, sl, "ssh-userauth") && c->first_kex_done)
		{
			unsigned char b[32];
			struct wr w;
			w.p = b; w.n = 0; w.max = sizeof b; w.bad = 0;
			w_u8(&w, SSH_MSG_SERVICE_ACCEPT);
			w_cstr(&w, "ssh-userauth");
			send_packet(c, b, w.n);
		}
		else
			send_disconnect(c, 7, "service not available");
		break;
	}
	case SSH_MSG_USERAUTH_REQUEST:
		if (!c->first_kex_done) { send_disconnect(c, 2, "not ready"); break; }
		if (!c->authed) handle_userauth(c, p, n);
		break;
	case SSH_MSG_GLOBAL_REQUEST:
	{
		long tl;
		r.p = p + 1; r.n = n - 1; r.bad = 0;
		r_str(&r, &tl);
		if (r_u8(&r) && !r.bad) send_simple(c, SSH_MSG_REQUEST_FAILURE, 0, 0);
		break;
	}
	case SSH_MSG_CHANNEL_OPEN:
		if (c->authed) handle_channel_open(c, p, n);
		else send_disconnect(c, 2, "not logged in");
		break;
	case SSH_MSG_CHANNEL_REQUEST:
		if (c->authed && c->chan_open) handle_channel_request(c, p, n);
		break;
	case SSH_MSG_CHANNEL_WINDOW_ADJUST:
		r.p = p + 1; r.n = n - 1; r.bad = 0;
		r_u32(&r);
		{
			unsigned long add = r_u32(&r);
			if (!r.bad && c->peer_window + add >= c->peer_window) c->peer_window += add;
		}
		break;
	case SSH_MSG_CHANNEL_DATA:
	{
		const unsigned char* d;
		long dl;
		r.p = p + 1; r.n = n - 1; r.bad = 0;
		r_u32(&r);
		d = r_str(&r, &dl);
		if (r.bad || !c->authed || !c->chan_open) break;
		if ((unsigned long)dl > c->our_window) { send_disconnect(c, 2, "window exceeded"); break; }
		c->our_window -= dl;
		if (dl > INQ_SIZE - c->inq_len) dl = INQ_SIZE - c->inq_len;   /* typing that fast is unlikely */
		memcpy(c->inq + c->inq_len, d, dl);
		c->inq_len += dl;
		if (c->our_window < OUR_WINDOW / 2)
		{
			unsigned char b[16];
			struct wr w;
			w.p = b; w.n = 0; w.max = sizeof b; w.bad = 0;
			w_u8(&w, SSH_MSG_CHANNEL_WINDOW_ADJUST);
			w_u32(&w, c->peer_chan);
			w_u32(&w, OUR_WINDOW - c->our_window);
			send_packet(c, b, w.n);
			c->our_window = OUR_WINDOW;
		}
		break;
	}
	case SSH_MSG_CHANNEL_EOF:
		break;
	case SSH_MSG_CHANNEL_CLOSE:
		close_channel(c, 0);
		c->alive = 0;
		break;
	case SSH_MSG_DISCONNECT:
		c->alive = 0;
		break;
	case SSH_MSG_IGNORE:
	case SSH_MSG_DEBUG:
	case SSH_MSG_UNIMPLEMENTED:
	case SSH_MSG_EXT_INFO:
		break;
	default:
		send_simple(c, SSH_MSG_UNIMPLEMENTED, (c->seq_in - 1) & 0xFFFFFFFFUL, 1);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* one connection                                                     */
/* ------------------------------------------------------------------ */

static void run_connection(struct conn* c)
{
	unsigned long start = TickCount();
	char line[300];
	long ll = 0;

	/* version exchange */
	{
		char v[40];
		snprintf(v, sizeof v, "%s\r\n", SERVER_VERSION);
		if (!net_send(c, (const unsigned char*)v, strlen(v))) return;
	}
	while (c->alive && !g_stop)
	{
		long i;
		int done = 0;
		if (!net_poll(c)) return;
		for (i = 0; i < c->rlen; i++)
		{
			if (c->rbuf[i] == '\n')
			{
				ll = i;
				if (ll > 0 && c->rbuf[ll - 1] == '\r') ll--;
				if (ll >= (long)sizeof line) ll = sizeof line - 1;
				memcpy(line, c->rbuf, ll);
				line[ll] = 0;
				memmove(c->rbuf, c->rbuf + i + 1, c->rlen - i - 1);
				c->rlen -= i + 1;
				done = 1;
				break;
			}
		}
		if (done)
		{
			if (strncmp(line, "SSH-2.0-", 8) == 0) break;
			continue;                                       /* skip anything else */
		}
		/* every SSH client sends its version line straight away */
		if (c->rlen > 1000 || TickCount() - start > 60 * 10)
		{
			log_only("%s: no SSH version line within 10 seconds, disconnected", c->peer);
			return;
		}
		YieldToAnyThread();
	}
	if (!c->alive || g_stop) return;
	strncpy(c->v_c, line, sizeof c->v_c - 1);
	c->v_c[sizeof c->v_c - 1] = 0;

	send_kexinit(c);

	while (c->alive && !g_stop)
	{
		long n;
		int idle = 1;

		/* not logged in within two minutes: give up */
		if (!c->authed && TickCount() - start > 60L * 120)
		{
			log_only("%s: no login within two minutes, disconnected", c->peer);
			send_disconnect(c, 2, "login timed out");
			break;
		}
		if (!net_poll(c)) break;
		while ((n = take_packet(c)) != 0)
		{
			if (n < 0) { log_only("%s: bad packet, disconnected", c->peer); send_disconnect(c, 2, "bad packet"); break; }
			handle_packet(c, c->pkt, n);
			idle = 0;
			if (!c->alive) break;
		}
		if (!c->alive) break;

		if (c->outq_len > 0) { flush_output(c); idle = 0; }

		/* the shell finished, or the tab couldn't be opened */
		if (c->chan_open && !c->close_sent &&
		    (c->exec_done || c->shell_closed || c->session_failed) && c->outq_len == 0)
		{
			close_channel(c, c->session_failed ? 1 : c->exit_status);
			idle = 0;
		}
		if (idle) YieldToAnyThread();
	}
}

/* ------------------------------------------------------------------ */
/* server thread                                                      */
/* ------------------------------------------------------------------ */

static void* sshd_thread(void* arg)
{
	EndpointRef listener;
	OSStatus err;
	InetAddress addr;
	TBind req, ret;
	InetAddress bound;
	(void)arg;

	listener = OTOpenEndpoint(OTCreateConfiguration("tilisten, tcp"), 0, nil, &err);
	if (err != noErr)
	{
		say("sshd: cannot open a listening endpoint (err=%d)\r\n", (int)err);
		goto out;
	}
	/* reuse the port at once after a crash or quick restart (otherwise
	   Open Transport keeps it reserved for a minute or so) */
	{
		TOptMgmt oreq, oret;
		UInt8 obuf[kOTFourByteOptionSize];
		TOption* opt = (TOption*)obuf;
		opt->len = kOTFourByteOptionSize;
		opt->level = INET_IP;
		opt->name = kIP_REUSEADDR;
		opt->status = 0;
		opt->value[0] = 1;
		oreq.opt.buf = obuf; oreq.opt.len = sizeof obuf; oreq.flags = T_NEGOTIATE;
		oret.opt.buf = obuf; oret.opt.maxlen = sizeof obuf; oret.flags = 0;
		OTSetSynchronous(listener);
		OTSetBlocking(listener);
		OTOptionManagement(listener, &oreq, &oret);
	}

	OTInitInetAddress(&addr, g_port, 0);
	OTMemzero(&req, sizeof req);
	req.addr.buf = (UInt8*)&addr;
	req.addr.len = sizeof addr;
	req.qlen = 1;
	OTMemzero(&ret, sizeof ret);
	ret.addr.buf = (UInt8*)&bound;
	ret.addr.maxlen = sizeof bound;
	err = OTBind(listener, &req, &ret);
	if (err != noErr || bound.fPort != g_port)
	{
		say("sshd: port %d is not available (err=%d)\r\n", (int)g_port, (int)err);
		OTCloseProvider(listener);
		goto out;
	}
	OTSetNonBlocking(listener);
	OTSetSynchronous(listener);
	say("sshd: listening on port %d\r\n", (int)g_port);

	while (!g_stop)
	{
		TCall call;
		InetAddress peer;
		EndpointRef ep;
		struct conn* c;

		OTMemzero(&call, sizeof call);
		call.addr.buf = (UInt8*)&peer;
		call.addr.maxlen = sizeof peer;
		err = OTListen(listener, &call);
		if (err == kOTNoDataErr) { YieldToAnyThread(); continue; }
		if (err == kOTLookErr) { OTLook(listener); YieldToAnyThread(); continue; }
		if (err != noErr) { YieldToAnyThread(); continue; }

		ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, nil, &err);
		if (err != noErr) { OTSndDisconnect(listener, &call); continue; }
		OTSetSynchronous(ep);
		OTSetBlocking(ep);
		err = OTAccept(listener, ep, &call);
		if (err != noErr) { OTCloseProvider(ep); continue; }
		OTSetNonBlocking(ep);

		c = (struct conn*)NewPtrClear(sizeof(struct conn));
		if (c == NULL) { OTSndDisconnect(ep, NULL); OTCloseProvider(ep); continue; }
		c->ep = ep;
		c->alive = 1;
		c->session_idx = -1;
		OTInetHostToString(peer.fHost, c->peer);
		g_connections++;
		say("sshd: connection from %s\r\n", c->peer);
		g_conn = c;

		run_connection(c);

		/* the main thread closes the tab when it sees the connection gone */
		c->alive = 0;
		OTSndOrderlyDisconnect(ep);
		OTUnbind(ep);
		OTCloseProvider(ep);
		while (c->session_idx >= 0 && !g_stop)
			YieldToAnyThread();
		say("sshd: %s disconnected\r\n", c->peer);
		g_conn = NULL;
		mbedtls_aes_free(&c->in.aes); mbedtls_aes_free(&c->out.aes);
		mbedtls_aes_free(&c->next_in.aes); mbedtls_aes_free(&c->next_out.aes);
		mbedtls_platform_zeroize(c, sizeof(struct conn));
		DisposePtr((Ptr)c);
	}

	OTUnbind(listener);
	OTCloseProvider(listener);
	say("sshd: stopped\r\n");
out:
	g_running = 0;
	g_thread = kNoThreadID;
	return 0;
}

/* ------------------------------------------------------------------ */
/* commands                                                           */
/* ------------------------------------------------------------------ */

int sshd_running(void) { return g_running; }

int sshd_start(int idx, unsigned short port)
{
	static int crypto_ready = 0;
	char fp[64];
	OSErr err;

	if (g_running)
	{
		printf_s(idx, "sshd: already running on port %d\r\n", (int)g_port);
		return 0;
	}
	if (!macentropy_get_stats()->ready)
	{
		printf_s(idx, "sshd: not enough randomness yet; type 'entropy' for details\r\n");
		return 0;
	}
	if (InitOpenTransport() != noErr)
	{
		printf_s(idx, "sshd: Open Transport not available\r\n");
		return 0;
	}
	if (!crypto_ready)
	{
		mbedtls_entropy_init(&g_entropy);
		mbedtls_ctr_drbg_init(&g_drbg);
		mbedtls_pk_init(&g_hostkey);
		if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
		                          (const unsigned char*)"MacShell sshd", 13) != 0)
		{
			printf_s(idx, "sshd: cannot start the random generator\r\n");
			return 0;
		}
		crypto_ready = 1;
	}
	if (g_hostkey_blob_len == 0 && !hostkey_load_or_make(idx))
		return 0;

	g_port = port;
	{
		/* the server gets its own tab for its messages */
		int log_idx = new_session(&windows[active_window], SESSION_LOCAL);
		if (log_idx >= 0)
		{
			snprintf(sessions[log_idx].tab_label, sizeof(sessions[log_idx].tab_label), "sshd");
			printf_s(idx, "sshd: started in its own tab (close that tab to stop it)\r\n");
			idx = log_idx;
		}
	}
	g_msg_idx = idx;
	g_stop = 0;
	g_running = 1;
	err = NewThread(kCooperativeThread, sshd_thread, NULL, 96L * 1024, kCreateIfNeeded, NULL, &g_thread);
	if (err != noErr)
	{
		g_running = 0;
		printf_s(idx, "sshd: cannot start (err=%d)\r\n", (int)err);
		return 0;
	}
	hostkey_fingerprint(fp, sizeof fp);
	printf_s(idx, "sshd: host key ECDSA %s\r\n", fp);
	printf_s(idx, "sshd: keys allowed from ~/.ssh/authorized_keys; no passwords\r\n");
	printf_s(idx, "sshd: log in ~/.sshd/logs/sshd.log\r\n");
	log_only("starting on port %d, host key ECDSA %s", (int)port, fp);
	return 1;
}

void sshd_stop(int idx)
{
	if (!g_running)
	{
		printf_s(idx, "sshd: not running\r\n");
		return;
	}
	if (g_msg_idx < 0 || !sessions[g_msg_idx].in_use) g_msg_idx = idx;
	g_stop = 1;
	if (g_conn) g_conn->alive = 0;
	printf_s(idx, "sshd: stopping\r\n");
}

void sshd_status(int idx)
{
	char fp[64];
	if (!g_running)
	{
		printf_s(idx, "sshd: not running (start it with 'sshd start')\r\n");
		return;
	}
	hostkey_fingerprint(fp, sizeof fp);
	printf_s(idx, "sshd: listening on port %d, %ld connection(s) so far\r\n", (int)g_port, g_connections);
	printf_s(idx, "sshd: host key ECDSA %s\r\n", fp);
	{
		/* the log's size (sshd.log plus the rolled-over sshd.log.1) */
		short vref;
		long sys, docs, sd, logs, total = 0;
		if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &vref, &sys) == noErr &&
		    log_folder(vref, fsRtDirID, "Documents", &docs) && log_folder(vref, docs, ".sshd", &sd) &&
		    log_folder(vref, sd, "logs", &logs))
		{
			const unsigned char* names[2];
			int i;
			names[0] = "\psshd.log";
			names[1] = "\psshd.log.1";
			for (i = 0; i < 2; i++)
			{
				CInfoPBRec pb;
				memset(&pb, 0, sizeof pb);
				pb.hFileInfo.ioNamePtr = (StringPtr)names[i];
				pb.hFileInfo.ioVRefNum = vref;
				pb.hFileInfo.ioDirID = logs;
				if (PBGetCatInfoSync(&pb) == noErr) total += pb.hFileInfo.ioFlLgLen;
			}
		}
		printf_s(idx, "sshd: log ~/.sshd/logs/sshd.log, %ld KB of at most 512 KB%s\r\n",
		         total / 1024, g_log_paused ? " (paused: disk almost full)" : "");
	}
	if (g_conn)
		printf_s(idx, "sshd: %s connected%s%s\r\n", g_conn->peer,
		         g_conn->authed ? " as " : " (logging in)", g_conn->authed ? g_conn->user : "");
}

/* ------------------------------------------------------------------ */
/* main-thread side                                                   */
/* ------------------------------------------------------------------ */

void sshd_claim_session(int idx)
{
	if (!g_claim_pending || g_conn == NULL) return;
	sessions[idx].sshd_owned = 1;
	sessions[idx].sshd_tap = (g_conn->want_session == 1);   /* exec: quiet until it runs */
	g_conn->session_idx = idx;
}

void sshd_session_closing(int idx)
{
	/* closing the sshd tab stops the server */
	if (idx == g_msg_idx)
	{
		g_msg_idx = -1;
		if (g_running)
		{
			g_stop = 1;
			if (g_conn) g_conn->alive = 0;
		}
	}
	if (!sessions[idx].sshd_owned) return;
	sessions[idx].sshd_owned = 0;
	sessions[idx].sshd_tap = 0;
	if (g_conn && g_conn->session_idx == idx)
	{
		g_conn->shell_closed = 1;
		g_conn->session_idx = -1;
	}
}

void sshd_output(int idx, const char* data, long len)
{
	struct conn* c = g_conn;
	if (c == NULL || c->session_idx != idx || !c->alive) return;
	while (len > 0 && c->alive)
	{
		long room = OUTQ_SIZE - c->outq_len, n;
		if (room <= 0)
		{
			YieldToAnyThread();                             /* let the server thread send */
			if (g_conn != c) return;
			continue;
		}
		n = len < room ? len : room;
		memcpy(c->outq + c->outq_len, data, n);
		c->outq_len += n;
		data += n;
		len -= n;
	}
}

/* turn bytes from a remote terminal into MacShell key presses */
static void feed_key(int idx, struct conn* c, unsigned char b)
{
	if (c->esc_state == 1)
	{
		if (b == '[' || b == 'O') { c->esc_state = 2; c->esc_len = 0; return; }
		c->esc_state = 0;
		shell_input(idx, 0x1B, 0, 0x35);
		/* fall through to handle b normally */
	}
	else if (c->esc_state == 2)
	{
		if (b >= '0' && b <= '9')
		{
			if (c->esc_len < 7) c->esc_num[c->esc_len++] = b;
			return;
		}
		c->esc_num[c->esc_len] = 0;
		c->esc_state = 0;
		switch (b)
		{
		case 'A': shell_input(idx, 0x1E, 0, 0x7E); break;   /* up */
		case 'B': shell_input(idx, 0x1F, 0, 0x7D); break;   /* down */
		case 'C': shell_input(idx, 0x1D, 0, 0x7C); break;   /* right */
		case 'D': shell_input(idx, 0x1C, 0, 0x7B); break;   /* left */
		case 'H': shell_input(idx, 0x01, 0, 0x73); break;   /* home */
		case 'F': shell_input(idx, 0x04, 0, 0x77); break;   /* end */
		case '~':
			if (!strcmp(c->esc_num, "3")) shell_input(idx, 0x7F, 0, 0x75);
			else if (!strcmp(c->esc_num, "1") || !strcmp(c->esc_num, "7")) shell_input(idx, 0x01, 0, 0x73);
			else if (!strcmp(c->esc_num, "4") || !strcmp(c->esc_num, "8")) shell_input(idx, 0x04, 0, 0x77);
			break;
		}
		return;
	}
	if (b == 0x1B) { c->esc_state = 1; return; }
	if (b == '\r') shell_input(idx, 0x0D, 0, 0x24);
	else if (b == '\n') { /* the \r already pressed Return */ }
	else if (b == 0x7F || b == 0x08) shell_input(idx, 0x08, 0, 0x33);
	else if (b == 0x03) shell_input(idx, 0x03, controlKey, 0x08);
	else if (b == 0x04) shell_input(idx, 0x04, controlKey, 0x02);
	else if (b == '\t') shell_input(idx, 0x09, 0, 0x30);
	else if (b >= 32 && b < 127) shell_input(idx, b, 0, 0);
}

void sshd_tick(void)
{
	struct conn* c = g_conn;
	int idx;
	long i;

	if (c == NULL) return;

	/* the connection ended: close its tab */
	if (!c->alive)
	{
		if (c->session_idx >= 0)
		{
			idx = c->session_idx;
			if (sessions[idx].in_use && sessions[idx].hidden)
				close_hidden_session(idx);
			else if (sessions[idx].in_use && sessions[idx].sshd_owned)
				close_session(idx);
			if (sessions[idx].in_use)                       /* last tab in its window */
			{
				sessions[idx].sshd_owned = 0;
				sessions[idx].sshd_tap = 0;
			}
			c->session_idx = -1;
		}
		return;
	}

	/* open a tab for a shell or command */
	if (c->want_session && c->session_idx < 0 && !c->session_failed && !c->shell_closed && !c->exec_done)
	{
		struct window_context* wc = &windows[active_window];
		g_claim_pending = 1;
		if (c->want_session == 2)
			idx = new_hidden_session();                 /* ssh mac 'command': no tab */
		else
		{
			new_session_background = 1;                 /* don't take over the user's tab */
			idx = new_session(wc, SESSION_LOCAL);
			new_session_background = 0;
		}
		g_claim_pending = 0;
		if (idx < 0 || c->session_idx != idx)
		{
			c->session_failed = 1;
			return;
		}
		snprintf(sessions[idx].tab_label, sizeof(sessions[idx].tab_label), "ssh %s", c->peer);
		if (c->want_session == 2)
		{
			sessions[idx].sshd_tap = 1;
			sessions[idx].sshd_exec = 1;
			shell_run_line(idx, c->exec_cmd);
			c->exec_started = 1;
		}
	}
	if (c->session_idx < 0) return;
	idx = c->session_idx;

	/* typed input */
	if (c->inq_len > 0)
	{
		unsigned char buf[INQ_SIZE];
		long n = c->inq_len;
		memcpy(buf, c->inq, n);
		c->inq_len = 0;
		for (i = 0; i < n && c->session_idx == idx; i++)
			feed_key(idx, c, buf[i]);
	}

	/* a command finished (including background work like wget or ping) */
	if (c->exec_started && !c->exec_done && c->session_idx == idx && !shell_is_busy(idx))
	{
		c->exit_status = sessions[idx].last_status;
		log_only("  finished, exit status %d", c->exit_status);
		c->exec_done = 1;
		close_hidden_session(idx);                      /* its output is already queued */
	}
}
