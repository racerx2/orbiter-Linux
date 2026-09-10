// DirectInput implementation over GLFW.
//
// Src/Orbiter/Di7frame.cpp wraps DirectInput for keyboard, mouse and joystick,
// and Orbiter::Create calls pDI->Create() before the Launchpad is built -- so
// this has to work, and work sanely with no joystick attached, before there is
// any window at all.
//
// GLFW covers all three devices. The awkward part is not the input itself but
// the shape of the API around it: DirectInput is device-oriented and polled,
// GLFW is window-oriented and callback-driven. The bridge is that a device
// here caches state which GLFW refreshes each frame, and GetDeviceState hands
// back that cache -- which is exactly what a polled DirectInput device does.
//
// KEYBOARD STATE
//   Orbiter reads a 256-byte buffer indexed by DirectInput scan code, with
//   0x80 set for a pressed key. GLFW reports by its own key enum, so a
//   translation table maps one to the other. The scan codes matter: Orbiter's
//   default key bindings and any saved keymap.cfg are written in terms of
//   them, so a wrong entry silently rebinds a key rather than failing.

#include <windows.h>
#include <dinput.h>

#include <GLFW/glfw3.h>

#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

// The Launchpad-phase window, set once the UI host creates it. Input is only
// meaningful once a window exists; before that every device reports idle.
extern "C" GLFWwindow *orbiter_GetGLFWWindow(void);

// Registered by a graphics client that owns its own window; defined in
// Win32Dlg.cpp alongside the cursor hooks.
extern "C" const unsigned char *(*g_clientKeyState)();

namespace {

// ---------------------------------------------------------------------------
// GLFW key -> DirectInput scan code
//
// THE SPECIFICATION FOR THIS TABLE IS keyname[NKEY] IN Src/Orbiter/Keymap.cpp.
//
// That array is the complete set of scan codes Orbiter is willing to bind: it
// is what Keymap::ScanStr matches a keymap.cfg line against and what
// Keymap::PrintStr writes back out. Every one of its 95 entries must therefore
// have a source here, or the corresponding key is unbindable and -- if it is
// bound BY DEFAULT -- simply dead.
//
// It had 74 of them, and the 21 that were missing were not an obscure tail.
// Reading lkeyspec[] in Keymap.cpp, the default bindings that produced no key
// state at all were:
//
//   ADD / SUBTRACT     IncMainThrust, DecMainThrust,
//                      OverrideFullMainThrust, OverrideFullRetroThrust
//   MULTIPLY           KillMainRetroThrust
//   DECIMAL            DecHoverThrust
//   DIVIDE             RCSMode, and RCSEnable with Ctrl
//   LBRACKET/RBRACKET  NMPrograde, NMRetrograde
//   SEMICOLON          NMNormal
//   APOSTROPHE         NMAntinormal
//   COMMA / PERIOD     WheelbrakeLeft, WheelbrakeRight
//   PRIOR / NEXT       TrackCamRetreat, TrackCamAdvance
//   SYSRQ              DlgCapture (with Ctrl)
//
// -- which is to say the main engine, the retros, RCS mode, hover-down, all
// four navmode autopilots, both wheel brakes and the external camera's zoom.
// The keys that DID work were the ones a QWERTY row happens to cover, so the
// simulator looked responsive while none of the actual flight controls
// existed.
//
// The rest (GRAVE, BACKSLASH, SLASH, CAPITAL, NUMLOCK, SCROLL, OEM_102,
// NUMPADENTER) are not bound by default but are nameable in keymap.cfg, so a
// user rebinding to one would have got silence.
//
// An unmapped GLFW key still sets no byte, which remains preferable to
// guessing a scan code and silently binding the wrong physical key -- but
// after this there is nothing left to guess about.
//
// GLFW_KEY_* are POSITIONAL codes named for the US layout, and DIK_* are
// positional scan codes named the same way, so the correspondence is exact
// and layout-independent on both sides. That is why this is a table of
// constants and not a lookup through any keymap.
// ---------------------------------------------------------------------------

struct KeyMap { int glfw; int dik; };

const KeyMap kKeyMap[] = {
    { GLFW_KEY_ESCAPE, DIK_ESCAPE }, { GLFW_KEY_1, DIK_1 },
    { GLFW_KEY_2, DIK_2 }, { GLFW_KEY_3, DIK_3 }, { GLFW_KEY_4, DIK_4 },
    { GLFW_KEY_5, DIK_5 }, { GLFW_KEY_6, DIK_6 }, { GLFW_KEY_7, DIK_7 },
    { GLFW_KEY_8, DIK_8 }, { GLFW_KEY_9, DIK_9 }, { GLFW_KEY_0, DIK_0 },
    { GLFW_KEY_MINUS, DIK_MINUS }, { GLFW_KEY_EQUAL, DIK_EQUALS },
    { GLFW_KEY_BACKSPACE, DIK_BACK }, { GLFW_KEY_TAB, DIK_TAB },
    { GLFW_KEY_Q, DIK_Q }, { GLFW_KEY_W, DIK_W }, { GLFW_KEY_E, DIK_E },
    { GLFW_KEY_R, DIK_R }, { GLFW_KEY_T, DIK_T }, { GLFW_KEY_Y, DIK_Y },
    { GLFW_KEY_U, DIK_U }, { GLFW_KEY_I, DIK_I }, { GLFW_KEY_O, DIK_O },
    { GLFW_KEY_P, DIK_P },
    { GLFW_KEY_LEFT_BRACKET,  DIK_LBRACKET },   // NMPrograde
    { GLFW_KEY_RIGHT_BRACKET, DIK_RBRACKET },   // NMRetrograde
    { GLFW_KEY_ENTER, DIK_RETURN },
    { GLFW_KEY_LEFT_CONTROL, DIK_LCONTROL },
    { GLFW_KEY_A, DIK_A }, { GLFW_KEY_S, DIK_S }, { GLFW_KEY_D, DIK_D },
    { GLFW_KEY_F, DIK_F }, { GLFW_KEY_G, DIK_G }, { GLFW_KEY_H, DIK_H },
    { GLFW_KEY_J, DIK_J }, { GLFW_KEY_K, DIK_K }, { GLFW_KEY_L, DIK_L },
    { GLFW_KEY_SEMICOLON,    DIK_SEMICOLON },   // NMNormal
    { GLFW_KEY_APOSTROPHE,   DIK_APOSTROPHE },  // NMAntinormal
    { GLFW_KEY_GRAVE_ACCENT, DIK_GRAVE },
    { GLFW_KEY_LEFT_SHIFT, DIK_LSHIFT },
    { GLFW_KEY_BACKSLASH,  DIK_BACKSLASH },
    { GLFW_KEY_Z, DIK_Z }, { GLFW_KEY_X, DIK_X }, { GLFW_KEY_C, DIK_C },
    { GLFW_KEY_V, DIK_V }, { GLFW_KEY_B, DIK_B }, { GLFW_KEY_N, DIK_N },
    { GLFW_KEY_M, DIK_M },
    { GLFW_KEY_COMMA,  DIK_COMMA },             // WheelbrakeLeft
    { GLFW_KEY_PERIOD, DIK_PERIOD },            // WheelbrakeRight
    { GLFW_KEY_SLASH,  DIK_SLASH },
    { GLFW_KEY_RIGHT_SHIFT, DIK_RSHIFT }, { GLFW_KEY_LEFT_ALT, DIK_LMENU },
    { GLFW_KEY_SPACE, DIK_SPACE },
    { GLFW_KEY_CAPS_LOCK, DIK_CAPITAL },
    { GLFW_KEY_F1, DIK_F1 }, { GLFW_KEY_F2, DIK_F2 }, { GLFW_KEY_F3, DIK_F3 },
    { GLFW_KEY_F4, DIK_F4 }, { GLFW_KEY_F5, DIK_F5 }, { GLFW_KEY_F6, DIK_F6 },
    { GLFW_KEY_F7, DIK_F7 }, { GLFW_KEY_F8, DIK_F8 }, { GLFW_KEY_F9, DIK_F9 },
    { GLFW_KEY_F10, DIK_F10 }, { GLFW_KEY_F11, DIK_F11 },
    { GLFW_KEY_F12, DIK_F12 },
    { GLFW_KEY_NUM_LOCK,    DIK_NUMLOCK },
    { GLFW_KEY_SCROLL_LOCK, DIK_SCROLL },
    { GLFW_KEY_KP_7, DIK_NUMPAD7 }, { GLFW_KEY_KP_8, DIK_NUMPAD8 },
    { GLFW_KEY_KP_9, DIK_NUMPAD9 }, { GLFW_KEY_KP_4, DIK_NUMPAD4 },
    { GLFW_KEY_KP_5, DIK_NUMPAD5 }, { GLFW_KEY_KP_6, DIK_NUMPAD6 },
    { GLFW_KEY_KP_1, DIK_NUMPAD1 }, { GLFW_KEY_KP_2, DIK_NUMPAD2 },
    { GLFW_KEY_KP_3, DIK_NUMPAD3 }, { GLFW_KEY_KP_0, DIK_NUMPAD0 },
    // The keypad operator cluster: the main engine, the retros, hover-down
    // and RCS mode. See the note above -- none of these existed.
    { GLFW_KEY_KP_SUBTRACT, DIK_SUBTRACT },     // DecMainThrust / retro
    { GLFW_KEY_KP_ADD,      DIK_ADD },          // IncMainThrust
    { GLFW_KEY_KP_MULTIPLY, DIK_MULTIPLY },     // KillMainRetroThrust
    { GLFW_KEY_KP_DIVIDE,   DIK_DIVIDE },       // RCSMode / RCSEnable
    { GLFW_KEY_KP_DECIMAL,  DIK_DECIMAL },      // DecHoverThrust
    { GLFW_KEY_KP_ENTER,    DIK_NUMPADENTER },
    { GLFW_KEY_RIGHT_CONTROL, DIK_RCONTROL },
    { GLFW_KEY_RIGHT_ALT, DIK_RMENU },
    // GLFW_KEY_WORLD_2 is the extra key ISO layouts carry beside the left
    // shift; DirectInput calls it OEM_102 and Keymap.cpp names it that too.
    { GLFW_KEY_WORLD_2, DIK_OEM_102 },
    { GLFW_KEY_PRINT_SCREEN, DIK_SYSRQ },       // DlgCapture, with Ctrl
    { GLFW_KEY_PAUSE, DIK_PAUSE },
    { GLFW_KEY_HOME, DIK_HOME }, { GLFW_KEY_UP, DIK_UP },
    { GLFW_KEY_PAGE_UP,   DIK_PRIOR },          // TrackCamRetreat
    { GLFW_KEY_LEFT, DIK_LEFT }, { GLFW_KEY_RIGHT, DIK_RIGHT },
    { GLFW_KEY_END, DIK_END }, { GLFW_KEY_DOWN, DIK_DOWN },
    { GLFW_KEY_PAGE_DOWN, DIK_NEXT },           // TrackCamAdvance
    { GLFW_KEY_INSERT, DIK_INSERT }, { GLFW_KEY_DELETE, DIK_DELETE },
};

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

enum class DeviceKind { Keyboard, Mouse, Joystick };

class Device : public IDirectInputDevice8A {
public:
    explicit Device(DeviceKind kind) : m_kind(kind) {}

    ULONG Release() override { delete this; return 0; }

    HRESULT SetDataFormat(LPCDIDATAFORMAT) override { return DI_OK; }
    HRESULT SetCooperativeLevel(HWND, DWORD) override { return DI_OK; }

    HRESULT SetProperty(LPCGUID prop, LPDIPROPHEADER hdr) override
    {
        // DIPROP_BUFFERSIZE switches the device to buffered mode. GLFW has no
        // separate buffered path -- events are already queued by the window
        // system -- so the request is accepted and the size noted.
        if (prop == DIPROP_BUFFERSIZE && hdr)
            m_bufferSize = ((LPDIPROPDWORD)hdr)->dwData;
        return DI_OK;
    }

    HRESULT Acquire() override   { m_acquired = true;  return DI_OK; }
    HRESULT Unacquire() override { m_acquired = false; return DI_OK; }
    HRESULT Poll() override      { return DI_OK; }

    HRESULT GetDeviceState(DWORD cb, LPVOID data) override
    {
        if (!data) return DIERR_INVALIDPARAM;
        if (!m_acquired) return DIERR_NOTACQUIRED;

        GLFWwindow *win = orbiter_GetGLFWWindow();
        if (!win) {
            // No window yet: report everything idle rather than failing.
            // Orbiter::Create runs this path before the Launchpad exists.
            memset(data, 0, cb);
            return DI_OK;
        }

        switch (m_kind) {
        case DeviceKind::Keyboard: {
            if (cb < 256) return DIERR_INVALIDPARAM;
            BYTE *keys = (BYTE *)data;
            memset(keys, 0, 256);
            for (const KeyMap &k : kKeyMap) {
                if (glfwGetKey(win, k.glfw) == GLFW_PRESS)
                    keys[k.dik] = 0x80;   // DirectInput's "pressed" bit
            }

            // WHAT GLFW ITSELF SEES, as opposed to what the table forwards.
            //
            // Orbiter::UserInput's own ORBITER_TRACE_INPUT line reports the
            // count AFTER this translation, so "no keys" there has two very
            // different causes -- GLFW saw nothing, or GLFW saw something this
            // table has no entry for -- and it cannot tell them apart. This
            // scans every GLFW key code, not just the mapped ones, and also
            // reports whether GLFW considers the window focused at all, which
            // is the third possibility.
            if (getenv("ORBITER_TRACE_INPUT")) {
                int anyGlfw = 0, firstKey = -1, mapped = 0;
                for (int g = GLFW_KEY_SPACE; g <= GLFW_KEY_LAST; ++g) {
                    if (glfwGetKey(win, g) == GLFW_PRESS) {
                        ++anyGlfw;
                        if (firstKey < 0) firstKey = g;
                    }
                }
                for (int i = 0; i < 256; ++i) if (keys[i]) ++mapped;
                static int n = 0;
                if (anyGlfw || (n++ % 200) == 0)
                    fprintf(stderr, "DInput.kbd: win=%p focused=%d "
                                    "glfwPressed=%d firstGlfwKey=%d mapped=%d\n",
                            (void *)win,
                            glfwGetWindowAttrib(win, GLFW_FOCUSED),
                            anyGlfw, firstKey, mapped);
            }

            // Merge in keys from a graphics client that owns its own window.
            //
            // THE PREMISE THIS WAS WRITTEN FOR IS NO LONGER TRUE, and the old
            // comment here asserted it as fact: "The Vulkan client creates an
            // xcb window of its own, which GLFW knows nothing about -- so
            // during a session glfwGetKey above sees nothing and no key ever
            // reaches the simulation."
            //
            // The Vulkan client creates no window. It adopts UIHost's, which
            // IS the GLFW window this function polls -- see the ownership note
            // at the top of OVP/VulkanClient/VulkanDevice.h. MEASURED, in a
            // live Delta-glider session on both backends, with keys injected
            // through /dev/uinput (tests/kbdproof.sh):
            //
            //   DInput.kbd: win=0x... focused=1 glfwPressed=1
            //               firstGlfwKey=334 mapped=1
            //
            // -- glfwGetKey sees the key during a session, on X11 and on
            // Wayland, and the table above translates it.
            //
            // So nothing calls orbiter_RegisterClientInput and g_clientKeyState
            // is always null. The hook is kept rather than deleted because a
            // future client that DOES open its own window would need exactly
            // it, and because a null check costs nothing -- but it is dead
            // today, and a comment claiming the keyboard depends on it sends
            // the next reader to the wrong file.
            //
            // Resolved through a registration call rather than dlsym: plugins
            // are RTLD_LOCAL and never enter the global scope.
            if (g_clientKeyState) {
                if (const unsigned char *ck = g_clientKeyState()) {
                    for (int i = 0; i < 256; ++i) keys[i] |= ck[i];
                }
            }

            return DI_OK;
        }

        case DeviceKind::Mouse: {
            if (cb < sizeof(DIMOUSESTATE)) return DIERR_INVALIDPARAM;
            DIMOUSESTATE *ms = (DIMOUSESTATE *)data;
            memset(ms, 0, sizeof(*ms));

            // DirectInput reports mouse motion as a delta since the last read,
            // GLFW reports an absolute position, so the delta is derived here.
            double cx = 0, cy = 0;
            glfwGetCursorPos(win, &cx, &cy);
            if (m_haveMouse) {
                ms->lX = (LONG)(cx - m_lastX);
                ms->lY = (LONG)(cy - m_lastY);
            }
            m_lastX = cx; m_lastY = cy; m_haveMouse = true;

            for (int b = 0; b < 3; ++b)
                if (glfwGetMouseButton(win, b) == GLFW_PRESS)
                    ms->rgbButtons[b] = 0x80;
            return DI_OK;
        }

        case DeviceKind::Joystick: {
            if (cb < sizeof(DIJOYSTATE2)) return DIERR_INVALIDPARAM;
            DIJOYSTATE2 *js = (DIJOYSTATE2 *)data;
            memset(js, 0, sizeof(*js));

            if (!glfwJoystickPresent(m_joyIndex)) return DIERR_INPUTLOST;

            int naxes = 0;
            const float *axes = glfwGetJoystickAxes(m_joyIndex, &naxes);
            // DirectInput axes are 0..65535 with 32767 centred, after the
            // range this device set; GLFW reports -1..+1. Orbiter sets the
            // range explicitly, so the fixed 16-bit mapping is what it expects.
            auto toDI = [](float v) -> LONG {
                return (LONG)((v + 1.0f) * 0.5f * 65535.0f);
            };
            if (axes) {
                if (naxes > 0) js->lX  = toDI(axes[0]);
                if (naxes > 1) js->lY  = toDI(axes[1]);
                if (naxes > 2) js->lZ  = toDI(axes[2]);
                if (naxes > 3) js->lRz = toDI(axes[3]);
                if (naxes > 4) js->rglSlider[0] = toDI(axes[4]);
                if (naxes > 5) js->rglSlider[1] = toDI(axes[5]);
            }

            int nbuttons = 0;
            const unsigned char *btn = glfwGetJoystickButtons(m_joyIndex, &nbuttons);
            if (btn) {
                const int n = nbuttons < 128 ? nbuttons : 128;
                for (int i = 0; i < n; ++i)
                    js->rgbButtons[i] = btn[i] ? 0x80 : 0x00;
            }

            // Hat switches report centipoint degrees, or -1 when centred.
            int nhats = 0;
            const unsigned char *hats = glfwGetJoystickHats(m_joyIndex, &nhats);
            for (int i = 0; i < 4; ++i) js->rgdwPOV[i] = (DWORD)-1;
            if (hats && nhats > 0) {
                switch (hats[0]) {
                case GLFW_HAT_UP:         js->rgdwPOV[0] = 0;     break;
                case GLFW_HAT_RIGHT_UP:   js->rgdwPOV[0] = 4500;  break;
                case GLFW_HAT_RIGHT:      js->rgdwPOV[0] = 9000;  break;
                case GLFW_HAT_RIGHT_DOWN: js->rgdwPOV[0] = 13500; break;
                case GLFW_HAT_DOWN:       js->rgdwPOV[0] = 18000; break;
                case GLFW_HAT_LEFT_DOWN:  js->rgdwPOV[0] = 22500; break;
                case GLFW_HAT_LEFT:       js->rgdwPOV[0] = 27000; break;
                case GLFW_HAT_LEFT_UP:    js->rgdwPOV[0] = 31500; break;
                default: break;
                }
            }
            return DI_OK;
        }
        }
        return DIERR_INVALIDPARAM;
    }

    HRESULT GetDeviceData(DWORD cbData, LPDIDEVICEOBJECTDATA rgdod,
                          LPDWORD count, DWORD flags) override
    {
        // Buffered key EVENTS, as distinct from the polled state above.
        //
        // This used to report nothing, on the reasoning that Orbiter reads the
        // keyboard through GetDeviceState. That is only half true: held keys
        // come from the immediate path, but every ONE-SHOT key is a buffered
        // event. Orbiter::KbdInputBuffered_System reads dod[i].dwOfs as the
        // DIK scan code and acts on dwData & 0x80 for key-down, and that is
        // where OAPI_LKEY_ToggleCamInternal lives -- the internal/external
        // view toggle -- along with pause, quicksave, FOV steps and every
        // dialog shortcut.
        //
        // Returning zero events meant none of those keys ever worked.
        if (!count) return DIERR_INVALIDPARAM;

        if (m_kind != DeviceKind::Keyboard) { *count = 0; return DI_OK; }

        const DWORD wanted = *count;
        DWORD n = 0;

        // Edge detection against the previous poll: a key that is down now and
        // was not before is a key-down event, and the reverse is a key-up.
        // DirectInput's own buffering is a queue filled by the driver; this
        // derives the same events from the state GLFW and the graphics client
        // provide.
        BYTE now[256];
        memset(now, 0, sizeof(now));
        if (GLFWwindow *win = orbiter_GetGLFWWindow()) {
            for (const KeyMap &k : kKeyMap)
                if (glfwGetKey(win, k.glfw) == GLFW_PRESS) now[k.dik] = 0x80;
        }
        if (g_clientKeyState) {
            if (const unsigned char *ck = g_clientKeyState())
                for (int i = 0; i < 256; ++i) now[i] |= ck[i];
        }

        for (int i = 0; i < 256 && n < wanted; ++i) {
            if (now[i] == m_prevKeys[i]) continue;
            if (rgdod) {
                rgdod[n].dwOfs  = DWORD(i);        // the DIK scan code
                rgdod[n].dwData = now[i] ? 0x80 : 0;
                rgdod[n].dwTimeStamp = 0;
                rgdod[n].dwSequence  = m_sequence++;
                rgdod[n].uAppData    = 0;
            }
            ++n;
        }

        // DIGDD_PEEK asks to inspect without consuming, so the previous state
        // is only advanced on a real read.
        if (!(flags & DIGDD_PEEK)) memcpy(m_prevKeys, now, sizeof(now));

        (void)cbData;
        *count = n;
        return DI_OK;
    }

    void setJoystickIndex(int i) { m_joyIndex = i; }

private:
    DeviceKind m_kind;
    bool   m_acquired  = false;
    DWORD  m_bufferSize = 0;
    int    m_joyIndex  = GLFW_JOYSTICK_1;
    BYTE   m_prevKeys[256] = {0};   //!< previous poll, for edge detection
    DWORD  m_sequence = 0;
    double m_lastX = 0, m_lastY = 0;
    bool   m_haveMouse = false;
};

class DirectInput : public IDirectInput8A {
public:
    ULONG Release() override { delete this; return 0; }

    HRESULT CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8 *out,
                         void *) override
    {
        if (!out) return DIERR_INVALIDPARAM;

        DeviceKind kind;
        if      (guid == GUID_SysKeyboard) kind = DeviceKind::Keyboard;
        else if (guid == GUID_SysMouse)    kind = DeviceKind::Mouse;
        else                               kind = DeviceKind::Joystick;

        *out = new Device(kind);
        return DI_OK;
    }

    HRESULT EnumDevices(DWORD devClass, LPDIENUMDEVICESCALLBACKA cb,
                        LPVOID ref, DWORD) override
    {
        if (!cb) return DIERR_INVALIDPARAM;
        // Orbiter enumerates game controllers to build its joystick list. A
        // machine with none simply yields an empty list, which is a normal
        // outcome and the reason Create() must not fail on it.
        if (devClass != DI8DEVCLASS_GAMECTRL && devClass != DI8DEVCLASS_ALL)
            return DI_OK;

        for (int i = GLFW_JOYSTICK_1; i <= GLFW_JOYSTICK_LAST; ++i) {
            if (!glfwJoystickPresent(i)) continue;

            DIDEVICEINSTANCEA inst;
            memset(&inst, 0, sizeof(inst));
            inst.dwSize = sizeof(inst);
            inst.guidInstance = GUID_Joystick;
            inst.guidProduct  = GUID_Joystick;
            // The instance guid's first word carries the index, so a device
            // created from it can be told apart from the others.
            inst.guidInstance.Data1 = (DWORD)i;

            const char *name = glfwGetJoystickName(i);
            snprintf(inst.tszInstanceName, sizeof(inst.tszInstanceName), "%s",
                     name ? name : "Joystick");
            snprintf(inst.tszProductName, sizeof(inst.tszProductName), "%s",
                     name ? name : "Joystick");

            if (cb(&inst, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }
};

} // namespace

// ===========================================================================
// GUIDs and data formats
//
// Only identity matters: CreateDevice compares against these, and the data
// format pointers are passed straight back to SetDataFormat, which ignores
// their contents. The values are the SDK's so that a device guid printed to
// the log matches what a Windows user would see.
// ===========================================================================

extern "C" {

const GUID GUID_SysKeyboard =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
const GUID GUID_SysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
const GUID GUID_Joystick =
    { 0x6F1D2B70, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

const DIDATAFORMAT c_dfDIKeyboard   = { sizeof(DIDATAFORMAT), 0, 0, 256, 0, nullptr };
const DIDATAFORMAT c_dfDIMouse      = { sizeof(DIDATAFORMAT), 0, 0, sizeof(DIMOUSESTATE), 0, nullptr };
const DIDATAFORMAT c_dfDIJoystick2  = { sizeof(DIDATAFORMAT), 0, 0, sizeof(DIJOYSTATE2), 0, nullptr };

HRESULT DirectInput8Create(HINSTANCE, DWORD, REFGUID, LPVOID *out, LPVOID)
{
    if (!out) return DIERR_INVALIDPARAM;
    *out = new DirectInput();
    return DI_OK;
}

} // extern "C"
