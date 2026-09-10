// BMP decoding for embedded resources.
//
// Orbiter's bitmaps are the ones rc.exe embeds: 4- and 8-bit palettised
// Windows 3.x BMPs, plus a couple of 24-bit ones. stb_image handles 24- and
// 32-bit BMP but not sub-8-bit palettised, and those are the majority here
// (Folder1.bmp is 4-bit, banner.bmp is 8-bit), so the decode is done directly.
//
// This is a decoder for exactly the format the resources are in, not a general
// BMP reader: BITMAPINFOHEADER, bottom-up, uncompressed or RLE8. Anything else
// is reported as unsupported rather than half-decoded, because a silently
// wrong image is harder to notice than a missing one.
//
// OUTPUT
//   RGBA8, top-down -- the orientation a texture upload wants, and the inverse
//   of BMP's own bottom-up storage.

#include <windows.h>
#include "ResourceTemplates.h"

#include <vector>
#include <cstring>

// Declared, not implemented, here. The implementation lives in
// Src/Orbiter/Linux/WinCodec.cpp, which defines STB_IMAGE_IMPLEMENTATION for
// the whole executable; including stb_image.h again would either duplicate the
// definitions or, without the macro, add a second copy of the declarations.
// Only the ICO decoder's PNG branch needs these two.
extern "C" unsigned char *stbi_load_from_memory(unsigned char const *buffer,
                                                int len, int *x, int *y,
                                                int *channels_in_file,
                                                int desired_channels);
extern "C" void stbi_image_free(void *retval_from_stbi_load);

namespace orbiter_res {

namespace {

// Little-endian readers. BMP is defined as little-endian regardless of host,
// so the bytes are assembled explicitly rather than cast.
inline uint16_t rd16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
inline uint32_t rd32(const unsigned char *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

} // namespace

bool DecodeBMP(const unsigned char *data, int size,
               std::vector<unsigned char> &rgba, int &width, int &height)
{
    // File header is 14 bytes, then the info header.
    if (!data || size < 54) return false;
    if (data[0] != 'B' || data[1] != 'M') return false;

    const uint32_t pixelOffset = rd32(data + 10);
    const uint32_t headerSize  = rd32(data + 14);
    if (headerSize < 40) return false;          // not BITMAPINFOHEADER

    const int32_t  w        = (int32_t)rd32(data + 18);
    const int32_t  h        = (int32_t)rd32(data + 22);
    const uint16_t bpp      = rd16(data + 28);
    const uint32_t compress = rd32(data + 30);
    uint32_t       paletteN = rd32(data + 46);

    if (w <= 0 || h == 0) return false;

    // A negative height means the rows are stored top-down; the magnitude is
    // the real height either way.
    const bool topDown = h < 0;
    const int  H = topDown ? -h : h;
    const int  W = w;

    // Only uncompressed and RLE8 appear in these resources.
    if (compress != BI_RGB && compress != BI_RLE8) return false;

    // The palette follows the info header. A zero count means the maximum for
    // the depth, which is what most writers emit.
    const unsigned char *palette = data + 14 + headerSize;
    if (bpp <= 8 && paletteN == 0) paletteN = 1u << bpp;
    if (bpp <= 8) {
        const size_t need = (size_t)(palette - data) + paletteN * 4;
        if (need > (size_t)size) return false;
    }

    rgba.assign((size_t)W * H * 4, 0);
    width  = W;
    height = H;

    // Palette entries are BGRA, and the fourth byte is padding rather than
    // alpha -- treating it as alpha would make every palettised image
    // transparent.
    auto putPalette = [&](int x, int y, unsigned index) {
        if (index >= paletteN) index = 0;
        const unsigned char *e = palette + index * 4;
        const int row = topDown ? y : (H - 1 - y);
        unsigned char *o = &rgba[((size_t)row * W + x) * 4];
        o[0] = e[2];   // R
        o[1] = e[1];   // G
        o[2] = e[0];   // B
        o[3] = 255;
    };

    if (compress == BI_RLE8) {
        // Run-length encoded 8-bit. Used by at least one of the larger
        // resources; the encoding is a sequence of (count, index) pairs with
        // escape codes for line ends and absolute runs.
        const unsigned char *p   = data + pixelOffset;
        const unsigned char *end = data + size;
        int x = 0, y = 0;

        while (p + 1 < end && y < H) {
            const unsigned char count = *p++;
            const unsigned char value = *p++;

            if (count > 0) {
                for (int i = 0; i < count && x < W; ++i, ++x)
                    putPalette(x, y, value);
                continue;
            }
            if (value == 0)      { x = 0; ++y; continue; }   // end of line
            if (value == 1)      break;                      // end of bitmap
            if (value == 2) {                                // delta
                if (p + 1 >= end) break;
                x += *p++;
                y += *p++;
                continue;
            }
            // Absolute mode: `value` literal indices, padded to a word.
            for (int i = 0; i < value && p < end; ++i, ++p)
                if (x < W) putPalette(x++, y, *p);
            if (value & 1) ++p;   // odd runs are padded
        }
        return true;
    }

    // Uncompressed. Rows are padded to a 4-byte boundary.
    const int rowBytes = ((W * bpp + 31) / 32) * 4;
    if ((size_t)pixelOffset + (size_t)rowBytes * H > (size_t)size) return false;

    for (int y = 0; y < H; ++y) {
        const unsigned char *row = data + pixelOffset + (size_t)y * rowBytes;

        switch (bpp) {
        case 1:
            for (int x = 0; x < W; ++x)
                putPalette(x, y, (row[x >> 3] >> (7 - (x & 7))) & 1);
            break;

        case 4:
            // Two pixels per byte, high nibble first.
            for (int x = 0; x < W; ++x) {
                const unsigned char b = row[x >> 1];
                putPalette(x, y, (x & 1) ? (b & 0x0F) : (b >> 4));
            }
            break;

        case 8:
            for (int x = 0; x < W; ++x)
                putPalette(x, y, row[x]);
            break;

        case 24:
        case 32: {
            const int stride = bpp / 8;
            for (int x = 0; x < W; ++x) {
                const unsigned char *s = row + x * stride;
                const int dstRow = topDown ? y : (H - 1 - y);
                unsigned char *o = &rgba[((size_t)dstRow * W + x) * 4];
                o[0] = s[2];   // stored BGR
                o[1] = s[1];
                o[2] = s[0];
                o[3] = (bpp == 32) ? s[3] : 255;
            }
            break;
        }

        default:
            return false;   // unsupported depth
        }
    }
    return true;
}

// ===========================================================================
// ICO
// ===========================================================================
//
// An .ico is a small directory of images, and rc.exe embeds the whole file for
// an ICON resource exactly as it does for a BITMAP. Windows then picks a size
// out of it inside LoadIcon; here the caller says which one it wants.
//
// Orbiter.ico carries six: 16, 24, 32, 48 and 64 as 32-bit DIBs, and 256 as a
// PNG. Both encodings occur in one file, which is normal for icons written
// since Vista, so both are handled -- a decoder that assumed DIB would silently
// skip the largest image, which is the one a modern desktop actually wants.
//
// THE DIB IN AN ICON IS NOT A .BMP. It has no 14-byte file header, and its
// BITMAPINFOHEADER declares DOUBLE the real height: the extra half is the
// 1-bit AND mask that predates alpha channels. DecodeBMP above cannot be
// reused for that reason, and feeding it these bytes returns a half-height
// image rather than an error.
//
// For 32-bit entries the alpha channel is authoritative and the AND mask is
// ignored, which is what every renderer since XP does.

int CountICOImages(const unsigned char *data, int size)
{
    if (!data || size < 6) return 0;
    if (rd16(data) != 0 || rd16(data + 2) != 1) return 0;   // reserved, type=1
    const int count = rd16(data + 4);
    if (count < 0 || 6 + count * 16 > size) return 0;
    return count;
}

bool GetICOImageSize(const unsigned char *data, int size, int index,
                     int &width, int &height)
{
    const int count = CountICOImages(data, size);
    if (index < 0 || index >= count) return false;
    const unsigned char *e = data + 6 + index * 16;
    // A stored dimension of 0 means 256 -- the field is one byte.
    width  = e[0] ? e[0] : 256;
    height = e[1] ? e[1] : 256;
    return true;
}

bool DecodeICO(const unsigned char *data, int size, int index,
               std::vector<unsigned char> &rgba, int &width, int &height)
{
    const int count = CountICOImages(data, size);
    if (index < 0 || index >= count) return false;

    const unsigned char *e = data + 6 + index * 16;
    const uint32_t bytes  = rd32(e + 8);
    const uint32_t offset = rd32(e + 12);
    if (offset + bytes > (uint32_t)size || bytes < 8) return false;

    const unsigned char *img = data + offset;

    // PNG-encoded entry. stb_image is already in this executable
    // (Src/Orbiter/Linux/WinCodec.cpp defines STB_IMAGE_IMPLEMENTATION), so
    // this costs no new dependency.
    static const unsigned char kPngSig[8] =
        { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    if (memcmp(img, kPngSig, 8) == 0) {
        int w = 0, h = 0, comp = 0;
        unsigned char *px = stbi_load_from_memory(img, (int)bytes, &w, &h,
                                                  &comp, 4);
        if (!px) return false;
        rgba.assign(px, px + (size_t)w * h * 4);
        stbi_image_free(px);
        width = w; height = h;
        return true;
    }

    // Otherwise a BITMAPINFOHEADER followed by pixels, with no file header.
    if (bytes < 40) return false;
    const uint32_t headerSize = rd32(img);
    if (headerSize < 40) return false;

    const int32_t  w   = (int32_t)rd32(img + 4);
    const int32_t  h2  = (int32_t)rd32(img + 8);   // twice the real height
    const uint16_t bpp = rd16(img + 14);
    const uint32_t comp = rd32(img + 16);
    if (w <= 0 || h2 <= 0 || comp != 0) return false;

    const int H = h2 / 2;
    if (H <= 0) return false;

    // Only the 32-bit form is decoded. Every entry in Orbiter.ico is 32-bit,
    // and a palettised icon would need the colour table and the AND mask
    // handled together -- unsupported is better than approximated.
    if (bpp != 32) return false;

    const size_t need = (size_t)40 + (size_t)w * H * 4;
    if (bytes < need) return false;

    const unsigned char *px = img + headerSize;
    rgba.assign((size_t)w * H * 4, 0);

    // Bottom-up, BGRA.
    for (int y = 0; y < H; ++y) {
        const unsigned char *s = px + (size_t)y * w * 4;
        unsigned char *o = &rgba[((size_t)(H - 1 - y) * w) * 4];
        for (int x = 0; x < w; ++x) {
            o[x * 4 + 0] = s[x * 4 + 2];
            o[x * 4 + 1] = s[x * 4 + 1];
            o[x * 4 + 2] = s[x * 4 + 0];
            o[x * 4 + 3] = s[x * 4 + 3];
        }
    }

    width = w; height = H;
    return true;
}

} // namespace orbiter_res
