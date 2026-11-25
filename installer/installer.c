/*
 * MacShell Installer
 *
 * 1. Shows the licenses (MacShell's and those of the software in it); the
 *    user must click Agree to continue.
 * 2. Installs MacShell (one application with both 68k and PowerPC code)
 *    in a "MacShell" folder on the chosen disk.
 * 3. Optionally sets up ~/.ssh: Documents:.ssh on the startup disk, with an
 *    authorized_keys file for MacShell's SSH server. Existing files are
 *    never replaced.
 * 4. Optionally gathers randomness now and saves MacShell's seed file, so
 *    MacShell's first start (and first secure connection) is quick.
 *
 * MacShell travels inside this application as resources, written out in
 * pieces (it is too big to load at once on small Macs):
 *   'MSdk' 128 = MacShell's data fork (PowerPC code)
 *   'MSfk' 128 = MacShell's resource fork (68k code and resources)
 *   'TEXT' 128 = the licenses
 */
#include <Controls.h>
#include <ControlDefinitions.h>
#include <Dialogs.h>
#include <Events.h>
#include <Files.h>
#include <Folders.h>
#include <Fonts.h>
#include <Gestalt.h>
#include <Memory.h>
#include <Menus.h>
#include <Quickdraw.h>
#include <Resources.h>
#include <Script.h>
#include <TextEdit.h>
#include <TextUtils.h>
#include <ToolUtils.h>
#include <Windows.h>
#include <string.h>

#include "macentropy.h"

enum {
    kMainDialog   = 128,
    kInstallBtn   = 1,
    kQuitBtn      = 2,
    kDiskPopup    = 3,
    kStatusText   = 4,
    kSSHBox       = 5,
    kSeedBox      = 6,
    kMaxDisks     = 20,
    kAlertID      = 129,
    kLicenseText  = 128
};

static short gDisks[kMaxDisks];
static short gDiskCount;

/* ------------------------------------------------------------------ */
/* Pascal string helpers                                              */
/* ------------------------------------------------------------------ */

static void PCopyC(StringPtr p, const char *c)
{
    size_t n = strlen(c);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, n);
}

static void PAppendC(StringPtr p, const char *c)
{
    size_t n = strlen(c);
    if (p[0] + n > 255) n = 255 - p[0];
    memcpy(p + 1 + p[0], c, n);
    p[0] += (unsigned char)n;
}

static void PAppend(StringPtr p, ConstStringPtr q)
{
    short n = q[0];
    if (p[0] + n > 255) n = 255 - p[0];
    memcpy(p + 1 + p[0], q + 1, n);
    p[0] += (unsigned char)n;
}

static void PAppendNum(StringPtr p, long n)
{
    Str255 s;
    NumToString(n, s);
    PAppend(p, s);
}

static void Fail(const char *message)
{
    Str255 s;
    PCopyC(s, message);
    ParamText(s, "\p", "\p", "\p");
    StopAlert(kAlertID, NULL);
}

/* ------------------------------------------------------------------ */
/* the license window                                                 */
/* ------------------------------------------------------------------ */

static TEHandle gTE;
static ControlHandle gScroll;
static short gLineHeight;

static void ScrollTo(short value)
{
    short old = GetControlValue(gScroll);
    if (value < 0) value = 0;
    if (value > GetControlMaximum(gScroll)) value = GetControlMaximum(gScroll);
    SetControlValue(gScroll, value);
    TEScroll(0, (old - value) * gLineHeight, gTE);
}

static pascal void ScrollAction(ControlHandle c, short part)
{
    short page = ((**gTE).viewRect.bottom - (**gTE).viewRect.top) / gLineHeight - 1;
    short v = GetControlValue(c);
    switch (part) {
    case kControlUpButtonPart:   ScrollTo(v - 1); break;
    case kControlDownButtonPart: ScrollTo(v + 1); break;
    case kControlPageUpPart:     ScrollTo(v - page); break;
    case kControlPageDownPart:   ScrollTo(v + page); break;
    }
}

/* Shows the licenses; returns true if the user clicks Agree. */
static Boolean LicenseAgreed(void)
{
    WindowPtr w;
    Rect r, view, text;
    ControlHandle agree, disagree;
    ControlActionUPP action = NewControlActionUPP(ScrollAction);
    Handle h = GetResource('TEXT', kLicenseText);
    FontInfo fi;
    short lines, visible, result = -1;
    EventRecord ev;

    if (!h) return true;                               /* nothing to show */
    SetRect(&r, 0, 0, 500, 360);
    OffsetRect(&r, (qd.screenBits.bounds.right - 500) / 2, 50);
    w = NewWindow(NULL, &r, "\pMacShell License Agreement", true, movableDBoxProc,
                  (WindowPtr)-1, false, 0);
    SetPort(w);

    SetRect(&view, 16, 40, 468, 310);
    text = view;
    InsetRect(&text, 4, 4);
    TextFont(kFontIDGeneva);
    TextSize(9);
    GetFontInfo(&fi);
    gLineHeight = fi.ascent + fi.descent + fi.leading;
    gTE = TENew(&text, &text);
    HLock(h);
    TESetText(*h, GetHandleSize(h), gTE);
    HUnlock(h);
    TECalText(gTE);
    lines = (**gTE).nLines;
    visible = (text.bottom - text.top) / gLineHeight;

    SetRect(&r, view.right, view.top, view.right + 16, view.bottom);
    gScroll = NewControl(w, &r, "\p", true, 0, 0, lines > visible ? lines - visible : 0,
                         scrollBarProc, 0);
    SetRect(&r, 388, 322, 468, 342);
    agree = NewControl(w, &r, "\pAgree", true, 0, 0, 1, pushButProc, 0);
    SetRect(&r, 290, 322, 370, 342);
    disagree = NewControl(w, &r, "\pDisagree", true, 0, 0, 1, pushButProc, 0);

    while (result < 0) {
        if (!WaitNextEvent(everyEvent, &ev, 20, NULL)) continue;
        switch (ev.what) {
        case updateEvt:
            if ((WindowPtr)ev.message == w) {
                BeginUpdate(w);
                EraseRect(&w->portRect);
                TextFont(systemFont);
                TextSize(12);
                MoveTo(16, 26);
                DrawString("\pPlease read the license agreement. Click Agree to install MacShell.");
                TextFont(kFontIDGeneva);
                TextSize(9);
                FrameRect(&view);
                TEUpdate(&view, gTE);
                DrawControls(w);
                /* the default button outline goes on Agree */
                PenSize(3, 3);
                SetRect(&r, 384, 318, 472, 346);
                FrameRoundRect(&r, 16, 16);
                PenNormal();
                EndUpdate(w);
            }
            break;
        case mouseDown:
        {
            WindowPtr hit;
            short part = FindWindow(ev.where, &hit);
            if (hit != w) break;
            if (part == inDrag) {
                DragWindow(w, ev.where, &qd.screenBits.bounds);
            } else if (part == inContent) {
                ControlHandle c;
                Point p = ev.where;
                GlobalToLocal(&p);
                part = FindControl(p, w, &c);
                if (c == gScroll) {
                    if (part == kControlIndicatorPart) {
                        short before = GetControlValue(gScroll);
                        TrackControl(c, p, NULL);
                        TEScroll(0, (before - GetControlValue(gScroll)) * gLineHeight, gTE);
                    } else if (part) {
                        TrackControl(c, p, action);
                    }
                } else if (c == agree && TrackControl(c, p, NULL)) {
                    result = 1;
                } else if (c == disagree && TrackControl(c, p, NULL)) {
                    result = 0;
                }
            }
            break;
        }
        case keyDown:
        case autoKey:
        {
            char ch = (char)(ev.message & charCodeMask);
            if (ch == 0x1B || ((ev.modifiers & cmdKey) && ch == '.')) result = 0;   /* Esc, Cmd-. */
            else if (ch == 0x1E) ScrollTo(GetControlValue(gScroll) - 1);           /* up arrow */
            else if (ch == 0x1F) ScrollTo(GetControlValue(gScroll) + 1);           /* down arrow */
            else if (ch == 0x0B) ScrollTo(GetControlValue(gScroll) - visible);     /* page up */
            else if (ch == 0x0C) ScrollTo(GetControlValue(gScroll) + visible);     /* page down */
            break;
        }
        }
    }
    TEDispose(gTE);
    DisposeWindow(w);
    ReleaseResource(h);
    return result == 1;
}

/* ------------------------------------------------------------------ */
/* disks                                                              */
/* ------------------------------------------------------------------ */

static Handle Item(DialogPtr d, short i)
{
    short type; Handle h; Rect r;
    GetDialogItem(d, i, &type, &h, &r);
    return h;
}

static void Status(DialogPtr d, ConstStringPtr s)
{
    SetDialogItemText(Item(d, kStatusText), s);
}

static void StatusC(DialogPtr d, const char *c)
{
    Str255 s;
    PCopyC(s, c);
    Status(d, s);
}

static MenuHandle PopupMenu(ControlHandle popup)
{
    PopupPrivateDataHandle data = (PopupPrivateDataHandle)(**popup).contrlData;
    return data ? (**data).mHandle : NULL;
}

/* Writable mounted disks, with the startup disk chosen. */
static void BuildDiskMenu(DialogPtr d)
{
    ControlHandle popup = (ControlHandle)Item(d, kDiskPopup);
    MenuHandle menu = PopupMenu(popup);
    HParamBlockRec pb;
    Str255 name;
    short index, startupVRef = 0, selected = 1;
    long dirID;

    if (!menu) return;
    while (CountMenuItems(menu) > 0) DeleteMenuItem(menu, 1);
    FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &startupVRef, &dirID);
    gDiskCount = 0;
    for (index = 1; gDiskCount < kMaxDisks; index++) {
        memset(&pb, 0, sizeof pb);
        pb.volumeParam.ioNamePtr = name;
        pb.volumeParam.ioVolIndex = index;
        if (PBHGetVInfoSync(&pb) != noErr) break;
        if (pb.volumeParam.ioVAtrb & 0x8080) continue;      /* locked */
        AppendMenu(menu, "\px");
        SetMenuItemText(menu, CountMenuItems(menu), name);
        gDisks[gDiskCount++] = pb.volumeParam.ioVRefNum;
        if (pb.volumeParam.ioVRefNum == startupVRef) selected = gDiskCount;
    }
    SetControlMaximum(popup, gDiskCount > 0 ? gDiskCount : 1);
    SetControlValue(popup, selected);
    if (gDiskCount == 0) {
        AppendMenu(menu, "\p(no unlocked disk)");
        HiliteControl((ControlHandle)Item(d, kInstallBtn), 255);
    }
    Draw1Control(popup);
}

static short ChosenDisk(DialogPtr d, StringPtr name)
{
    ControlHandle popup = (ControlHandle)Item(d, kDiskPopup);
    short i = GetControlValue(popup);
    MenuHandle menu = PopupMenu(popup);
    if (i < 1 || i > gDiskCount) return 0;
    if (name && menu) GetMenuItemText(menu, i, name);
    return gDisks[i - 1];
}

/* ------------------------------------------------------------------ */
/* installing                                                         */
/* ------------------------------------------------------------------ */

/* folder dirID of name inside parent, created if missing */
static OSErr SubFolder(short vRefNum, long parent, ConstStringPtr name, long *dirID)
{
    CInfoPBRec pb;
    Str255 n;
    OSErr err;
    memcpy(n, name, name[0] + 1);
    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = n;
    pb.dirInfo.ioVRefNum = vRefNum;
    pb.dirInfo.ioDrDirID = parent;
    err = PBGetCatInfoSync(&pb);
    if (err == noErr) {
        if (!(pb.dirInfo.ioFlAttrib & 0x10)) return dupFNErr;   /* a file of that name */
        *dirID = pb.dirInfo.ioDrDirID;
        return noErr;
    }
    if (err != fnfErr) return err;
    return DirCreate(vRefNum, parent, name, dirID);
}

/* Copies an embedded resource into a fork, in pieces. */
static OSErr WriteForkFromResource(const FSSpec *spec, Boolean rsrcFork, ResType type)
{
    Handle h;
    long size, offset = 0;
    short ref;
    Ptr buf;
    OSErr err;

    SetResLoad(false);
    h = Get1Resource(type, 128);
    SetResLoad(true);
    if (!h) return resNotFound;
    size = GetResourceSizeOnDisk(h);
    err = rsrcFork ? FSpOpenRF(spec, fsWrPerm, &ref) : FSpOpenDF(spec, fsWrPerm, &ref);
    if (err) { ReleaseResource(h); return err; }
    buf = NewPtr(32768);
    if (!buf) { FSClose(ref); ReleaseResource(h); return memFullErr; }
    while (offset < size && !err) {
        long n = size - offset;
        if (n > 32768) n = 32768;
        ReadPartialResource(h, offset, buf, n);
        err = ResError();
        if (!err) err = FSWrite(ref, &n, buf);
        offset += n;
    }
    if (!err) err = SetEOF(ref, size);
    DisposePtr(buf);
    FSClose(ref);
    ReleaseResource(h);
    return err;
}

static OSErr InstallApp(short vRefNum, Boolean *replaced)
{
    long dir;
    FSSpec spec;
    FInfo fi;
    OSErr err = SubFolder(vRefNum, fsRtDirID, "\pMacShell", &dir);
    *replaced = false;
    if (err) return err;
    err = FSMakeFSSpec(vRefNum, dir, "\pMacShell", &spec);
    if (err == noErr) {
        err = FSpDelete(&spec);                         /* an older copy */
        if (err) return err;
        *replaced = true;
    } else if (err != fnfErr) {
        return err;
    }
    err = FSpCreate(&spec, 'MShl', 'APPL', smSystemScript);
    if (!err) err = WriteForkFromResource(&spec, false, 'MSdk');
    if (!err) err = WriteForkFromResource(&spec, true, 'MSfk');
    if (!err && FSpGetFInfo(&spec, &fi) == noErr) {
        fi.fdFlags = (fi.fdFlags | kHasBundle) & ~kHasBeenInited;   /* show its icon */
        err = FSpSetFInfo(&spec, &fi);
    }
    if (err) FSpDelete(&spec);                          /* no half-written copy */
    return err;
}

/* ~/.ssh = Documents:.ssh on the startup disk, with authorized_keys */
static OSErr SetUpSSH(Boolean *created)
{
    short vRefNum;
    long sys, docs, ssh;
    FSSpec spec;
    OSErr err;
    short ref;
    static const char text[] =
        "# Public keys allowed to log in to this Mac with MacShell's SSH server\r"
        "# (sshd start), one per line, as in OpenSSH's authorized_keys:\r"
        "#   ecdsa-sha2-nistp256 AAAA... comment\r"
        "# ECDSA P-256 and RSA (2048 bits or more) keys work; Ed25519 doesn't.\r";
    long len = sizeof text - 1;

    *created = false;
    err = FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &vRefNum, &sys);
    if (!err) err = SubFolder(vRefNum, fsRtDirID, "\pDocuments", &docs);
    if (!err) err = SubFolder(vRefNum, docs, "\p.ssh", &ssh);
    if (err) return err;
    err = FSMakeFSSpec(vRefNum, ssh, "\pauthorized_keys", &spec);
    if (err == noErr) return noErr;                     /* keep the existing one */
    if (err != fnfErr) return err;
    err = FSpCreate(&spec, 'ttxt', 'TEXT', smSystemScript);
    if (!err) err = FSpOpenDF(&spec, fsWrPerm, &ref);
    if (!err) {
        err = FSWrite(ref, &len, text);
        FSClose(ref);
    }
    if (!err) *created = true;
    return err;
}

static void Explain(StringPtr msg, OSErr err)
{
    if (err == fBsyErr)
        PAppendC(msg, "MacShell is open. Quit it and click Install again.");
    else if (err == wPrErr || err == vLckdErr)
        PAppendC(msg, "the disk is locked.");
    else if (err == dskFulErr)
        PAppendC(msg, "the disk is full (MacShell needs about 2.5 MB).");
    else if (err == dupFNErr)
        PAppendC(msg, "there is a file called MacShell where its folder would go.");
    else {
        PAppendC(msg, "error ");
        PAppendNum(msg, err);
        PAppendC(msg, ".");
    }
}

static void DoInstall(DialogPtr d)
{
    Str255 disk, msg;
    short vRefNum = ChosenDisk(d, disk);
    Boolean replaced = false, sshCreated = false;
    Boolean wantSSH = GetControlValue((ControlHandle)Item(d, kSSHBox));
    Boolean wantSeed = GetControlValue((ControlHandle)Item(d, kSeedBox));
    OSErr err;

    if (!vRefNum) return;
    StatusC(d, "Installing MacShell...");
    SetCursor(*GetCursor(watchCursor));
    err = InstallApp(vRefNum, &replaced);
    if (err) {
        InitCursor();
        PCopyC(msg, "Couldn't install MacShell: ");
        Explain(msg, err);
        Status(d, msg);
        return;
    }
    if (wantSSH) {
        err = SetUpSSH(&sshCreated);
        if (err) {
            InitCursor();
            PCopyC(msg, "MacShell is installed, but ~/.ssh couldn't be set up: ");
            Explain(msg, err);
            Status(d, msg);
            return;
        }
    }
    if (wantSeed) {
        StatusC(d, "Gathering randomness for MacShell's secure connections...");
        macentropy_init();                              /* saves the seed file */
    }
    InitCursor();

    PCopyC(msg, replaced ? "MacShell was updated in \"" : "MacShell is installed in \"");
    PAppend(msg, disk);
    PAppendC(msg, ":MacShell\".");
    if (wantSSH)
        PAppendC(msg, sshCreated ? " ~/.ssh is set up: put the public keys allowed to log in in "
                                   "~/.ssh/authorized_keys."
                                 : " ~/.ssh/authorized_keys was already there and was kept.");
    if (wantSeed)
        PAppendC(msg, macentropy_get_stats()->ready ? " Randomness is ready." : "");
    Status(d, msg);
    HiliteControl((ControlHandle)Item(d, kInstallBtn), 255);
    SetControlTitle((ControlHandle)Item(d, kQuitBtn), "\pDone");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    DialogPtr d;
    short hit = 0;
    long resp;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    FlushEvents(everyEvent, 0);
    InitCursor();

    if (Gestalt(gestaltFindFolderAttr, &resp) != noErr || !(resp & (1L << gestaltFindFolderPresent))) {
        Fail("MacShell needs System 7 or later.");
        return 0;
    }
    if (!(Gestalt(gestaltSysArchitecture, &resp) == noErr && resp == gestaltPowerPC) &&
        (Gestalt(gestaltProcessorType, &resp) != noErr || resp < gestalt68020)) {
        Fail("MacShell needs a 68020 or later processor, or a Power Mac.");
        return 0;
    }
    if (!LicenseAgreed())
        return 0;

    d = GetNewDialog(kMainDialog, NULL, (WindowPtr)-1);
    if (!d) return 0;
    SetDialogDefaultItem(d, kInstallBtn);
    SetDialogCancelItem(d, kQuitBtn);
    SetControlValue((ControlHandle)Item(d, kSSHBox), 1);
    SetControlValue((ControlHandle)Item(d, kSeedBox), 1);
    BuildDiskMenu(d);
    StatusC(d, "Choose a disk, then click Install. MacShell goes into a folder called MacShell "
               "on that disk.");
    ShowWindow(d);

    while (hit != kQuitBtn) {
        ModalDialog(NULL, &hit);
        if (hit == kInstallBtn) DoInstall(d);
        else if (hit == kSSHBox || hit == kSeedBox) {
            ControlHandle c = (ControlHandle)Item(d, hit);
            SetControlValue(c, !GetControlValue(c));
        }
    }
    DisposeDialog(d);
    return 0;
}
