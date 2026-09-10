// Linux <ddraw.h> — DirectDraw types, no runtime.
//
// Src/Orbiter/D3dmath.h includes this. The only DirectDraw name reachable
// from here is LPDIRECTDRAWSURFACE7, which d3dtypes.h already forward-declares
// as a pointer to an undefined struct, so this header only has to forward.
//
// Kept as a separate file rather than aliased to d3dtypes.h so that the
// include graph on Linux mirrors the Windows one.

#ifndef ORBITER_LINUX_DDRAW_H
#define ORBITER_LINUX_DDRAW_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <d3dtypes.h>

// DirectDraw return codes referenced by error-checking macros.
#define DD_OK      0

// Src/Orbiter/Log.cpp maps these to strings in LogOut_DDErr for diagnostics.
// The values are the SDK's: MAKE_DDHRESULT(n) = 0x88760000 | n, except for the
// handful that alias standard COM codes. They only ever reach a log line here,
// but keeping the real numbers means a code copied from a Windows bug report
// still resolves to the same name.
#define MAKE_DDHRESULT(code) ((HRESULT)(0x88760000 | (code)))

#define DDERR_GENERIC                       E_FAIL
#define DDERR_INVALIDPARAMS                 E_INVALIDARG
#define DDERR_OUTOFMEMORY                   E_OUTOFMEMORY
#define DDERR_NOTINITIALIZED                ((HRESULT)0x800401F0L)
#define DDERR_ALREADYINITIALIZED            MAKE_DDHRESULT(5)
#define DDERR_CANNOTATTACHSURFACE           MAKE_DDHRESULT(10)
#define DDERR_CANNOTDETACHSURFACE           MAKE_DDHRESULT(20)
#define DDERR_CURRENTLYNOTAVAIL             MAKE_DDHRESULT(40)
#define DDERR_EXCEPTION                     MAKE_DDHRESULT(55)
#define DDERR_HEIGHTALIGN                   MAKE_DDHRESULT(90)
#define DDERR_INCOMPATIBLEPRIMARY           MAKE_DDHRESULT(95)
#define DDERR_INVALIDCAPS                   MAKE_DDHRESULT(100)
#define DDERR_INVALIDCLIPLIST               MAKE_DDHRESULT(110)
#define DDERR_INVALIDMODE                   MAKE_DDHRESULT(120)
#define DDERR_INVALIDOBJECT                 MAKE_DDHRESULT(130)
#define DDERR_INVALIDPIXELFORMAT            MAKE_DDHRESULT(145)
#define DDERR_INVALIDRECT                   MAKE_DDHRESULT(150)
#define DDERR_LOCKEDSURFACES                MAKE_DDHRESULT(160)
#define DDERR_NO3D                          MAKE_DDHRESULT(170)
#define DDERR_NOALPHAHW                     MAKE_DDHRESULT(180)
#define DDERR_NOSTEREOHARDWARE              MAKE_DDHRESULT(181)
#define DDERR_NOSURFACELEFT                 MAKE_DDHRESULT(182)
#define DDERR_NOCLIPLIST                    MAKE_DDHRESULT(205)
#define DDERR_NOCOLORCONVHW                 MAKE_DDHRESULT(210)
#define DDERR_NOCOOPERATIVELEVELSET         MAKE_DDHRESULT(212)
#define DDERR_NOCOLORKEY                    MAKE_DDHRESULT(215)
#define DDERR_NOCOLORKEYHW                  MAKE_DDHRESULT(220)
#define DDERR_NODIRECTDRAWSUPPORT           MAKE_DDHRESULT(222)
#define DDERR_NOEXCLUSIVEMODE               MAKE_DDHRESULT(225)
#define DDERR_NOFLIPHW                      MAKE_DDHRESULT(230)
#define DDERR_NOGDI                         MAKE_DDHRESULT(240)
#define DDERR_NOMIRRORHW                    MAKE_DDHRESULT(250)
#define DDERR_NOTFOUND                      MAKE_DDHRESULT(255)
#define DDERR_NOOVERLAYHW                   MAKE_DDHRESULT(260)
#define DDERR_OVERLAPPINGRECTS              MAKE_DDHRESULT(270)
#define DDERR_NORASTEROPHW                  MAKE_DDHRESULT(280)
#define DDERR_NOROTATIONHW                  MAKE_DDHRESULT(290)
#define DDERR_NOSTRETCHHW                   MAKE_DDHRESULT(310)
#define DDERR_NOT4BITCOLOR                  MAKE_DDHRESULT(316)
#define DDERR_NOT4BITCOLORINDEX             MAKE_DDHRESULT(317)
#define DDERR_NOT8BITCOLOR                  MAKE_DDHRESULT(320)
#define DDERR_NOTEXTUREHW                   MAKE_DDHRESULT(330)
#define DDERR_NOVSYNCHW                     MAKE_DDHRESULT(335)
#define DDERR_NOZBUFFERHW                   MAKE_DDHRESULT(340)
#define DDERR_NOZOVERLAYHW                  MAKE_DDHRESULT(350)
#define DDERR_OUTOFCAPS                     MAKE_DDHRESULT(360)
#define DDERR_OUTOFVIDEOMEMORY              MAKE_DDHRESULT(380)
#define DDERR_OVERLAYCANTCLIP               MAKE_DDHRESULT(382)
#define DDERR_OVERLAYCOLORKEYONLYONEACTIVE  MAKE_DDHRESULT(384)
#define DDERR_PALETTEBUSY                   MAKE_DDHRESULT(387)
#define DDERR_COLORKEYNOTSET                MAKE_DDHRESULT(400)
#define DDERR_SURFACEALREADYATTACHED        MAKE_DDHRESULT(410)
#define DDERR_SURFACEALREADYDEPENDENT       MAKE_DDHRESULT(420)
#define DDERR_SURFACEBUSY                   MAKE_DDHRESULT(430)
#define DDERR_CANTLOCKSURFACE               MAKE_DDHRESULT(435)
#define DDERR_SURFACEISOBSCURED             MAKE_DDHRESULT(440)
#define DDERR_SURFACELOST                   MAKE_DDHRESULT(450)
#define DDERR_SURFACENOTATTACHED            MAKE_DDHRESULT(460)
#define DDERR_TOOBIGHEIGHT                  MAKE_DDHRESULT(470)
#define DDERR_TOOBIGSIZE                    MAKE_DDHRESULT(480)
#define DDERR_TOOBIGWIDTH                   MAKE_DDHRESULT(490)
#define DDERR_UNSUPPORTED                   MAKE_DDHRESULT(500)
#define DDERR_UNSUPPORTEDFORMAT             MAKE_DDHRESULT(510)
#define DDERR_UNSUPPORTEDMASK               MAKE_DDHRESULT(520)
#define DDERR_INVALIDSTREAM                 MAKE_DDHRESULT(521)
#define DDERR_VERTICALBLANKINPROGRESS       MAKE_DDHRESULT(537)
#define DDERR_WASSTILLDRAWING               MAKE_DDHRESULT(540)
#define DDERR_DDSCAPSCOMPLEXREQUIRED        MAKE_DDHRESULT(542)
#define DDERR_XALIGN                        MAKE_DDHRESULT(560)
#define DDERR_INVALIDDIRECTDRAWGUID         MAKE_DDHRESULT(561)
#define DDERR_DIRECTDRAWALREADYCREATED      MAKE_DDHRESULT(562)
#define DDERR_NODIRECTDRAWHW                MAKE_DDHRESULT(563)
#define DDERR_PRIMARYSURFACEALREADYEXISTS   MAKE_DDHRESULT(564)
#define DDERR_NOEMULATION                   MAKE_DDHRESULT(565)
#define DDERR_REGIONTOOSMALL                MAKE_DDHRESULT(566)
#define DDERR_CLIPPERISUSINGHWND            MAKE_DDHRESULT(567)
#define DDERR_NOCLIPPERATTACHED             MAKE_DDHRESULT(568)
#define DDERR_NOHWND                        MAKE_DDHRESULT(569)
#define DDERR_HWNDSUBCLASSED                MAKE_DDHRESULT(570)
#define DDERR_HWNDALREADYSET                MAKE_DDHRESULT(571)
#define DDERR_NOPALETTEATTACHED             MAKE_DDHRESULT(572)
#define DDERR_NOPALETTEHW                   MAKE_DDHRESULT(573)
#define DDERR_BLTFASTCANTCLIP               MAKE_DDHRESULT(574)
#define DDERR_NOBLTHW                       MAKE_DDHRESULT(575)
#define DDERR_NODDROPSHW                    MAKE_DDHRESULT(576)
#define DDERR_OVERLAYNOTVISIBLE             MAKE_DDHRESULT(577)
#define DDERR_NOOVERLAYDEST                 MAKE_DDHRESULT(578)
#define DDERR_INVALIDPOSITION               MAKE_DDHRESULT(579)
#define DDERR_NOTAOVERLAYSURFACE            MAKE_DDHRESULT(580)
#define DDERR_EXCLUSIVEMODEALREADYSET       MAKE_DDHRESULT(581)
#define DDERR_NOTFLIPPABLE                  MAKE_DDHRESULT(582)
#define DDERR_CANTDUPLICATE                 MAKE_DDHRESULT(583)
#define DDERR_NOTLOCKED                     MAKE_DDHRESULT(584)
#define DDERR_CANTCREATEDC                  MAKE_DDHRESULT(585)
#define DDERR_NODC                          MAKE_DDHRESULT(586)
#define DDERR_WRONGMODE                     MAKE_DDHRESULT(587)
#define DDERR_IMPLICITLYCREATED             MAKE_DDHRESULT(588)
#define DDERR_NOTPALETTIZED                 MAKE_DDHRESULT(589)
#define DDERR_UNSUPPORTEDMODE               MAKE_DDHRESULT(590)
#define DDERR_NOMIPMAPHW                    MAKE_DDHRESULT(591)
#define DDERR_INVALIDSURFACETYPE            MAKE_DDHRESULT(592)
#define DDERR_NOOPTIMIZEHW                  MAKE_DDHRESULT(600)
#define DDERR_NOTLOADED                     MAKE_DDHRESULT(601)
#define DDERR_NOFOCUSWINDOW                 MAKE_DDHRESULT(602)
#define DDERR_NOTONMIPMAPSUBLEVEL           MAKE_DDHRESULT(603)
#define DDERR_DCALREADYCREATED              MAKE_DDHRESULT(620)
#define DDERR_NONONLOCALVIDMEM              MAKE_DDHRESULT(630)
#define DDERR_CANTPAGELOCK                  MAKE_DDHRESULT(640)
#define DDERR_CANTPAGEUNLOCK                MAKE_DDHRESULT(660)
#define DDERR_NOTPAGELOCKED                 MAKE_DDHRESULT(680)
#define DDERR_MOREDATA                      MAKE_DDHRESULT(690)
#define DDERR_EXPIRED                       MAKE_DDHRESULT(691)
#define DDERR_TESTFINISHED                  MAKE_DDHRESULT(692)
#define DDERR_NEWMODE                       MAKE_DDHRESULT(693)
#define DDERR_D3DNOTINITIALIZED             MAKE_DDHRESULT(694)
#define DDERR_VIDEONOTACTIVE                MAKE_DDHRESULT(695)
#define DDERR_NOMONITORINFORMATION          MAKE_DDHRESULT(696)
#define DDERR_NODRIVERSUPPORT               MAKE_DDHRESULT(697)
#define DDERR_DEVICEDOESNTOWNSURFACE        MAKE_DDHRESULT(699)

// Driver capability block. Orbiter.cpp declares ConfirmDevice(DDCAPS*, ...)
// as a device-enumeration callback; nothing on Linux enumerates DirectDraw
// devices, so the contents are never read and an incomplete type suffices.
struct _DDCAPS;
typedef struct _DDCAPS DDCAPS, *LPDDCAPS;

// Surface and pixel format descriptors. Src/Orbiter/Texture.{h,cpp} uses these
// when loading DDS textures: the fields carry image dimensions and the
// compressed-format FourCC, both of which are read from the file header, so
// the layouts here match the DirectDraw SDK.

typedef struct _DDPIXELFORMAT {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    union {
        DWORD dwRGBBitCount;
        DWORD dwYUVBitCount;
        DWORD dwZBufferBitDepth;
        DWORD dwAlphaBitDepth;
        DWORD dwLuminanceBitCount;
        DWORD dwBumpBitCount;
    };
    union {
        DWORD dwRBitMask;
        DWORD dwYBitMask;
        DWORD dwStencilBitDepth;
        DWORD dwLuminanceBitMask;
        DWORD dwBumpDuBitMask;
    };
    union {
        DWORD dwGBitMask;
        DWORD dwUBitMask;
        DWORD dwZBitMask;
        DWORD dwBumpDvBitMask;
    };
    union {
        DWORD dwBBitMask;
        DWORD dwVBitMask;
        DWORD dwStencilBitMask;
        DWORD dwBumpLuminanceBitMask;
    };
    union {
        DWORD dwRGBAlphaBitMask;
        DWORD dwYUVAlphaBitMask;
        DWORD dwLuminanceAlphaBitMask;
        DWORD dwRGBZBitMask;
        DWORD dwYUVZBitMask;
    };
} DDPIXELFORMAT, *LPDDPIXELFORMAT;

typedef struct _DDCOLORKEY {
    DWORD dwColorSpaceLowValue;
    DWORD dwColorSpaceHighValue;
} DDCOLORKEY, *LPDDCOLORKEY;

// DUMMYUNIONNAMEN is the Windows SDK's way of naming the anonymous unions in
// these structures when NONAMELESSUNION is defined, and of expanding to
// nothing when it is not. The unions above are written unnamed here, so the
// second behaviour is the right one -- and the client's own copy of
// DDSURFACEDESC2 (OVP/VulkanClient/Texture.cpp) spells its unions with this
// macro, which is why it has to exist rather than simply not be used.
#ifndef DUMMYUNIONNAMEN
#define DUMMYUNIONNAMEN(n)
#endif

// Blit effects block. Pane.cpp fills the fill-colour field when clearing a
// surface, so that member has to exist with the right name.
typedef struct _DDBLTFX {
    DWORD dwSize;
    DWORD dwDDFX;
    DWORD dwROP;
    DWORD dwDDROP;
    DWORD dwRotationAngle;
    DWORD dwZBufferOpCode;
    DWORD dwZBufferLow;
    DWORD dwZBufferHigh;
    DWORD dwZBufferBaseDest;
    DWORD dwZDestConstBitDepth;
    DWORD dwZSrcConstBitDepth;
    DWORD dwAlphaEdgeBlendBitDepth;
    DWORD dwAlphaEdgeBlend;
    DWORD dwReserved;
    DWORD dwAlphaDestConstBitDepth;
    DWORD dwAlphaSrcConstBitDepth;
    union {
        DWORD dwFillColor;
        DWORD dwFillDepth;
        DWORD dwFillPixel;
    };
    DDCOLORKEY ddckDestColorkey;
    DDCOLORKEY ddckSrcColorkey;
} DDBLTFX, *LPDDBLTFX;

// Blit flags
#define DDBLT_COLORFILL   0x00000400
#define DDBLT_KEYSRC      0x00008000
#define DDBLT_WAIT        0x01000000

typedef struct _DDSCAPS2 {
    DWORD dwCaps, dwCaps2, dwCaps3, dwCaps4;
} DDSCAPS2, *LPDDSCAPS2;

typedef struct _DDSURFACEDESC2 {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwHeight;
    DWORD dwWidth;
    union {
        LONG  lPitch;
        DWORD dwLinearSize;
    };
    DWORD dwBackBufferCount;
    union {
        DWORD dwMipMapCount;
        DWORD dwRefreshRate;
        DWORD dwSrcVBHandle;
    };
    DWORD dwAlphaBitDepth;
    DWORD dwReserved;
    LPVOID lpSurface;
    DDCOLORKEY ddckCKDestOverlay;
    DDCOLORKEY ddckCKDestBlt;
    DDCOLORKEY ddckCKSrcOverlay;
    DDCOLORKEY ddckCKSrcBlt;
    DDPIXELFORMAT ddpfPixelFormat;
    DDSCAPS2 ddsCaps;
    DWORD dwTextureStage;
} DDSURFACEDESC2, *LPDDSURFACEDESC2;

// Pixel format flags
#define DDPF_ALPHAPIXELS  0x00000001
#define DDPF_FOURCC       0x00000004
#define DDPF_RGB          0x00000040
#define DDPF_LUMINANCE    0x00020000

// Surface description flags
#define DDSD_CAPS         0x00000001
#define DDSD_HEIGHT       0x00000002
#define DDSD_WIDTH        0x00000004
#define DDSD_PITCH        0x00000008
#define DDSD_PIXELFORMAT  0x00001000
#define DDSD_MIPMAPCOUNT  0x00020000
#define DDSD_LINEARSIZE   0x00080000

// Surface capability flags
#define DDSCAPS_TEXTURE   0x00001000
#define DDSCAPS_MIPMAP    0x00400000
#define DDSCAPS_COMPLEX   0x00000008

// The DirectDraw root object. Texture.h declares
// DDCopyBitmap(LPDIRECTDRAW7, HBITMAP); nothing on Linux creates a DirectDraw
// object, so an incomplete type is enough for the declaration.
struct IDirectDraw7;
typedef struct IDirectDraw7 *LPDIRECTDRAW7;

// Colour key selectors
#define DDCKEY_COLORSPACE  0x00000001
#define DDCKEY_DESTBLT     0x00000002
#define DDCKEY_DESTOVERLAY 0x00000004
#define DDCKEY_SRCBLT      0x00000008
#define DDCKEY_SRCOVERLAY  0x00000010

// Surfaces are handles as far as the core is concerned, but OrbiterAPI.cpp
// calls SetColorKey on one when a vessel sets a transparent panel colour, so
// the interface needs that method. Pure virtual for the same reason as the
// Direct3D device: vtable dispatch, no bodies required to link the callers.
struct IDirectDrawSurface7 {
    virtual ULONG   Release     () = 0;
    virtual HRESULT SetColorKey (DWORD flags, LPDDCOLORKEY key) = 0;
    virtual HRESULT Lock        (LPRECT rc, LPDDSURFACEDESC2 desc,
                                 DWORD flags, HANDLE ev) = 0;
    virtual HRESULT Unlock      (LPRECT rc) = 0;
    virtual HRESULT Blt         (LPRECT dst, IDirectDrawSurface7 *src,
                                 LPRECT srcRc, DWORD flags, LPDDBLTFX fx) = 0;
    virtual HRESULT GetSurfaceDesc (LPDDSURFACEDESC2 desc) = 0;
protected:
    ~IDirectDrawSurface7() = default;
};

#endif // ORBITER_LINUX_DDRAW_H
