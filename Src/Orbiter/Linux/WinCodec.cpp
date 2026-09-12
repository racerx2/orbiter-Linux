// Windows Imaging Component implementation, and resource loading.
//
// GraphicsAPI.cpp uses WIC to decode images into HBITMAPs and to encode
// screenshots. WIC is COM and Windows-only, but what it does is ordinary:
// decode, scale, convert pixel format, encode. That is implemented here over
// stb_image, which ImGui already vendors.
//
// Orbiter asks for GUID_WICPixelFormat24bppBGR or 32bppBGR -- Windows' BGR
// byte order, which is also the order a DIB expects. stb_image decodes to
// RGB(A), so the channel swap happens during conversion. Getting it backwards
// produces images that look plausible with red and blue exchanged.
//
// FindResource/LoadResource/LockResource read from the table rc2cpp.py
// generates. Only bitmaps are looked up this way in this tree, and the
// converter does not currently carry bitmap payloads, so a lookup reports
// "not found" rather than returning a bogus pointer.

#include <windows.h>
#include <wincodec.h>
#include "ResourceTemplates.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include "stb_image.h"
#include "stb_image_write.h"

#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

namespace {

// Converts a wide filename to narrow. The tree's paths are ASCII, so this is
// exact.
std::string narrow(LPCWSTR w)
{
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w, -1, &s[0], n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// A decoded image, shared by every object in a decode chain.
// ---------------------------------------------------------------------------

struct Image {
    int   width = 0, height = 0;
    // Always RGBA8 internally. Converting once on decode keeps the scaler and
    // the format converter simple, and the cost is one copy of an image that
    // is about to be uploaded anyway.
    std::vector<unsigned char> rgba;

    bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

// Area-average resample, separable, one axis at a time.
//
// GraphicsClient::ReadImageFromDecoder -- the path this file serves -- asks
// for WICBitmapInterpolationModeFant, and Fant is an area-average filter, so
// an area average is what the API contract calls for. It matters: the splash
// screen is a 1920x1200 photograph scaled to the viewport at a non-integer
// ratio, and nearest-neighbour on that is visibly stepped along every edge.
//
// One rule covers both directions. The destination pixel i covers the source
// interval [i*scale, (i+1)*scale); each source sample contributes in
// proportion to how much of that interval it occupies. Downscaling averages
// whole pixels, upscaling blends the two the interval straddles, and a 1:1
// ratio reduces to a copy.
void resampleAxis(const std::vector<float> &src, int srcW, int srcH,
                  std::vector<float> &dst, int dstW)
{
    dst.assign((size_t)dstW * srcH * 4, 0.0f);
    const double scale = (double)srcW / (double)dstW;

    for (int i = 0; i < dstW; ++i) {
        const double a = i * scale;
        const double b = (i + 1) * scale;
        const int    i0 = (int)a;
        const int    i1 = (int)(b - 1e-9);      // last source column touched

        for (int y = 0; y < srcH; ++y) {
            double acc[4] = { 0, 0, 0, 0 };
            double wsum   = 0;

            for (int s = i0; s <= i1; ++s) {
                const int sc = (s < 0) ? 0 : (s >= srcW ? srcW - 1 : s);
                // Overlap between [a,b) and this source sample's [s, s+1).
                const double lo = (a > (double)s)       ? a : (double)s;
                const double hi = (b < (double)(s + 1)) ? b : (double)(s + 1);
                double wgt = hi - lo;
                if (wgt <= 0.0) continue;

                const float *p = &src[((size_t)y * srcW + sc) * 4];
                for (int c = 0; c < 4; ++c) acc[c] += wgt * p[c];
                wsum += wgt;
            }

            float *q = &dst[((size_t)y * dstW + i) * 4];
            if (wsum > 0.0)
                for (int c = 0; c < 4; ++c) q[c] = (float)(acc[c] / wsum);
        }
    }
}

// Transpose, so the vertical pass can reuse resampleAxis unchanged.
void transposePlanar(const std::vector<float> &src, int w, int h,
                     std::vector<float> &dst)
{
    dst.assign((size_t)w * h * 4, 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            memcpy(&dst[((size_t)x * h + y) * 4],
                   &src[((size_t)y * w + x) * 4], 4 * sizeof(float));
}

Image resample(const Image &src, int w, int h)
{
    Image out;
    if (!src.valid() || w <= 0 || h <= 0) return out;

    if (w == src.width && h == src.height) {   // nothing to do
        out = src;
        return out;
    }

    // Float throughout: the accumulator has to hold a weighted sum, and
    // rounding each intermediate back to 8 bits would show as banding on the
    // second pass. Each buffer is released as soon as the next pass has
    // consumed it -- at splash-screen sizes one of these is forty megabytes,
    // and holding all four at once would be a hundred and fifty.
    auto release = [](std::vector<float> &v) { std::vector<float>().swap(v); };

    std::vector<float> a((size_t)src.width * src.height * 4);
    for (size_t i = 0; i < a.size(); ++i) a[i] = (float)src.rgba[i];

    std::vector<float> b, c, d;
    resampleAxis(a, src.width, src.height, b, w);   // -> b is w x src.height
    release(a);
    transposePlanar(b, w, src.height, c);           // -> c is src.height x w
    release(b);
    resampleAxis(c, src.height, w, d, h);           // -> d is h x w
    release(c);
    transposePlanar(d, h, w, b);                    // -> b is w x h
    release(d);

    out.width  = w;
    out.height = h;
    out.rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < out.rgba.size(); ++i) {
        const float v = b[i] + 0.5f;
        out.rgba[i] = (unsigned char)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Interface implementations
// ---------------------------------------------------------------------------

// Reach the pixels behind any IWICBitmapSource this file hands out. Every
// decode chain in this tree goes through it -- GraphicsClient's is
//
//     FrameDecode  ->  FormatConverter  ->  Scaler  ->  CopyPixels
//
// and a source type missing from the list below makes the stage that received
// it return E_NOINTERFACE with an empty image, after which CopyPixels writes
// nothing into the DIB CreateDIBSection just zeroed. The caller gets a
// correctly sized, entirely black bitmap and no error anywhere, because
// ReadImageFromDecoder checks none of the four HRESULTs. Hence one function
// rather than a cast chain repeated in each Initialize.
struct BitmapSource;
struct FrameDecode;
struct Scaler;
struct FormatConverter;
const Image *imageOf(IWICBitmapSource *src);

struct BitmapSource : IWICBitmapSource {
    Image image;

    ULONG Release() override { delete this; return 0; }

    HRESULT GetSize(UINT *w, UINT *h) override
    {
        if (!w || !h) return E_INVALIDARG;
        *w = (UINT)image.width;
        *h = (UINT)image.height;
        return S_OK;
    }

    HRESULT CopyPixels(const WICRect *, UINT stride, UINT bufSize,
                       BYTE *buf) override
    {
        // Said out loud rather than only returned as a code nobody reads:
        // ReadImageFromDecoder ignores the HRESULT, so an empty source here
        // leaves the caller's freshly-zeroed DIB untouched and hands back a
        // perfectly sized black picture.
        if (!image.valid())
            fprintf(stderr, "Orbiter: WIC CopyPixels from an empty source -- "
                            "the decode chain did not produce an image\n");
        if (!buf || !image.valid()) return E_INVALIDARG;

        // The caller supplies the stride, which for a DIB is padded to a
        // 4-byte boundary and may exceed width*bpp. Rows are copied
        // individually rather than as one block for that reason.
        const UINT bpp = (stride >= (UINT)image.width * 4) ? 4 : 3;
        for (int y = 0; y < image.height; ++y) {
            BYTE *dstRow = buf + (size_t)y * stride;
            if ((size_t)(y + 1) * stride > bufSize) break;
            const unsigned char *srcRow = &image.rgba[(size_t)y * image.width * 4];
            for (int x = 0; x < image.width; ++x) {
                // RGBA -> BGR(A): Windows pixel formats are blue-first.
                dstRow[x * bpp + 0] = srcRow[x * 4 + 2];
                dstRow[x * bpp + 1] = srcRow[x * 4 + 1];
                dstRow[x * bpp + 2] = srcRow[x * 4 + 0];
                if (bpp == 4) dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
            }
        }
        return S_OK;
    }
};

struct FrameDecode : IWICBitmapFrameDecode {
    Image image;

    ULONG Release() override { delete this; return 0; }

    HRESULT GetSize(UINT *w, UINT *h) override
    {
        if (!w || !h) return E_INVALIDARG;
        *w = (UINT)image.width;
        *h = (UINT)image.height;
        return S_OK;
    }

    HRESULT CopyPixels(const WICRect *rc, UINT stride, UINT bufSize,
                       BYTE *buf) override
    {
        BitmapSource tmp;
        tmp.image = image;
        return tmp.CopyPixels(rc, stride, bufSize, buf);
    }
};

struct Decoder : IWICBitmapDecoder {
    Image image;

    ULONG Release() override { delete this; return 0; }

    HRESULT GetFrameCount(UINT *count) override
    {
        // Multi-frame containers are not used by this tree.
        if (count) *count = 1;
        return S_OK;
    }

    HRESULT GetFrame(UINT, IWICBitmapFrameDecode **frame) override
    {
        if (!frame) return E_INVALIDARG;
        if (!image.valid()) return E_FAIL;
        FrameDecode *f = new FrameDecode();
        f->image = image;
        *frame = f;
        return S_OK;
    }
};

struct Scaler : IWICBitmapScaler {
    Image image;

    ULONG Release() override { delete this; return 0; }

    HRESULT Initialize(IWICBitmapSource *src, UINT w, UINT h,
                       WICBitmapInterpolationMode) override
    {
        if (!src) return E_INVALIDARG;
        UINT sw = 0, sh = 0;
        src->GetSize(&sw, &sh);
        if (!sw || !sh) return E_FAIL;

        // The source is one of this file's own objects, so its pixels are
        // reachable directly rather than through CopyPixels and back.
        const Image *from = imageOf(src);
        if (!from) return E_NOINTERFACE;

        image = resample(*from, (int)w, (int)h);
        return image.valid() ? S_OK : E_FAIL;
    }

    HRESULT GetSize(UINT *w, UINT *h) override
    {
        if (!w || !h) return E_INVALIDARG;
        *w = (UINT)image.width;
        *h = (UINT)image.height;
        return S_OK;
    }

    HRESULT CopyPixels(const WICRect *rc, UINT stride, UINT bufSize,
                       BYTE *buf) override
    {
        BitmapSource tmp;
        tmp.image = image;
        return tmp.CopyPixels(rc, stride, bufSize, buf);
    }
};

struct FormatConverter : IWICFormatConverter {
    Image image;

    ULONG Release() override { delete this; return 0; }

    HRESULT Initialize(IWICBitmapSource *src, REFGUID, WICBitmapDitherType,
                       void *, double, WICBitmapPaletteType) override
    {
        if (!src) return E_INVALIDARG;
        // Everything is held as RGBA internally and converted on the way out
        // in CopyPixels, so a format conversion is a pass-through here.
        const Image *from = imageOf(src);
        if (!from) return E_NOINTERFACE;
        image = *from;
        return S_OK;
    }

    HRESULT GetSize(UINT *w, UINT *h) override
    {
        if (!w || !h) return E_INVALIDARG;
        *w = (UINT)image.width;
        *h = (UINT)image.height;
        return S_OK;
    }

    HRESULT CopyPixels(const WICRect *rc, UINT stride, UINT bufSize,
                       BYTE *buf) override
    {
        BitmapSource tmp;
        tmp.image = image;
        return tmp.CopyPixels(rc, stride, bufSize, buf);
    }
};

const Image *imageOf(IWICBitmapSource *src)
{
    if (auto *bs = dynamic_cast<BitmapSource *>(src))       return &bs->image;
    if (auto *fd = dynamic_cast<FrameDecode *>(src))        return &fd->image;
    if (auto *sc = dynamic_cast<Scaler *>(src))             return &sc->image;
    if (auto *fc = dynamic_cast<FormatConverter *>(src))    return &fc->image;
    return nullptr;
}

struct Stream : IWICStream {
    std::string                filename;
    std::vector<unsigned char> memory;

    ULONG Release() override { delete this; return 0; }

    HRESULT InitializeFromFilename(LPCWSTR name, DWORD) override
    {
        filename = narrow(name);
        return filename.empty() ? E_INVALIDARG : S_OK;
    }

    HRESULT InitializeFromMemory(BYTE *buf, DWORD size) override
    {
        if (!buf) return E_INVALIDARG;
        memory.assign(buf, buf + size);
        return S_OK;
    }
};

struct FrameEncode : IWICBitmapFrameEncode {
    Image        image;
    std::string *target = nullptr;   // owning encoder's output path
    const GUID  *format = nullptr;   // container chosen by the encoder
    // WIC's own JPEG default, so a caller that sets no option gets what
    // Windows would have given it.
    float        quality = 0.9f;
    // The options bag handed out by CreateNewFrame, owned here: GraphicsAPI.cpp
    // never releases it, exactly as the Windows code never does.
    struct PropertyBag *options = nullptr;

    ULONG Release() override;

    HRESULT Initialize(void *) override { return S_OK; }

    HRESULT SetSize(UINT w, UINT h) override
    {
        image.width  = (int)w;
        image.height = (int)h;
        image.rgba.assign((size_t)w * h * 4, 0);
        return S_OK;
    }

    HRESULT SetPixelFormat(WICPixelFormatGUID *) override { return S_OK; }

    HRESULT WritePixels(UINT lineCount, UINT stride, UINT bufSize,
                        BYTE *buf) override
    {
        if (!buf || !image.valid()) return E_INVALIDARG;

        // Incoming rows are BGR or BGRA, matching what the caller captured
        // from the framebuffer.
        const UINT bpp = (stride >= (UINT)image.width * 4) ? 4 : 3;
        for (UINT y = 0; y < lineCount && (int)y < image.height; ++y) {
            if ((size_t)(y + 1) * stride > bufSize) break;
            const BYTE *srcRow = buf + (size_t)y * stride;
            unsigned char *dstRow = &image.rgba[(size_t)y * image.width * 4];
            for (int x = 0; x < image.width; ++x) {
                dstRow[x * 4 + 0] = srcRow[x * bpp + 2];
                dstRow[x * 4 + 1] = srcRow[x * bpp + 1];
                dstRow[x * 4 + 2] = srcRow[x * bpp + 0];
                dstRow[x * 4 + 3] = (bpp == 4) ? srcRow[x * 4 + 3] : 255;
            }
        }
        return S_OK;
    }

    HRESULT Commit() override;
};

// ---------------------------------------------------------------------------
// The encoder options bag.
//
// WIC hands the caller an IPropertyBag2 from CreateNewFrame, and
// GraphicsAPI.cpp writes "ImageQuality" into it before Initialize -- so it has
// to be a real object, not a null pointer the caller would Write through.
//
// The value is carried rather than discarded: WIC's ImageQuality is 0..1 and
// stb_image_write's JPEG quality is 1..100, the same number scaled. Orbiter
// asks for 0.7 for the exit screenshot.
// ---------------------------------------------------------------------------
struct PropertyBag : IPropertyBag2 {
    FrameEncode *frame = nullptr;

    ULONG Release() override { delete this; return 0; }

    HRESULT Write(ULONG cProperties, PROPBAG2 *pPropBag, VARIANT *pvarValue) override
    {
        if (!pPropBag || !pvarValue) return E_INVALIDARG;

        for (ULONG i = 0; i < cProperties; i++) {
            const std::string name = narrow(pPropBag[i].pstrName);
            if (name != "ImageQuality" || !frame) continue;
            if (pvarValue[i].vt == VT_R4)      frame->quality = pvarValue[i].fltVal;
            else if (pvarValue[i].vt == VT_R8) frame->quality = (float)pvarValue[i].dblVal;
        }
        return S_OK;
    }

public:
    ~PropertyBag() = default;
};

ULONG FrameEncode::Release()
{
    delete options;
    options = nullptr;
    delete this;
    return 0;
}

struct Encoder : IWICBitmapEncoder {
    std::string path;
    const GUID *format = nullptr;
    FrameEncode *frame = nullptr;

    ULONG Release() override { delete this; return 0; }

    HRESULT Initialize(IWICStream *stream, WICBitmapEncoderCacheOption) override
    {
        Stream *s = dynamic_cast<Stream *>(stream);
        if (!s) return E_INVALIDARG;
        path = s->filename;
        return path.empty() ? E_INVALIDARG : S_OK;
    }

    HRESULT CreateNewFrame(IWICBitmapFrameEncode **out, void **opts) override
    {
        if (!out) return E_INVALIDARG;
        frame = new FrameEncode();
        frame->target = &path;
        frame->format = format;
        *out = frame;
        // Encoder options are a property bag on Windows, and the caller writes
        // to it, so it has to be a real object.
        if (opts) {
            PropertyBag *bag = new PropertyBag();
            bag->frame    = frame;
            frame->options = bag;
            *opts = (void *)(IPropertyBag2 *)bag;
        }
        return S_OK;
    }

    HRESULT Commit() override { return S_OK; }
};

HRESULT FrameEncode::Commit()
{
    if (!target || target->empty() || !image.valid()) return E_FAIL;

    // The container chosen by CreateEncoder decides the writer. stbi's
    // writers take RGBA directly, which is the order held internally.
    int ok = 0;
    const std::string &p = *target;

    if (format == &GUID_ContainerFormatPng)
        ok = stbi_write_png(p.c_str(), image.width, image.height, 4,
                            image.rgba.data(), image.width * 4);
    else if (format == &GUID_ContainerFormatJpeg) {
        // WIC's ImageQuality is 0..1; stb's is 1..100.
        int q = (int)(quality * 100.0f + 0.5f);
        if (q < 1) q = 1;
        if (q > 100) q = 100;
        ok = stbi_write_jpg(p.c_str(), image.width, image.height, 4,
                            image.rgba.data(), q);
    }
    else
        // BMP and TIFF both fall back to BMP: it is the format Orbiter's
        // screenshot default expects and stbi can write it losslessly.
        ok = stbi_write_bmp(p.c_str(), image.width, image.height, 4,
                            image.rgba.data());

    return ok ? S_OK : E_FAIL;
}

struct Factory : IWICImagingFactory {
    ULONG Release() override { delete this; return 0; }

    HRESULT CreateStream(IWICStream **out) override
    {
        if (!out) return E_INVALIDARG;
        *out = new Stream();
        return S_OK;
    }

    HRESULT CreateDecoderFromFilename(LPCWSTR filename, const GUID *, DWORD,
                                      WICDecodeOptions,
                                      IWICBitmapDecoder **out) override
    {
        if (!out) return E_INVALIDARG;
        const std::string path = narrow(filename);

        int w = 0, h = 0, channels = 0;
        unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
        if (!pixels) return E_FAIL;

        Decoder *d = new Decoder();
        d->image.width  = w;
        d->image.height = h;
        d->image.rgba.assign(pixels, pixels + (size_t)w * h * 4);
        stbi_image_free(pixels);

        *out = d;
        return S_OK;
    }

    HRESULT CreateDecoderFromStream(IWICStream *stream, const GUID *,
                                    WICDecodeOptions,
                                    IWICBitmapDecoder **out) override
    {
        Stream *s = dynamic_cast<Stream *>(stream);
        if (!s || !out) return E_INVALIDARG;

        int w = 0, h = 0, channels = 0;
        unsigned char *pixels = nullptr;

        if (!s->memory.empty()) {
            pixels = stbi_load_from_memory(s->memory.data(),
                                           (int)s->memory.size(),
                                           &w, &h, &channels, 4);
        } else if (!s->filename.empty()) {
            pixels = stbi_load(s->filename.c_str(), &w, &h, &channels, 4);
        }
        if (!pixels) return E_FAIL;

        Decoder *d = new Decoder();
        d->image.width  = w;
        d->image.height = h;
        d->image.rgba.assign(pixels, pixels + (size_t)w * h * 4);
        stbi_image_free(pixels);

        *out = d;
        return S_OK;
    }

    HRESULT CreateFormatConverter(IWICFormatConverter **out) override
    {
        if (!out) return E_INVALIDARG;
        *out = new FormatConverter();
        return S_OK;
    }

    HRESULT CreateBitmapScaler(IWICBitmapScaler **out) override
    {
        if (!out) return E_INVALIDARG;
        *out = new Scaler();
        return S_OK;
    }

    HRESULT CreateEncoder(REFGUID containerFormat, const GUID *,
                          IWICBitmapEncoder **out) override
    {
        if (!out) return E_INVALIDARG;
        Encoder *e = new Encoder();
        // The container is identified by address: the caller passes one of the
        // GUID constants below, and comparing pointers avoids a value compare
        // that would also have to handle unknown formats.
        if      (containerFormat == GUID_ContainerFormatPng)  e->format = &GUID_ContainerFormatPng;
        else if (containerFormat == GUID_ContainerFormatJpeg) e->format = &GUID_ContainerFormatJpeg;
        else if (containerFormat == GUID_ContainerFormatTiff) e->format = &GUID_ContainerFormatTiff;
        else                                                  e->format = &GUID_ContainerFormatBmp;
        *out = e;
        return S_OK;
    }
};

} // namespace

// ===========================================================================
// GUIDs
//
// Real WIC values, so a format guid appearing in a log matches what a Windows
// user would see. Only identity is relied on internally.
// ===========================================================================

extern "C" {

const GUID CLSID_WICImagingFactory =
    { 0xCACAF262, 0x9370, 0x4615, { 0xA1, 0x3B, 0x9F, 0x55, 0x39, 0xDA, 0x4C, 0x0A } };

const GUID GUID_ContainerFormatBmp =
    { 0x0AF1D87E, 0xFCFE, 0x4188, { 0xBD, 0xEB, 0xA7, 0x90, 0x64, 0x71, 0xCB, 0xE3 } };
const GUID GUID_ContainerFormatPng =
    { 0x1B7CFAF4, 0x713F, 0x473C, { 0xBB, 0xCD, 0x61, 0x37, 0x42, 0x5F, 0xAE, 0xAF } };
const GUID GUID_ContainerFormatJpeg =
    { 0x19E4A5AA, 0x5662, 0x4FC5, { 0xA0, 0xC0, 0x17, 0x58, 0x02, 0x8E, 0x10, 0x57 } };
const GUID GUID_ContainerFormatTiff =
    { 0x163BCC30, 0xE2E9, 0x4F0B, { 0x96, 0x1D, 0xA3, 0xE9, 0xFD, 0xB7, 0x88, 0xA3 } };

const GUID GUID_WICPixelFormat24bppBGR =
    { 0x6FDDC324, 0x4E03, 0x4BFE, { 0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x0C } };
const GUID GUID_WICPixelFormat32bppBGR =
    { 0x6FDDC324, 0x4E03, 0x4BFE, { 0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x0E } };
const GUID GUID_WICPixelFormat32bppBGRA =
    { 0x6FDDC324, 0x4E03, 0x4BFE, { 0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x0F } };

HRESULT CoCreateInstance(REFGUID clsid, void *, DWORD, void **out)
{
    if (!out) return E_INVALIDARG;
    if (clsid == CLSID_WICImagingFactory) {
        *out = new Factory();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

HRESULT CoInitializeEx(void *, DWORD) { return S_OK; }
void    CoUninitialize(void)          { }

// ===========================================================================
// Resource loading
//
// Bitmap resources are served from the table rc2cpp.py generates, which
// carries the .bmp bytes exactly as rc.exe embeds them on Windows. The handle
// returned by FindResource is the BitmapResource itself, LoadResource is a
// no-op that passes it through, and LockResource yields the bytes -- the same
// three-step dance the Windows API uses.
// ===========================================================================

HANDLE FindResourceA(HMODULE, LPCSTR name, LPCSTR type)
{
    // Resource ids arrive packed into the pointer by MAKEINTRESOURCE.
    //
    // The type has to be consulted: ids are not unique across types, because
    // on Windows a resource is keyed by (type, id). resource.h defines both
    // IDI_FINGER2 and IDR_IMAGE1 as 292, so a type-blind lookup for the image
    // would return whichever came first in the table -- a finger cursor where
    // the splash screen should be.
    //
    // A named (non-integer) type is passed through as the string; Orbiter uses
    // "TEXT" and "IMAGE" that way.
    if (!IS_INTRESOURCE(name)) return nullptr;
    const int id = (int)(uintptr_t)name;
    const char *typeName = IS_INTRESOURCE(type) ? nullptr : (const char *)type;
    return (HANDLE)orbiter_res::FindResourceOfType(id, typeName);
}

HANDLE LoadResource(HMODULE, HANDLE res)
{
    // The data is already in the image; there is nothing to load.
    return res;
}

LPVOID LockResource(HANDLE resData)
{
    const auto *r = (const orbiter_res::BitmapResource *)resData;
    return r ? (LPVOID)r->data : nullptr;
}

DWORD SizeofResource(HMODULE, HANDLE res)
{
    const auto *r = (const orbiter_res::BitmapResource *)res;
    return r ? (DWORD)r->size : 0;
}

} // extern "C"
