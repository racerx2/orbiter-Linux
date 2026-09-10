// Linux <wincodec.h> — the Windows Imaging Component subset Orbiter uses.
//
// Src/Orbiter/GraphicsAPI.cpp uses WIC to load images (BMP/PNG/JPEG/TIFF) into
// HBITMAPs and to encode screenshots. WIC is COM and Windows-only, but unlike
// the embedded-IE control it replaces, its *function* is entirely ordinary:
// decode an image file, scale it, convert pixel format, encode it back out.
// Linux has several good implementations of that, so this is reimplemented
// rather than stubbed out.
//
// The interfaces are declared here exactly as GraphicsAPI.cpp codes against
// them, so that file compiles unmodified. They are pure virtual because that
// is what COM is -- every call is vtable dispatch -- which means no bodies are
// needed to compile or link the callers. The concrete classes live in
// Src/Orbiter/Linux/WinCodec.cpp and are backed by stb_image / stb_image_write.
//
// Only the 18 methods GraphicsAPI.cpp actually calls are declared:
//   factory : CreateStream, CreateDecoderFromFilename, CreateDecoderFromStream,
//             CreateFormatConverter, CreateBitmapScaler, CreateEncoder
//   decoder : GetFrameCount, GetFrame
//   source  : GetSize, CopyPixels
//   scaler  : Initialize
//   convert : Initialize
//   stream  : InitializeFromFilename, InitializeFromMemory
//   encoder : Initialize, CreateNewFrame, Commit
//   frame   : Initialize, SetSize, SetPixelFormat, WritePixels, Commit
//
// COM reference counting is honoured: Release() is real, because
// GraphicsAPI.cpp calls it on every object it creates.

#ifndef ORBITER_LINUX_WINCODEC_H
#define ORBITER_LINUX_WINCODEC_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>
#include <dinput.h>   // for GUID and its comparison operators

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

typedef enum WICBitmapDitherType {
    WICBitmapDitherTypeNone = 0
} WICBitmapDitherType;

typedef enum WICBitmapPaletteType {
    WICBitmapPaletteTypeCustom = 0
} WICBitmapPaletteType;

typedef enum WICBitmapInterpolationMode {
    WICBitmapInterpolationModeNearestNeighbor = 0,
    WICBitmapInterpolationModeLinear          = 1,
    WICBitmapInterpolationModeCubic           = 2,
    WICBitmapInterpolationModeFant            = 3
} WICBitmapInterpolationMode;

typedef enum WICBitmapEncoderCacheOption {
    WICBitmapEncoderCacheInMemory = 0,
    WICBitmapEncoderCacheTempFile = 1,
    WICBitmapEncoderNoCache       = 2
} WICBitmapEncoderCacheOption;

typedef enum WICDecodeOptions {
    WICDecodeMetadataCacheOnDemand = 0,
    WICDecodeMetadataCacheOnLoad   = 1
} WICDecodeOptions;

typedef GUID WICPixelFormatGUID;
typedef GUID *REFWICPixelFormatGUID;

typedef struct WICRect {
    INT X, Y, Width, Height;
} WICRect;

// Access modes for stream/decoder creation
#define GENERIC_READ  0x80000000
#define GENERIC_WRITE 0x40000000

// ---------------------------------------------------------------------------
// Format and container GUIDs
//
// Defined in Src/Orbiter/Linux/WinCodec.cpp. The implementation compares
// against these by identity, so their values only need to be distinct.
// ---------------------------------------------------------------------------

ORB_EXTERN_C_BEGIN

extern const GUID CLSID_WICImagingFactory;

extern const GUID GUID_ContainerFormatBmp;
extern const GUID GUID_ContainerFormatPng;
extern const GUID GUID_ContainerFormatJpeg;
extern const GUID GUID_ContainerFormatTiff;

extern const GUID GUID_WICPixelFormat24bppBGR;
extern const GUID GUID_WICPixelFormat32bppBGR;
extern const GUID GUID_WICPixelFormat32bppBGRA;

ORB_EXTERN_C_END

// ---------------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------------

struct IWICBitmapSource;
struct IWICBitmapFrameDecode;
struct IWICBitmapDecoder;
struct IWICBitmapScaler;
struct IWICFormatConverter;
struct IWICStream;
struct IWICBitmapFrameEncode;
struct IWICBitmapEncoder;
struct IWICImagingFactory;

// Every WIC object derives from IUnknown; only Release is used here.
struct IWICUnknown {
    virtual ULONG Release () = 0;
protected:
    ~IWICUnknown() = default;
};

struct IWICBitmapSource : public IWICUnknown {
    virtual HRESULT GetSize    (UINT *w, UINT *h) = 0;
    virtual HRESULT CopyPixels (const WICRect *rc, UINT stride,
                                UINT bufSize, BYTE *buf) = 0;
};

struct IWICBitmapFrameDecode : public IWICBitmapSource {
};

struct IWICBitmapDecoder : public IWICUnknown {
    virtual HRESULT GetFrameCount (UINT *count) = 0;
    virtual HRESULT GetFrame      (UINT index, IWICBitmapFrameDecode **frame) = 0;
};

struct IWICBitmapScaler : public IWICBitmapSource {
    virtual HRESULT Initialize (IWICBitmapSource *src, UINT w, UINT h,
                                WICBitmapInterpolationMode mode) = 0;
};

struct IWICFormatConverter : public IWICBitmapSource {
    virtual HRESULT Initialize (IWICBitmapSource *src,
                                REFGUID dstFormat,
                                WICBitmapDitherType dither,
                                void *palette,
                                double alphaThreshold,
                                WICBitmapPaletteType paletteType) = 0;
};

struct IWICStream : public IWICUnknown {
    // Wide filenames: GraphicsAPI.cpp builds wchar_t[256] paths for these,
    // because that is what the Windows entry points take.
    virtual HRESULT InitializeFromFilename (LPCWSTR filename, DWORD access) = 0;
    virtual HRESULT InitializeFromMemory   (BYTE *buf, DWORD size) = 0;
};

struct IWICBitmapFrameEncode : public IWICUnknown {
    virtual HRESULT Initialize     (void *encoderOptions) = 0;
    virtual HRESULT SetSize        (UINT w, UINT h) = 0;
    virtual HRESULT SetPixelFormat (WICPixelFormatGUID *fmt) = 0;
    virtual HRESULT WritePixels    (UINT lineCount, UINT stride,
                                    UINT bufSize, BYTE *buf) = 0;
    virtual HRESULT Commit         () = 0;
};

struct IWICBitmapEncoder : public IWICUnknown {
    virtual HRESULT Initialize     (IWICStream *stream,
                                    WICBitmapEncoderCacheOption cache) = 0;
    virtual HRESULT CreateNewFrame (IWICBitmapFrameEncode **frame,
                                    void **encoderOptions) = 0;
    virtual HRESULT Commit         () = 0;
};

struct IWICImagingFactory : public IWICUnknown {
    virtual HRESULT CreateStream               (IWICStream **stream) = 0;
    virtual HRESULT CreateDecoderFromFilename  (LPCWSTR filename, const GUID *vendor,
                                                DWORD access, WICDecodeOptions opt,
                                                IWICBitmapDecoder **decoder) = 0;
    virtual HRESULT CreateDecoderFromStream    (IWICStream *stream, const GUID *vendor,
                                                WICDecodeOptions opt,
                                                IWICBitmapDecoder **decoder) = 0;
    virtual HRESULT CreateFormatConverter      (IWICFormatConverter **conv) = 0;
    virtual HRESULT CreateBitmapScaler         (IWICBitmapScaler **scaler) = 0;
    virtual HRESULT CreateEncoder              (REFGUID containerFormat,
                                                const GUID *vendor,
                                                IWICBitmapEncoder **encoder) = 0;
};

// ---------------------------------------------------------------------------
// Property bag
//
// WIC passes encoder options through an IPropertyBag2. GraphicsAPI.cpp uses it
// to set the JPEG quality: it fills a PROPBAG2 with the option name and a
// VARIANT with a float, then calls Write. Nothing reads options back.
// ---------------------------------------------------------------------------

#define VT_EMPTY 0
#define VT_R4    4
#define VT_R8    5
#define VT_BOOL  11
#define VT_UI1   17

typedef unsigned short VARTYPE;

typedef struct tagVARIANT {
    VARTYPE vt;
    WORD    wReserved1, wReserved2, wReserved3;
    union {
        float  fltVal;
        double dblVal;
        short  boolVal;
        BYTE   bVal;
        LONG   lVal;
        void  *byref;
    };
} VARIANT, *LPVARIANT;

typedef struct tagPROPBAG2 {
    DWORD   dwType;
    VARTYPE vt;
    DWORD   cfType;
    DWORD   dwHint;
    LPWSTR  pstrName;
    GUID    clsid;
} PROPBAG2;

struct IPropertyBag2 {
    virtual ULONG   Release () = 0;
    virtual HRESULT Write   (ULONG cProperties, PROPBAG2 *pPropBag,
                             VARIANT *pvarValue) = 0;
protected:
    ~IPropertyBag2() = default;
};

static inline void VariantInit(VARIANT *v)
{
    if (!v) return;
    memset(v, 0, sizeof(VARIANT));
    v->vt = VT_EMPTY;
}

static inline BOOL IsEqualGUID(REFGUID a, REFGUID b)
{
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}

// ---------------------------------------------------------------------------
// Minimal COM entry points
// ---------------------------------------------------------------------------

#define CLSCTX_INPROC_SERVER 0x1

// IID_PPV_ARGS expands to (riid, ppv) on Windows. There is no interface
// registry here, so the interface id is unused and the factory is chosen by
// the CLSID alone.
#define IID_PPV_ARGS(ppType) ((void **)(ppType))

ORB_EXTERN_C_BEGIN

HRESULT CoCreateInstance (REFGUID clsid, void *outer, DWORD context,
                          void **out);
HRESULT CoInitializeEx   (void *reserved, DWORD flags);
void    CoUninitialize   (void);

ORB_EXTERN_C_END

#endif // ORBITER_LINUX_WINCODEC_H
