/* MacShell Installer resources. Rez runs in the build folder ($$read). */
#include "MacTypes.r"
#include "Dialogs.r"
#include "Controls.r"
#include "Menus.r"
#include "Processes.r"
#include "Finder.r"
#include "Icons.r"
#include "constants.r"

resource 'DLOG' (128) {
    { 60, 60, 320, 520 },
    movableDBoxProc,
    invisible,
    noGoAway,
    0,
    128,
    "MacShell Installer",
    centerMainScreen
};

resource 'DITL' (128) {
    {
        /* 1 */ { 226, 360, 246, 440 }, Button { enabled, "Install" };
        /* 2 */ { 226, 270, 246, 350 }, Button { enabled, "Quit" };
        /* 3 */ { 84, 20, 104, 440 },   Control { enabled, 128 };
        /* 4 */ { 162, 20, 218, 440 },  StaticText { disabled, "" };
        /* 5 */ { 114, 20, 132, 440 },  CheckBox { enabled, "Set up ~/.ssh for logging in to this Mac with SSH" };
        /* 6 */ { 134, 20, 152, 440 },  CheckBox { enabled, "Gather randomness now, so MacShell starts quickly" };
        /* 7 */ { 14, 20, 46, 52 },     Icon { disabled, 128 };
        /* 8 */ { 14, 66, 32, 440 },    StaticText { disabled, "MacShell " APP_VERSION };
        /* 9 */ { 34, 66, 70, 440 },    StaticText { disabled, "A Unix-style shell for classic Mac OS, with an SSH client and server, file transfer and network tools." }
    }
};

/* popup CDEF: popupMenuProc (1008) + popupFixedWidth (1) */
resource 'CNTL' (128) {
    { 84, 20, 104, 440 }, 0, visible, 80, 128, 1009, 0, "Install on:"
};

resource 'MENU' (128) {
    128, textMenuProc, allEnabled, enabled, "Disks", { }
};

resource 'ALRT' (129) {
    { 40, 40, 140, 380 }, 129,
    { OK, visible, sound1; OK, visible, sound1; OK, visible, sound1; OK, visible, sound1 },
    alertPositionMainScreen
};
resource 'DITL' (129) {
    {
        { 70, 260, 90, 330 }, Button { enabled, "OK" };
        { 10, 60, 60, 330 },  StaticText { disabled, "^0" }
    }
};

/* MacShell itself, and the licenses */
data 'MSdk' (128, "MacShell data fork (PowerPC code)") { $$read("MacShell.data") };
data 'MSfk' (128, "MacShell resource fork (68k code and resources)") { $$read("MacShell.rsrc") };
data 'TEXT' (128, "Licenses") { $$read("licenses.txt") };

/* MacShell's icon */
#include "icons.r"

resource 'BNDL' (128) {
    'MShi', 0,
    { 'ICN#', { 0, APP_ICON }; 'FREF', { 0, APP_ICON } }
};
resource 'FREF' (APP_ICON) { 'APPL', 0, "" };
data 'MShi' (0, "Owner resource") { $"12" "MacShell Installer" };

resource 'vers' (1) {
    APP_VERSION_MAJOR, APP_VERSION_MINOR, release, 0x00, verUS, APP_VERSION,
    APP_VERSION ", MacShell Installer"
};

resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground, doesActivateOnFGSwitch,
    backgroundAndForeground, dontGetFrontClicks, ignoreChildDiedEvents, is32BitCompatible,
    notHighLevelEventAware, onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    reserved, reserved, reserved,
    768 * 1024, 512 * 1024
};
