// Linux <dinput.h> — the DirectInput 8 subset Orbiter uses.
//
// Di7frame.{h,cpp} wraps DirectInput for keyboard, mouse and joystick. This
// header supplies the interface it codes against; the implementation in
// DInput.cpp is backed by GLFW. The interfaces are pure virtual, matching what
// DirectInput really is (COM), so callers link against vtable dispatch with no
// stub bodies.

#ifndef ORBITER_LINUX_DINPUT_H
#define ORBITER_LINUX_DINPUT_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

#define DIRECTINPUT_VERSION 0x0800

// GetDeviceData flag: inspect the buffered events without consuming them.
#define DIGDD_PEEK 0x00000001

// ---------------------------------------------------------------------------
// GUIDs
//
// Real GUIDs so device identity comparisons behave; Di7frame.cpp compares
// against GUID_SysKeyboard and GUID_SysMouse with operator==.
// ---------------------------------------------------------------------------

typedef struct _GUID {
    DWORD Data1;
    WORD  Data2;
    WORD  Data3;
    BYTE  Data4[8];
} GUID;

inline bool operator==(const GUID &a, const GUID &b) {
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}
inline bool operator!=(const GUID &a, const GUID &b) { return !(a == b); }

typedef GUID *LPGUID;
typedef const GUID *LPCGUID;
typedef GUID IID;
typedef GUID CLSID;
typedef const GUID &REFGUID;

extern "C" {
extern const GUID GUID_SysKeyboard;
extern const GUID GUID_SysMouse;
extern const GUID GUID_Joystick;
}

// ---------------------------------------------------------------------------
// Return codes
// ---------------------------------------------------------------------------

#define DI_OK                  ((HRESULT)0)
#define DI_NOEFFECT            ((HRESULT)1)
#define DI_BUFFEROVERFLOW      ((HRESULT)1)
#define DIERR_INPUTLOST        ((HRESULT)0x8007001EL)
#define DIERR_NOTACQUIRED      ((HRESULT)0x8007000CL)
#define DIERR_INVALIDPARAM     ((HRESULT)0x80070057L)
#define DIERR_OTHERAPPHASPRIO  ((HRESULT)0x80070005L)
#define DIERR_NOTINITIALIZED   ((HRESULT)0x80070015L)

// Log.cpp's LogOut_DIErr switches on these, so every value below must be
// distinct or the switch will not compile. Most alias a Win32 error in the
// 0x8007xxxx range; the two force-feedback codes are in DirectInput's own
// 0x8004xxxx facility.
#define DIERR_OBJECTNOTFOUND   ((HRESULT)0x80070002L)  // ERROR_FILE_NOT_FOUND
#define DIERR_UNSUPPORTED      ((HRESULT)0x80004001L)  // E_NOTIMPL
#define DIERR_DEVICENOTREG     ((HRESULT)0x80040154L)  // REGDB_E_CLASSNOTREG
#define DIERR_ACQUIRED         ((HRESULT)0x800700AAL)  // ERROR_BUSY
#define DIERR_UNPLUGGED        ((HRESULT)0x80040209L)
#define DIERR_REPORTFULL       ((HRESULT)0x8004020AL)

// ---------------------------------------------------------------------------
// Cooperative level flags
// ---------------------------------------------------------------------------

#define DISCL_EXCLUSIVE     0x00000001
#define DISCL_NONEXCLUSIVE  0x00000002
#define DISCL_FOREGROUND    0x00000004
#define DISCL_BACKGROUND    0x00000008

// ---------------------------------------------------------------------------
// Device classes and enumeration flags
// ---------------------------------------------------------------------------

#define DI8DEVCLASS_ALL         0
#define DI8DEVCLASS_DEVICE      1
#define DI8DEVCLASS_POINTER     2
#define DI8DEVCLASS_KEYBOARD    3
#define DI8DEVCLASS_GAMECTRL    4

#define DIEDFL_ALLDEVICES       0x00000000
#define DIEDFL_ATTACHEDONLY     0x00000001

#define DIENUM_STOP             0
#define DIENUM_CONTINUE         1

// ---------------------------------------------------------------------------
// Property access
// ---------------------------------------------------------------------------

#define DIPH_DEVICE     0
#define DIPH_BYOFFSET   1
#define DIPH_BYID       2
#define DIPH_BYUSAGE    3

typedef struct DIPROPHEADER {
    DWORD dwSize;
    DWORD dwHeaderSize;
    DWORD dwObj;
    DWORD dwHow;
} DIPROPHEADER, *LPDIPROPHEADER;

typedef struct DIPROPDWORD {
    DIPROPHEADER diph;
    DWORD        dwData;
} DIPROPDWORD, *LPDIPROPDWORD;

typedef struct DIPROPRANGE {
    DIPROPHEADER diph;
    LONG         lMin;
    LONG         lMax;
} DIPROPRANGE, *LPDIPROPRANGE;

// Property ids are MAKEINTRESOURCE-style small integers in the SDK.
#define DIPROP_BUFFERSIZE   ((LPCGUID)1)
#define DIPROP_AXISMODE     ((LPCGUID)2)
#define DIPROP_RANGE        ((LPCGUID)4)
#define DIPROP_DEADZONE     ((LPCGUID)5)
#define DIPROP_SATURATION   ((LPCGUID)6)

#define DIPROPAXISMODE_ABS  0
#define DIPROPAXISMODE_REL  1

// Axis offsets within DIJOYSTATE2. Orbiter uses these with DIPH_BYOFFSET to
// set per-axis range and deadzone, so they must match the struct layout above.
#define DIJOFS_X        (offsetof(DIJOYSTATE2, lX))
#define DIJOFS_Y        (offsetof(DIJOYSTATE2, lY))
#define DIJOFS_Z        (offsetof(DIJOYSTATE2, lZ))
#define DIJOFS_RX       (offsetof(DIJOYSTATE2, lRx))
#define DIJOFS_RY       (offsetof(DIJOYSTATE2, lRy))
#define DIJOFS_RZ       (offsetof(DIJOYSTATE2, lRz))
#define DIJOFS_SLIDER(n) (offsetof(DIJOYSTATE2, rglSlider) + (n) * sizeof(LONG))
#define DIJOFS_POV(n)    (offsetof(DIJOYSTATE2, rgdwPOV)   + (n) * sizeof(DWORD))
#define DIJOFS_BUTTON(n) (offsetof(DIJOYSTATE2, rgbButtons) + (n))

// ---------------------------------------------------------------------------
// Device state structures
// ---------------------------------------------------------------------------

typedef struct DIJOYSTATE2 {
    LONG  lX, lY, lZ;
    LONG  lRx, lRy, lRz;
    LONG  rglSlider[2];
    DWORD rgdwPOV[4];
    BYTE  rgbButtons[128];
    LONG  lVX, lVY, lVZ;
    LONG  lVRx, lVRy, lVRz;
    LONG  rglVSlider[2];
    LONG  lAX, lAY, lAZ;
    LONG  lARx, lARy, lARz;
    LONG  rglASlider[2];
    LONG  lFX, lFY, lFZ;
    LONG  lFRx, lFRy, lFRz;
    LONG  rglFSlider[2];
} DIJOYSTATE2, *LPDIJOYSTATE2;

typedef struct DIMOUSESTATE {
    LONG lX, lY, lZ;
    BYTE rgbButtons[4];
} DIMOUSESTATE, *LPDIMOUSESTATE;

typedef struct DIDEVICEOBJECTDATA {
    DWORD     dwOfs;
    DWORD     dwData;
    DWORD     dwTimeStamp;
    DWORD     dwSequence;
    UINT_PTR  uAppData;
} DIDEVICEOBJECTDATA, *LPDIDEVICEOBJECTDATA;

#define MAX_PATH_DI 260

typedef struct DIDEVICEINSTANCEA {
    DWORD dwSize;
    GUID  guidInstance;
    GUID  guidProduct;
    DWORD dwDevType;
    CHAR  tszInstanceName[MAX_PATH_DI];
    CHAR  tszProductName[MAX_PATH_DI];
    GUID  guidFFDriver;
    WORD  wUsagePage;
    WORD  wUsage;
} DIDEVICEINSTANCEA, *LPDIDEVICEINSTANCEA;

typedef DIDEVICEINSTANCEA DIDEVICEINSTANCE, *LPDIDEVICEINSTANCE;
typedef const DIDEVICEINSTANCEA *LPCDIDEVICEINSTANCE;

// Data-format descriptors. Orbiter passes the address of one of the three
// standard formats straight through to SetDataFormat; the implementation
// recognises them by identity, so the contents are opaque.
typedef struct _DIDATAFORMAT {
    DWORD dwSize;
    DWORD dwObjSize;
    DWORD dwFlags;
    DWORD dwDataSize;
    DWORD dwNumObjs;
    void *rgodf;
} DIDATAFORMAT, *LPDIDATAFORMAT;
typedef const DIDATAFORMAT *LPCDIDATAFORMAT;

extern "C" {
extern const DIDATAFORMAT c_dfDIKeyboard;
extern const DIDATAFORMAT c_dfDIMouse;
extern const DIDATAFORMAT c_dfDIJoystick2;
}

// ---------------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------------

struct IDirectInputDevice8A;
typedef struct IDirectInputDevice8A *LPDIRECTINPUTDEVICE8;

typedef BOOL (CALLBACK *LPDIENUMDEVICESCALLBACKA)(LPCDIDEVICEINSTANCE, VOID *);

struct IDirectInputDevice8A {
    virtual ULONG   Release             () = 0;
    virtual HRESULT SetDataFormat       (LPCDIDATAFORMAT fmt) = 0;
    virtual HRESULT SetCooperativeLevel (HWND hwnd, DWORD flags) = 0;
    virtual HRESULT SetProperty         (LPCGUID prop, LPDIPROPHEADER hdr) = 0;
    virtual HRESULT Acquire             () = 0;
    virtual HRESULT Unacquire           () = 0;
    virtual HRESULT GetDeviceState      (DWORD cb, LPVOID data) = 0;
    virtual HRESULT GetDeviceData       (DWORD cbObjectData,
                                         LPDIDEVICEOBJECTDATA rgdod,
                                         LPDWORD pdwInOut, DWORD dwFlags) = 0;
    // Poll is required for buffered/polled devices; joysticks need it before
    // each GetDeviceState.
    virtual HRESULT Poll                () = 0;
protected:
    ~IDirectInputDevice8A() = default;
};

struct IDirectInput8A;
typedef struct IDirectInput8A *LPDIRECTINPUT8;

struct IDirectInput8A {
    virtual ULONG   Release      () = 0;
    virtual HRESULT CreateDevice (REFGUID guid, LPDIRECTINPUTDEVICE8 *dev,
                                  void *outer) = 0;
    virtual HRESULT EnumDevices  (DWORD devClass, LPDIENUMDEVICESCALLBACKA cb,
                                  LPVOID ref, DWORD flags) = 0;
protected:
    ~IDirectInput8A() = default;
};

extern "C" {
HRESULT DirectInput8Create (HINSTANCE hinst, DWORD version, REFGUID riid,
                            LPVOID *out, LPVOID outer);
}

#define IID_IDirectInput8 GUID_SysKeyboard  // unused by this tree

// ---------------------------------------------------------------------------
// Keyboard scan codes
//
// These are DirectInput scan codes, not ASCII: they identify a physical key
// position and are the values Orbiter stores in its key map and compares
// against the 256-byte keyboard state buffer. The numbers are the standard
// set-1 scan codes and must match exactly, because Orbiter's default key
// bindings and saved keymap files are written in terms of them. The input
// implementation translates GLFW key codes into this numbering.
// ---------------------------------------------------------------------------

#define DIK_ESCAPE      0x01
#define DIK_1           0x02
#define DIK_2           0x03
#define DIK_3           0x04
#define DIK_4           0x05
#define DIK_5           0x06
#define DIK_6           0x07
#define DIK_7           0x08
#define DIK_8           0x09
#define DIK_9           0x0A
#define DIK_0           0x0B
#define DIK_MINUS       0x0C
#define DIK_EQUALS      0x0D
#define DIK_BACK        0x0E
#define DIK_TAB         0x0F
#define DIK_Q           0x10
#define DIK_W           0x11
#define DIK_E           0x12
#define DIK_R           0x13
#define DIK_T           0x14
#define DIK_Y           0x15
#define DIK_U           0x16
#define DIK_I           0x17
#define DIK_O           0x18
#define DIK_P           0x19
#define DIK_LBRACKET    0x1A
#define DIK_RBRACKET    0x1B
#define DIK_RETURN      0x1C
#define DIK_LCONTROL    0x1D
#define DIK_A           0x1E
#define DIK_S           0x1F
#define DIK_D           0x20
#define DIK_F           0x21
#define DIK_G           0x22
#define DIK_H           0x23
#define DIK_J           0x24
#define DIK_K           0x25
#define DIK_L           0x26
#define DIK_SEMICOLON   0x27
#define DIK_APOSTROPHE  0x28
#define DIK_GRAVE       0x29
#define DIK_LSHIFT      0x2A
#define DIK_BACKSLASH   0x2B
#define DIK_Z           0x2C
#define DIK_X           0x2D
#define DIK_C           0x2E
#define DIK_V           0x2F
#define DIK_B           0x30
#define DIK_N           0x31
#define DIK_M           0x32
#define DIK_COMMA       0x33
#define DIK_PERIOD      0x34
#define DIK_SLASH       0x35
#define DIK_RSHIFT      0x36
#define DIK_LMENU       0x38
#define DIK_SPACE       0x39
#define DIK_CAPITAL     0x3A
#define DIK_F1          0x3B
#define DIK_F2          0x3C
#define DIK_F3          0x3D
#define DIK_F4          0x3E
#define DIK_F5          0x3F
#define DIK_F6          0x40
#define DIK_F7          0x41
#define DIK_F8          0x42
#define DIK_F9          0x43
#define DIK_F10         0x44
#define DIK_NUMLOCK     0x45
#define DIK_SCROLL      0x46
#define DIK_OEM_102     0x56
#define DIK_F11         0x57
#define DIK_F12         0x58
#define DIK_NUMPAD7     0x47
#define DIK_NUMPAD8     0x48
#define DIK_NUMPAD9     0x49
#define DIK_NUMPAD4     0x4B
#define DIK_NUMPAD5     0x4C
#define DIK_NUMPAD6     0x4D
#define DIK_NUMPAD1     0x4F
#define DIK_NUMPAD2     0x50
#define DIK_NUMPAD3     0x51
#define DIK_NUMPAD0     0x52

// Keypad operators and the remaining navigation keys. The graphics client
// routes its own window's key events into this same DIK-indexed state buffer,
// and Orbiter's camera controls use the keypad heavily.
#define DIK_SUBTRACT    0x4A
#define DIK_ADD         0x4E
#define DIK_MULTIPLY    0x37
#define DIK_DIVIDE      0xB5
#define DIK_DECIMAL     0x53
#define DIK_NUMPADENTER 0x9C
#define DIK_PRIOR       0xC9    /* PageUp */
#define DIK_NEXT        0xD1    /* PageDown */
#define DIK_INSERT      0xD2
#define DIK_DELETE      0xD3
#define DIK_RCONTROL    0x9D
#define DIK_RMENU       0xB8
#define DIK_HOME        0xC7
#define DIK_UP          0xC8
#define DIK_LEFT        0xCB
#define DIK_RIGHT       0xCD
#define DIK_END         0xCF
#define DIK_DOWN        0xD0
#define DIK_SYSRQ       0xB7    /* SysRq / PrtScn */
#define DIK_PAUSE       0xC5

#endif // ORBITER_LINUX_DINPUT_H
