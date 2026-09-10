// ===========================================================================
// uinject -- a virtual keyboard AND pointer, for driving Orbiter under test.
//
// WHY THIS EXISTS, AGAIN
// ----------------------
// This desk is KDE on Wayland. xdotool cannot drive it:
//
//   * `xdotool key --window <id>` sends a synthetic XSendEvent to one window.
//     GLFW reads the physical key state the X server maintains, which a
//     synthetic event does not touch -- and xdotool still exits 0.
//   * Plain XTEST fake input does not reach the client either. Measured again
//     on 2026-09-08 with ORBITER_TRACE_INPUT: the window reported
//     `focused=1` while `glfwPressed=0 firstGlfwKey=-1` for every poll of an
//     `xdotool key F8`, and no button change ever reached postMouseMessages.
//
// A test built on xdotool therefore reports "the input is dead" on a
// perfectly good build. It has now done so twice, in two different sessions.
//
// /dev/uinput enters through evdev, exactly where a USB device does, so
// neither the compositor, the X server nor the application can tell the
// difference. No root needed: logind puts an ACL on the node.
//
// WHAT IS NEW HERE vs. the keyboard-only tests/uinput_key.c this replaces:
// EV_REL (REL_X, REL_Y, REL_WHEEL) and the three mouse buttons, which
// the porting notes recorded as the open gap --
// "the click lands wherever the pointer already is ... closing it needs
// EV_REL on the virtual device". Now the pointer can be put on a cockpit
// button before it is clicked, and the wheel can be turned.
//
// A HAPPY ACCIDENT WORTH REMEMBERING: for the main keyboard block, Linux
// evdev KEY_* codes ARE the AT set-1 scan codes, which are the DIK_* codes
// Orbiter indexes its 256-byte key state by. KEY_ESC 1 == DIK_ESCAPE,
// KEY_KPPLUS 78 == DIK_ADD 0x4E. So the number injected is the number that
// should light up in Orbiter's buffer, with no second table in the way.
//
// Build:  cc -O2 -o uinject uinject.c
// Drive:  ./uinject 'moveto 3200 700' 'click l' 'key F8' 'wheel 3'
//         ./uinject -   (reads commands from stdin, one per line)
// ===========================================================================

#define _GNU_SOURCE
#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>

static int fd = -1;

static void emit(int type, int code, int val)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.code = code; ev.value = val;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        fprintf(stderr, "uinject: write failed: %s\n", strerror(errno));
}

static void syn(void) { emit(EV_SYN, SYN_REPORT, 0); }

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// ---------------------------------------------------------------------------
// Key names. Deliberately the names Orbiter's Keymap.cpp uses where they
// differ from evdev's, so a script reads like a keymap.cfg line.
struct KeyName { const char *name; int code; };
static const struct KeyName kNames[] = {
    {"ESC",KEY_ESC},{"ESCAPE",KEY_ESC},{"TAB",KEY_TAB},{"SPACE",KEY_SPACE},
    {"RETURN",KEY_ENTER},{"ENTER",KEY_ENTER},{"BACK",KEY_BACKSPACE},
    {"LSHIFT",KEY_LEFTSHIFT},{"RSHIFT",KEY_RIGHTSHIFT},
    {"LCONTROL",KEY_LEFTCTRL},{"RCONTROL",KEY_RIGHTCTRL},
    {"LALT",KEY_LEFTALT},{"RALT",KEY_RIGHTALT},
    {"UP",KEY_UP},{"DOWN",KEY_DOWN},{"LEFT",KEY_LEFT},{"RIGHT",KEY_RIGHT},
    {"HOME",KEY_HOME},{"END",KEY_END},{"PRIOR",KEY_PAGEUP},{"NEXT",KEY_PAGEDOWN},
    {"INSERT",KEY_INSERT},{"DELETE",KEY_DELETE},
    {"F1",KEY_F1},{"F2",KEY_F2},{"F3",KEY_F3},{"F4",KEY_F4},{"F5",KEY_F5},
    {"F6",KEY_F6},{"F7",KEY_F7},{"F8",KEY_F8},{"F9",KEY_F9},{"F10",KEY_F10},
    {"F11",KEY_F11},{"F12",KEY_F12},
    {"ADD",KEY_KPPLUS},{"SUBTRACT",KEY_KPMINUS},{"MULTIPLY",KEY_KPASTERISK},
    {"DIVIDE",KEY_KPSLASH},{"DECIMAL",KEY_KPDOT},{"NUMPADENTER",KEY_KPENTER},
    {"NUMPAD0",KEY_KP0},{"NUMPAD1",KEY_KP1},{"NUMPAD2",KEY_KP2},
    {"NUMPAD3",KEY_KP3},{"NUMPAD4",KEY_KP4},{"NUMPAD5",KEY_KP5},
    {"NUMPAD6",KEY_KP6},{"NUMPAD7",KEY_KP7},{"NUMPAD8",KEY_KP8},
    {"NUMPAD9",KEY_KP9},
    {"LBRACKET",KEY_LEFTBRACE},{"RBRACKET",KEY_RIGHTBRACE},
    {"SEMICOLON",KEY_SEMICOLON},{"APOSTROPHE",KEY_APOSTROPHE},
    {"COMMA",KEY_COMMA},{"PERIOD",KEY_DOT},{"SLASH",KEY_SLASH},
    {"BACKSLASH",KEY_BACKSLASH},{"GRAVE",KEY_GRAVE},{"MINUS",KEY_MINUS},
    {"EQUALS",KEY_EQUAL},
    {NULL,0}
};

static int keyFromName(const char *n)
{
    char up[64]; size_t i;
    for (i = 0; n[i] && i < sizeof(up)-1; i++) up[i] = (char)toupper((unsigned char)n[i]);
    up[i] = 0;

    if (up[0] == '#') return atoi(up + 1);               // raw evdev code
    if (up[1] == 0) {
        if (up[0] >= 'A' && up[0] <= 'Z') {
            static const int a[26] = {KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,
                KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,KEY_N,KEY_O,KEY_P,KEY_Q,
                KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z};
            return a[up[0]-'A'];
        }
        if (up[0] >= '0' && up[0] <= '9') {
            static const int d[10] = {KEY_0,KEY_1,KEY_2,KEY_3,KEY_4,
                                      KEY_5,KEY_6,KEY_7,KEY_8,KEY_9};
            return d[up[0]-'0'];
        }
    }
    for (i = 0; kNames[i].name; i++)
        if (!strcmp(up, kNames[i].name)) return kNames[i].code;
    return -1;
}

static int btnFromName(const char *s)
{
    if (s[0]=='l'||s[0]=='L') return BTN_LEFT;
    if (s[0]=='r'||s[0]=='R') return BTN_RIGHT;
    if (s[0]=='m'||s[0]=='M') return BTN_MIDDLE;
    return -1;
}

// Relative motion, chunked. libinput applies pointer acceleration to large
// single deltas, so a 3000-pixel jump does NOT land 3000 pixels away. Small
// steps stay in the flat part of the acceleration curve and land where asked.
static void moveRel(int dx, int dy)
{
    const int step = 8;
    while (dx || dy) {
        int sx = dx >  step ?  step : (dx < -step ? -step : dx);
        int sy = dy >  step ?  step : (dy < -step ? -step : dy);
        if (sx) emit(EV_REL, REL_X, sx);
        if (sy) emit(EV_REL, REL_Y, sy);
        syn();
        dx -= sx; dy -= sy;
        usleep(300);
    }
}

// Pin the pointer at the desktop origin by driving it hard into the corner,
// then walk out to (x,y). The corner is the only position this device can
// know without reading the cursor back, and REL devices cannot be queried.
static void moveTo(int x, int y)
{
    int i;
    for (i = 0; i < 40; i++) { emit(EV_REL, REL_X, -400); emit(EV_REL, REL_Y, -400); syn(); usleep(300); }
    msleep(60);
    moveRel(x, y);
}

static void runCommand(char *line)
{
    char op[32] = {0}, a1[64] = {0}, a2[64] = {0};
    int n = sscanf(line, "%31s %63s %63s", op, a1, a2);
    if (n < 1 || op[0] == '#') return;

    if (!strcmp(op, "key")) {
        int k = keyFromName(a1);
        long ms = (n >= 3) ? atol(a2) : 150;            // 150 ms: 40 ms taps
        if (k < 0) { fprintf(stderr, "uinject: unknown key '%s'\n", a1); return; }
        emit(EV_KEY, k, 1); syn();                      // are lost at ~95 fps
        msleep(ms);
        emit(EV_KEY, k, 0); syn();
    } else if (!strcmp(op, "keydown") || !strcmp(op, "keyup")) {
        int k = keyFromName(a1);
        if (k < 0) { fprintf(stderr, "uinject: unknown key '%s'\n", a1); return; }
        emit(EV_KEY, k, op[3] == 'd' ? 1 : 0); syn();
    } else if (!strcmp(op, "moveto")) {
        moveTo(atoi(a1), atoi(a2));
    } else if (!strcmp(op, "move")) {
        moveRel(atoi(a1), atoi(a2));
    } else if (!strcmp(op, "btn")) {
        int b = btnFromName(a1);
        if (b < 0) return;
        emit(EV_KEY, b, (a2[0]=='d') ? 1 : 0); syn();
    } else if (!strcmp(op, "click")) {
        int b = btnFromName(a1);
        if (b < 0) return;
        emit(EV_KEY, b, 1); syn(); msleep(80);
        emit(EV_KEY, b, 0); syn();
    } else if (!strcmp(op, "wheel")) {
        int c = atoi(a1), i, s = c < 0 ? -1 : 1;
        for (i = 0; i < (c < 0 ? -c : c); i++) { emit(EV_REL, REL_WHEEL, s); syn(); msleep(60); }
    } else if (!strcmp(op, "sleep")) {
        msleep(atol(a1));
    } else {
        fprintf(stderr, "uinject: unknown command '%s'\n", op);
    }
}

int main(int argc, char **argv)
{
    int i;
    struct uinput_setup us;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open /dev/uinput"); return 1; }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (i = 1; i < KEY_MAX; i++) ioctl(fd, UI_SET_KEYBIT, i);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    // REL_X / REL_Y ARE DELIBERATELY NOT DECLARED.
    //
    // With them, this device owns a cursor position, and KDE restores the
    // cursor to that position on the device's first event after a pause --
    // measured: `mv 636 344` was immediately followed by a motion to
    // (1154,610), the point a much earlier `moveto` had left this device at,
    // and the wheel and the click then landed THERE. Two hours of "the click
    // does nothing" was the click going somewhere else.
    //
    // Motion comes from xdotool, which works on this desk; this device only
    // has to press things. Without a position it has none to restore, and
    // libinput still classifies it as a pointer because it has buttons.
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);

    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_USB;
    us.id.vendor  = 0x1d6b;
    us.id.product = 0x0104;
    strcpy(us.name, "orbiter-uinject");
    ioctl(fd, UI_DEV_SETUP, &us);
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); return 1; }

    // udev/libinput must see the device before it will route its events.
    msleep(700);

    if (argc == 2 && !strcmp(argv[1], "-")) {
        char line[256];
        while (fgets(line, sizeof(line), stdin)) runCommand(line);
    } else {
        for (i = 1; i < argc; i++) { char b[256]; snprintf(b,sizeof(b),"%s",argv[i]); runCommand(b); }
    }

    msleep(200);
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
