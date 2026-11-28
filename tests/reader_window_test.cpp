/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
 * (C) 2018-2021 The Natron Developers
 * (C) 2013-2018 INRIA
 *
 * openfx-io is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-io is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-io.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */
/*
 * reader_window_test — render windows and render scales of ReadOIIO, ReadPFM and ReadPNG (issues #160, #163, #232,
 * #234, #371), driven by the in-process host (ofx_test_host.h). Built with AddressSanitizer and UBSan's
 * signed-integer-overflow check (ctest test-reader-window), and the output image is between two guard pages (PROT_NONE), flush against one of them, so that a write out of it
 * crashes.
 *
 *  - render scale 1/2: GenericReader decodes a full-resolution window into its buffer, then downscales it; decode()
 *    gets a render scale of 1 (ReadOIIO and ReadPFM assert it, #160)
 *  - render windows narrower than the image (a few columns, a few rows, a box, a pixel, at the edges), at scale 1
 *    (decode() writes in the output image, or in GenericReader's buffer if the image is premultiplied by
 *    GenericReader) and 1/2 (GenericReader's buffer): only the window is written, with the pixels of the whole
 *    image's render (#163). ReadOIIO supports tiles: also with an output image that is the render window only.
 *    (ReadOIIO reads with OpenImageIO's ImageInput.)
 *  - render windows partly or entirely out of the RoD, in an image that covers the RoD and the window (and, for
 *    ReadOIIO, of the window): transparent black out of the RoD (#232)
 *  - the files: ReadOIIO, float TIFF (grey, RGB, RGBA), scanline and tiled EXR with a data window smaller than the
 *    display window, and an EXR whose data window has a negative origin and extends past the display window; ReadPFM,
 *    an RGB and a grey PFM; ReadPNG (if built with libpng), 8-bit RGB and RGBA, 16-bit grey and RGBA, interlaced 8-bit
 *    RGB and 16-bit RGBA PNG. Each one with two filePremult values: decode() writes in the output image at scale 1
 *    (Opaque RGB, PreMultiplied RGBA), or in GenericReader's buffer, which GenericReader then premultiplies
 *    (UnPreMultiplied). At scale 1, the whole image is the file's pixels, and alpha is checked (1 for Opaque files,
 *    also in the black border that ReadOIIO adds around a data window).
 *  - files without alpha (RGB and grey PFM and TIFF, RGB PNG), read as RGBA with each filePremult, without and with
 *    an OCIO conversion (x2 to x1 of swap.ocio, next to $OCIO): the file's colours (times 2) and alpha 1 (#234)
 *  - ReadPNG files with an offset (oFFs) near the limits of an int, at scales 1/2 and 1/4: the render of the file
 *    without an offset, translated (#371)
 *  - ReadOIIO, tiled data windows that end near INT_MAX, in a synthetic OpenImageIO format: the windows are the ones
 *    of the same pixels at x = y = 128 if the last tile ends at INT_MAX, and the renders fail with an error message
 *    if it ends past it (#466: the rounding to the tiles overflowed an int, and so does OpenImageIO's read_tiles()
 *    before 3.1.19 and 3.2.2)
 *  - render scales that are not a power of two (0.75, 1/3), not the same in x and y (0.5 x 0.75, 1 x 0.5), or above 1
 *    (2): the file's image, scaled (the mean of the pixels that each pixel covers, premultiplied after the mean for an
 *    UnPreMultiplied file), and the windows as above (#194); and the PNG offsets above at scale 0.75
 *  - a file rewritten with a smaller or larger image, rendered with the RoD that the host got before (decode() then
 *    gets a render window that is not the file's): only the window is written, and for an Opaque file, alpha is 1 in
 *    the new image and the rest of the window is transparent black (#232)
 *  - concurrent renders while another thread replaces the file by smaller and larger ones (#163): only the
 *    windows are written, alpha is 1 in the image and 0 in the black (Opaque files); ReadOIIO also while the TIFF
 *    file is overwritten in place (#242): the renders may fail, but must not crash or write out of their windows
 *
 * Usage: reader_window_test <scratch dir>   (needs $OCIO, see tests/CMakeLists.txt, and swap.ocio in its directory)
 *        $READER_WINDOW_ONLY: run only the tests whose title contains it (e.g. "ReadPFM", "rewrite", "scale 0.5")
 *        $READER_WINDOW_MULTIPLANE=1: only the 2-part EXR test, in a Natron-like host (dynamic choices, plane suite)
 *        $READER_WINDOW_DUMP: a directory where the render of the whole RoD of each test is written (raw RGBA float)
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <OpenImageIO/imageio.h>
#ifdef HAVE_PNG
#include <png.h>
#endif

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

// $READER_WINDOW_ONLY: run only the tests whose title contains it (e.g. "ReadPFM", "rewrite", "scale 0.5")
bool
selected(const std::string& title)
{
    const char* only = std::getenv("READER_WINDOW_ONLY");

    return !only || title.find(only) != std::string::npos;
}

std::string
strprintf(const char* fmt, ...)
{
    char buf[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);

    return buf;
}

// ------------------------------------------------------------------ an output image between guard pages

// A buffer of n bytes between two pages that can't be accessed, flush against the second one (atEnd) or the
// first one: a write just out of the buffer, after or before it, crashes.
class GuardedBuffer {
public:
    GuardedBuffer() = default;
    GuardedBuffer(const GuardedBuffer&) = delete;
    GuardedBuffer& operator=(const GuardedBuffer&) = delete;
    ~GuardedBuffer() { release(); }

    char* alloc(size_t n, bool atEnd)
    {
        release();
        const size_t page = pageSize();
        const size_t inside = ((n + page - 1) / page) * page;
        _size = inside + 2 * page;
#ifdef _WIN32
        _base = (char*)VirtualAlloc(nullptr, _size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD old;
        if (_base) {
            VirtualProtect(_base, page, PAGE_NOACCESS, &old);
            VirtualProtect(_base + page + inside, page, PAGE_NOACCESS, &old);
        }
#else
        void* p = mmap(nullptr, _size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        _base = (p == MAP_FAILED) ? nullptr : (char*)p;
        if (_base) {
            mprotect(_base, page, PROT_NONE);
            mprotect(_base + page + inside, page, PROT_NONE);
        }
#endif
        if (!_base) {
            std::printf("FAIL: cannot allocate %zu bytes\n", _size);
            std::exit(1);
        }

        return atEnd ? _base + page + inside - n : _base + page;
    }

private:
    static size_t pageSize()
    {
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);

        return si.dwPageSize;
#else
        return (size_t)sysconf(_SC_PAGESIZE);
#endif
    }

    void release()
    {
        if (_base) {
#ifdef _WIN32
            VirtualFree(_base, 0, MEM_RELEASE);
#else
            munmap(_base, _size);
#endif
            _base = nullptr;
        }
    }

    char* _base = nullptr;
    size_t _size = 0;
};

// The value of the pixels that the plug-in did not write (see fillUnwritten()), compared on its bits (never as a float)
const uint32_t kUnwrittenBits = 0xc49a5a5au; // -1234.8...

bool
isUnwrittenBits(const float* p)
{
    return hasBits(p, kUnwrittenBits);
}

bool
sameBits(const float* a,
         const float* b)
{
    return !std::memcmp(a, b, sizeof(float));
}

// ------------------------------------------------------------------ a reader instance

// An instance of a reader in the Reader context, with its own clips and params (separate instances can render
// concurrently), and an RGBA float output image.
struct Reader {
    OfxPlugin* p = nullptr;
    Effect ctx; // context descriptor: clips and params
    Effect inst;
    bool created = false;
    Clip* out = nullptr;
    OfxRectI rod = { 0, 0, 0, 0 }; // in pixels at the scale of prepare()
    OfxRectI img = { 0, 0, 0, 0 }; // the bounds of the output image
    int rowBytes = 0;
    float* data = nullptr;
    GuardedBuffer buffer;

    // filePremult: 0 Opaque, 1 PreMultiplied, 2 UnPreMultiplied (the output is premultiplied)
    // outputLayer: if not empty, ReadOIIO's output layer (its kParamChannelOutputLayerChoice string param, in a host
    // that supports dynamic choices and multiple planes)
    // ocioConfig, ocioInput, ocioOutput: if not empty, the OCIO config file (instead of $OCIO), and the input and
    // output colorspaces (set after the reader guessed them from the file)
    std::string ocioConfig, ocioInput, ocioOutput;

    bool create(OfxPlugin* plugin, const std::string& filename, int filePremult, const std::string& outputLayer = std::string())
    {
        p = plugin;
        PropSet inArgs;
        pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextReader);
        if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, EH(ctx), PH(&inArgs), nullptr))) {
            std::printf("  describeInContext failed\n");

            return false;
        }
        setOverrideString(&ctx, kOfxImageEffectFileParamName, filename);
        if (!ocioConfig.empty()) {
            setOverrideString(&ctx, "ocioConfigFile", ocioConfig); // GenericOCIO loads it when the instance is created
        }
        if (!outputLayer.empty()) {
            setOverrideString(&ctx, "outputLayerChoice", outputLayer);
        }
        inst.delegate = &ctx;
        setInstanceProps(inst, 100, 100, 1, kOfxImageEffectContextReader);
        if (!ok(p->mainEntry(kOfxActionCreateInstance, EH(inst), nullptr, nullptr))) {
            std::printf("  createInstance failed\n");

            return false;
        }
        created = true;
        out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
        if (!out) {
            std::printf("  no output clip\n");

            return false;
        }
        // the user set the filename: guessParamsFromFilename, frame range...
        if (!ok(instanceChanged(p, inst, kOfxImageEffectFileParamName))) {
            std::printf("  instanceChanged(filename) failed\n");

            return false;
        }
        // UnPreMultiplied: GenericReader premultiplies the image, from its buffer; Opaque or PreMultiplied (the
        // output's premultiplication): decode() writes in the output image, at scale 1
        setOverrideInt(&ctx, "filePremult", filePremult);
        setOverrideInt(&ctx, "outputPremult", 1);
        if (!ocioInput.empty()) {
            setOverrideString(&ctx, "ocioInputSpace", ocioInput);
            setOverrideString(&ctx, "ocioOutputSpace", ocioOutput);
        }
        setOverrideInt(&ctx, "outputComponents", 0); // RGBA
        out->connected = true;
        setupClipImage(out, 1, 1, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        PropSet o;
        if (!ok(p->mainEntry(kOfxImageEffectActionGetClipPreferences, EH(inst), nullptr, PH(&o)))) {
            std::printf("  getClipPreferences failed\n");

            return false;
        }

        return true;
    }

    // getRegionOfDefinition at the given render scale (rod, in pixels at that scale)
    OfxStatus getRoD(double scale) { return getRoD(OfxPointD { scale, scale }); }
    OfxStatus getRoD(const OfxPointD& scale)
    {
        PropSet a;
        pSetDouble(PH(&a), kOfxPropTime, 0, 1.);
        double rs[2] = { scale.x, scale.y };
        pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, 0);
        PropSet o;
        OfxStatus st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, EH(inst), PH(&a), PH(&o));
        if (!ok(st)) {
            return st;
        }
        double r[4] = { 0., 0., 0., 0. };
        pGetDoubleN(PH(&o), kOfxImageEffectPropRegionOfDefinition, 4, r);
        rod = { (int)std::floor(r[0] * scale.x), (int)std::floor(r[1] * scale.y), (int)std::ceil(r[2] * scale.x), (int)std::ceil(r[3] * scale.y) };

        return kOfxStatOK;
    }

    // an output image with the given bounds (in pixels at the render scale), between guard pages, every value
    // unwritten; the RoD of the image is rod
    void setImage(const OfxRectI& bounds, double scale, bool atEnd) { setImage(bounds, OfxPointD { scale, scale }, atEnd); }
    void setImage(const OfxRectI& bounds, const OfxPointD& scale, bool atEnd)
    {
        img = bounds;
        double rs[2] = { scale.x, scale.y };
        const int w = img.x2 - img.x1;
        const int h = img.y2 - img.y1;
        rowBytes = w * 4 * (int)sizeof(float);
        const size_t n = (size_t)rowBytes * h;
        data = (float*)buffer.alloc(n, atEnd);
        fillUnwritten(data, n / sizeof(float), kUnwrittenBits);
        OfxPropertySetHandle ip = PH(&out->image);
        pSetPointer(ip, kOfxImagePropData, 0, data);
        int b[4] = { img.x1, img.y1, img.x2, img.y2 };
        pSetIntN(ip, kOfxImagePropBounds, 4, b);
        int r[4] = { rod.x1, rod.y1, rod.x2, rod.y2 };
        pSetIntN(ip, kOfxImagePropRegionOfDefinition, 4, r);
        pSetInt(ip, kOfxImagePropRowBytes, 0, rowBytes);
        pSetDoubleN(ip, kOfxImageEffectPropRenderScale, 2, rs);
        double rd[4] = { rod.x1 / scale.x, rod.y1 / scale.y, rod.x2 / scale.x, rod.y2 / scale.y };
        pSetDoubleN(PH(&out->props), kOfxImageEffectPropRegionOfDefinition, 4, rd);
    }

    // render a window (in pixels at the render scale)
    OfxStatus render(const OfxRectI& window, double scale, bool sequential) { return render(window, OfxPointD { scale, scale }, sequential); }
    OfxStatus render(const OfxRectI& window, const OfxPointD& scale, bool sequential)
    {
        PropSet ra = makeRenderInArgs(1., 0, 0);
        int win[4] = { window.x1, window.y1, window.x2, window.y2 };
        pSetIntN(PH(&ra), kOfxImageEffectPropRenderWindow, 4, win);
        double rs[2] = { scale.x, scale.y };
        pSetDoubleN(PH(&ra), kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(PH(&ra), kOfxImageEffectPropSequentialRenderStatus, 0, sequential ? 1 : 0);

        return p->mainEntry(kOfxImageEffectActionRender, EH(inst), PH(&ra), nullptr);
    }

    const float* pixel(int x, int y) const
    {
        return reinterpret_cast<const float*>(reinterpret_cast<const char*>(data) + (size_t)(y - img.y1) * rowBytes) + (size_t)(x - img.x1) * 4;
    }

    ~Reader()
    {
        if (created) {
            p->mainEntry(kOfxActionDestroyInstance, EH(inst), nullptr, nullptr);
        }
    }
};

// A whole image (RGBA float), copied from a render of the RoD
struct Image {
    OfxRectI bounds = { 0, 0, 0, 0 };
    std::vector<float> pixels;

    const float* pixel(int x, int y) const { return &pixels[((size_t)(y - bounds.y1) * (bounds.x2 - bounds.x1) + (x - bounds.x1)) * 4]; }
};

// ------------------------------------------------------------------ the files

// The files' values, at pixel (x, y) of the file (y down from the top line), channel c (3: alpha)
float
floatValue(int x,
           int y,
           int c)
{
    if (c == 3) {
        return 0.25f + (((x + 2 * y) % 4 + 4) % 4) * 0.25f; // 0.25 to 1 (also for negative x or y)
    }

    return (((x + 3 * y + 7 * c) % 97 + 97) % 97) / 16.f + c * 0.0625f;
}

unsigned char
byteValue(int x,
          int y,
          int c)
{
    if (c == 3) {
        return (unsigned char)(64 + ((x + 2 * y) % 3) * 64);
    }

    return (unsigned char)((x * 5 + y * 11 + c * 67) & 0xff);
}

unsigned short
shortValue(int x,
           int y,
           int c)
{
    return (unsigned short)((byteValue(x, y, c) << 8) | ((x * 3 + y * 5 + c * 11) & 0xff));
}

enum FileKind {
    eTIFF, // float TIFF
    eEXR, // float EXR, scanlines, data window in the display window
    eEXRTiled, // the same, tiled
    ePFM,
    ePNG, // 8-bit PNG
};

struct File {
    const char* plugin;
    FileKind kind;
    int width, height; // the display window (the image of PNG, TIFF and PFM)
    int nchannels;
    int dataX, dataY, dataW, dataH; // EXR: the data window, in the display window (y down)
    int pngBits = 8; // PNG: 8 or 16 bits
    bool pngInterlaced = false; // PNG: Adam7 interlacing
    int pngOffsetX = 0, pngOffsetY = 0; // PNG: the image offset (oFFs chunk, in pixels), the origin of the bounds

    bool isFloat() const { return kind != ePNG; }
    bool isEXR() const { return (kind == eEXR) || (kind == eEXRTiled); }

    // the frame bounds of the file in OFX pixels: the image, or the data window of an EXR (y flipped), with the
    // 1-pixel black border that ReadOIIO adds around a data window that is not the display window (Edge Pixels: Auto)
    OfxRectI frameBounds() const
    {
        if (!isEXR()) {
            return { 0, 0, width, height };
        }
        OfxRectI b = { dataX, height - (dataY + dataH), dataX + dataW, height - dataY };
        if ((dataX != 0) || (dataY != 0) || (dataW != width) || (dataH != height)) {
            b.x1 -= 1;
            b.y1 -= 1;
            b.x2 += 1;
            b.y2 += 1;
        }

        return b;
    }

    // the value of the file's channel c at file pixel (fx, fy) (y down), as the reader reads it
    float fileValue(int fx, int fy, int c) const
    {
        if (isFloat()) {
            return floatValue(fx, fy, c);
        }
        if (pngBits == 16) {
            return shortValue(fx, fy, c) * (1.f / 65535);
        }

        return byteValue(fx, fy, c) * (1.f / 255);
    }
    bool hasData(int fx, int fy) const
    {
        if ((kind != eEXR) && (kind != eEXRTiled)) {
            return fx >= 0 && fx < width && fy >= 0 && fy < height;
        }

        return fx >= dataX && fx < dataX + dataW && fy >= dataY && fy < dataY + dataH;
    }

    // the value of OFX pixel (x, y) at render scale 1 of the RGBA output, channel c, read with the given filePremult
    // (premultiplied by GenericReader if 2)
    float expected(int x, int y, int c, int filePremult) const
    {
        // PFM is stored bottom-up, the others top-down
        const int fx = x;
        const int fy = (kind == ePFM) ? y : height - 1 - y;
        if (!hasData(fx, fy)) {
            // black, out of the data window (e.g. the border that ReadOIIO adds around it): transparent, unless the
            // file is Opaque (it is in the RoD: GenericReader sets alpha to 1)
            return ((c == 3) && (nchannels < 4) && (filePremult == 0)) ? 1.f : 0.f;
        }
        // the alpha of the output: 1 for a file without alpha, whatever filePremult is (#234)
        const float alpha = (nchannels == 4) ? fileValue(fx, fy, 3) : 1.f;
        if (c == 3) {
            return alpha;
        }
        // a grey file (1 channel): R = G = B
        const int fc = (nchannels == 1) ? 0 : c;
        const float value = fileValue(fx, fy, fc);

        return (filePremult == 2) ? value * alpha : value;
    }
};

bool
writePFM(const std::string& path,
         const File& f)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    // a negative scale: little-endian
    const uint16_t one = 1;
    const bool littleEndian = *(const unsigned char*)&one == 1;
    // "PF": RGB, "Pf": grey
    const int C = (f.nchannels == 1) ? 1 : 3;
    std::fprintf(file, "P%c\n%d %d\n%s\n", (C == 1) ? 'f' : 'F', f.width, f.height, littleEndian ? "-1.0" : "1.0");
    std::vector<float> line((size_t)f.width * C);
    for (int y = 0; y < f.height; ++y) { // bottom-up
        for (int x = 0; x < f.width; ++x) {
            for (int c = 0; c < C; ++c) {
                line[(size_t)x * C + c] = floatValue(x, y, c);
            }
        }
        std::fwrite(line.data(), sizeof(float), line.size(), file);
    }

    return std::fclose(file) == 0;
}

bool
writeOIIO(const std::string& path,
          const File& f)
{
    const bool isExr = (f.kind == eEXR) || (f.kind == eEXRTiled);
    const int w = isExr ? f.dataW : f.width;
    const int h = isExr ? f.dataH : f.height;
    OIIO::ImageSpec spec(w, h, f.nchannels, f.isFloat() ? OIIO::TypeDesc::FLOAT : OIIO::TypeDesc::UINT8);
    if (isExr) {
        spec.x = f.dataX;
        spec.y = f.dataY;
        spec.full_x = 0;
        spec.full_y = 0;
        spec.full_width = f.width;
        spec.full_height = f.height;
        if (f.kind == eEXRTiled) {
            spec.tile_width = 16;
            spec.tile_height = 16;
        }
    }
    if (f.nchannels == 4) {
        spec.alpha_channel = 3;
        if (!isExr) {
            // the values are not premultiplied: TIFF and PNG store them as is (EXR files are always premultiplied:
            // the test reads them as unpremultiplied anyway)
            spec.attribute("oiio:UnassociatedAlpha", 1);
        }
    }
    std::vector<float> fpix;
    std::vector<unsigned char> bpix;
    const int x0 = isExr ? f.dataX : 0;
    const int y0 = isExr ? f.dataY : 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < f.nchannels; ++c) {
                if (f.isFloat()) {
                    fpix.push_back(floatValue(x + x0, y + y0, c));
                } else {
                    bpix.push_back(byteValue(x, y, c));
                }
            }
        }
    }
    auto out = OIIO::ImageOutput::create(path);
    if (!out || !out->open(path, spec)) {
        std::printf("  cannot write %s: %s\n", path.c_str(), out ? out->geterror().c_str() : OIIO::geterror().c_str());

        return false;
    }
    const bool written = f.isFloat() ? out->write_image(OIIO::TypeDesc::FLOAT, fpix.data()) : out->write_image(OIIO::TypeDesc::UINT8, bpix.data());

    return out->close() && written;
}

#ifdef HAVE_PNG
// A PNG file (8 or 16 bits, interlaced or not), written with libpng (OpenImageIO does not write interlaced PNG files)
bool
writePNG(const std::string& path,
         const File& f)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, info ? &info : nullptr);
        std::fclose(file);

        return false;
    }
    png_init_io(png, file);
    static const int colorTypes[] = { PNG_COLOR_TYPE_GRAY, PNG_COLOR_TYPE_GRAY_ALPHA, PNG_COLOR_TYPE_RGB, PNG_COLOR_TYPE_RGB_ALPHA };
    png_set_IHDR(png, info, f.width, f.height, f.pngBits, colorTypes[f.nchannels - 1],
                 f.pngInterlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    if ((f.pngOffsetX != 0) || (f.pngOffsetY != 0)) {
        png_set_oFFs(png, info, f.pngOffsetX, f.pngOffsetY, PNG_OFFSET_PIXEL);
    }
    png_write_info(png, info);
    const size_t bytes = (f.pngBits == 16) ? 2 : 1;
    const size_t rowBytes = (size_t)f.width * f.nchannels * bytes;
    std::vector<unsigned char> pixels(rowBytes * f.height);
    std::vector<png_bytep> rows(f.height);
    for (int y = 0; y < f.height; ++y) { // top-down
        rows[y] = &pixels[rowBytes * y];
        for (int x = 0; x < f.width; ++x) {
            for (int c = 0; c < f.nchannels; ++c) {
                unsigned char* px = rows[y] + ((size_t)x * f.nchannels + c) * bytes;
                if (f.pngBits == 16) {
                    const unsigned short v = shortValue(x, y, c);
                    px[0] = (unsigned char)(v >> 8); // big-endian
                    px[1] = (unsigned char)(v & 0xff);
                } else {
                    px[0] = byteValue(x, y, c);
                }
            }
        }
    }
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);

    return std::fclose(file) == 0;
}
#endif

bool
writeFile(const std::string& path,
          const File& f)
{
#ifdef HAVE_PNG
    if (f.kind == ePNG) {
        return writePNG(path, f);
    }
#endif

    return (f.kind == ePFM) ? writePFM(path, f) : writeOIIO(path, f);
}

const char*
extension(FileKind k)
{
    switch (k) {
    case eTIFF:
        return ".tif";
    case eEXR:
    case eEXRTiled:
        return ".exr";
    case ePFM:
        return ".pfm";
    case ePNG:
        return ".png";
    }

    return "";
}

// ------------------------------------------------------------------ tests

// render windows: at scale 1, in pixels of the image (w x h); at scale 1/2 they are halved
std::vector<OfxRectI>
windows(const OfxRectI& r)
{
    const int w = r.x2 - r.x1;
    const int h = r.y2 - r.y1;
    std::vector<OfxRectI> v = {
        { r.x1 + w / 2, r.y1, r.x1 + w / 2 + 3, r.y2 }, // a few columns
        { r.x1, r.y1, r.x1 + 1, r.y2 }, // the left column
        { r.x2 - 1, r.y1, r.x2, r.y2 }, // the right column
        { r.x1, r.y1 + h / 2, r.x2, r.y1 + h / 2 + 2 }, // a few rows
        { r.x1, r.y1, r.x2, r.y1 + 1 }, // the bottom row
        { r.x1, r.y2 - 1, r.x2, r.y2 }, // the top row
        { r.x1 + w / 4, r.y1 + h / 4, r.x1 + w / 2, r.y1 + h / 2 }, // a box
        { r.x1 + w / 3, r.y1 + h / 3, r.x1 + w / 3 + 1, r.y1 + h / 3 + 1 }, // a pixel
        { r.x1, r.y1, r.x1 + 2, r.y1 + 2 }, // the bottom left corner
        { r.x2 - 2, r.y2 - 2, r.x2, r.y2 }, // the top right corner
    };

    return v;
}

// render windows partly or entirely out of the RoD r (in an image that covers them): the OFX spec says that "If an
// effect is asked to render outside of its region of definition, it should fill those pixels in with black
// transparent pixels"
std::vector<OfxRectI>
outsideWindows(const OfxRectI& r)
{
    const int w = r.x2 - r.x1;
    const int h = r.y2 - r.y1;
    std::vector<OfxRectI> v = {
        { r.x1 - 3, r.y1 + h / 3, r.x1 + 2, r.y1 + h / 2 }, // across the left edge
        { r.x2 - 2, r.y2 - 2, r.x2 + 5, r.y2 + 4 }, // across the top right corner
        { r.x1 + w / 2, r.y1 - 4, r.x1 + w / 2 + 3, r.y2 + 3 }, // a few columns, longer than the RoD
        { r.x1 - 4, r.y1 - 4, r.x2 + 4, r.y2 + 4 }, // the RoD and a border
        { r.x2 + 2, r.y1, r.x2 + 6, r.y1 + 3 }, // right of the RoD
        { r.x1, r.y1 - 5, r.x1 + 4, r.y1 - 1 }, // below the RoD
    };

    return v;
}

OfxRectI
boundingBox(const OfxRectI& a,
            const OfxRectI& b)
{
    return { (std::min)(a.x1, b.x1), (std::min)(a.y1, b.y1), (std::max)(a.x2, b.x2), (std::max)(a.y2, b.y2) };
}

// The pixels of a render of window into r's image, compared to ref (the render of the whole RoD): the same bits in
// the window (transparent black out of the RoD), unwritten elsewhere. Returns the number of wrong values.
int
compareWindow(const Reader& r,
              const Image& ref,
              const OfxRectI& window)
{
    int bad = 0;
    const float zero = 0.f;
    for (int y = r.img.y1; y < r.img.y2; ++y) {
        for (int x = r.img.x1; x < r.img.x2; ++x) {
            const bool inWindow = x >= window.x1 && x < window.x2 && y >= window.y1 && y < window.y2;
            const bool inRoD = x >= ref.bounds.x1 && x < ref.bounds.x2 && y >= ref.bounds.y1 && y < ref.bounds.y2;
            const float* px = r.pixel(x, y);
            for (int c = 0; c < 4; ++c) {
                const bool good = inWindow ? sameBits(&px[c], inRoD ? &ref.pixel(x, y)[c] : &zero) : isUnwrittenBits(&px[c]);
                if (!good) {
                    if (bad < 3) {
                        std::printf("  pixel (%d,%d) channel %d (%s the window): %.9g\n", x, y, c, inWindow ? "in" : "out of", px[c]);
                    }
                    ++bad;
                }
            }
        }
    }

    return bad;
}

// "0.5" for a render scale of 0.5 in x and y, "1x0.5" for 1 in x and 0.5 in y
std::string
scaleName(const OfxPointD& scale)
{
    return (scale.x == scale.y) ? strprintf("%g", scale.x) : strprintf("%gx%g", scale.x, scale.y);
}

// $READER_WINDOW_DUMP: a directory where the whole renders are written (raw RGBA float, one file per test), to
// compare the images of two builds bit by bit (e.g. "cmp -s").
void
dumpImage(const std::string& title,
          const Image& image)
{
    const char* dir = std::getenv("READER_WINDOW_DUMP");
    if (!dir || !*dir) {
        return;
    }
    std::string name;
    for (char ch : title) {
        name.push_back((std::isalnum((unsigned char)ch) || ch == '.') ? ch : '_');
    }
    std::FILE* file = std::fopen((std::string(dir) + "/" + name + ".raw").c_str(), "wb");
    if (file) {
        std::fwrite(image.pixels.data(), sizeof(float), image.pixels.size(), file);
        std::fclose(file);
    }
}

// Returns the render of the whole RoD (empty if the instance or the RoD failed).
Image
testFileRenders(OfxPlugin* p,
                const std::string& path,
                const File& f,
                bool isOIIO,
                int filePremult,
                const OfxPointD& scale)
{
    Reader r;
    if (!r.create(p, path, filePremult)) {
        check(false, "create");

        return Image();
    }
    OfxStatus st = r.getRoD(scale);
    if (!ok(st)) {
        check(false, "getRegionOfDefinition = %d", st);

        return Image();
    }
    // the whole image
    r.setImage(r.rod, scale, true);
    gPersistentError.clear();
    st = r.render(r.rod, scale, false);
    check(ok(st) && gPersistentError.empty(), "render %d,%d,%d,%d = %d%s%s", r.rod.x1, r.rod.y1, r.rod.x2, r.rod.y2, st,
          gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
    Image ref;
    ref.bounds = r.rod;
    ref.pixels.assign(r.data, r.data + (size_t)(r.rod.x2 - r.rod.x1) * (r.rod.y2 - r.rod.y1) * 4);
    dumpImage(strprintf("%s %s filePremult %d scale %s", f.plugin + 16, path.substr(path.find_last_of("/\\") + 1).c_str(), filePremult, scaleName(scale).c_str()), ref);
    // the readers don't depend on kOfxImageEffectPropSequentialRenderStatus ("whether the effect is currently being
    // rendered in frame order on a single effect instance", not playback): the same image with 1
    {
        r.setImage(r.rod, scale, false);
        st = r.render(r.rod, scale, true);
        int differ = 0;
        for (size_t k = 0; k < ref.pixels.size(); ++k) {
            differ += sameBits(&ref.pixels[k], &r.data[k]) ? 0 : 1;
        }
        check(ok(st) && differ == 0, "the same render with a sequential render status of 1 (%d values differ)", differ);
    }
    if ((scale.x == 1.) && (scale.y == 1.)) {
        int bad = 0;
        for (int y = r.rod.y1; y < r.rod.y2; ++y) {
            for (int x = r.rod.x1; x < r.rod.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const float v = f.expected(x, y, c, filePremult);
                    const float* px = ref.pixel(x, y);
                    if (!(std::fabs(px[c] - v) <= 1e-6f * (1.f + std::fabs(v)))) {
                        if (bad < 3) {
                            std::printf("  pixel (%d,%d) channel %d: %.9g, expected %.9g\n", x, y, c, px[c], v);
                        }
                        ++bad;
                    }
                }
            }
        }
        check(bad == 0, "the file's pixels, and alpha (%d wrong)", bad);
    } else {
        int unwritten = 0;
        for (float v : ref.pixels) {
            unwritten += isUnwrittenBits(&v) ? 1 : 0;
        }
        check(unwritten == 0, "every value written (%d not)", unwritten);
    }
    // the windows, in an image of the RoD (flush against the guard page after or before it) and, for a
    // reader that supports tiles, in an image of the window
    int failed = 0, bad = 0, n = 0;
    for (const OfxRectI& w : windows(r.rod)) {
        for (int k = 0; k < (isOIIO ? 4 : 2); ++k) {
            r.setImage((k < 2) ? r.rod : w, scale, (k % 2) == 0);
            gPersistentError.clear();
            st = r.render(w, scale, false);
            failed += (ok(st) && gPersistentError.empty()) ? 0 : 1;
            const int b = compareWindow(r, ref, w);
            if (b) {
                std::printf("  (window %d,%d,%d,%d, %s image%s: %d wrong)\n", w.x1, w.y1, w.x2, w.y2, (k < 2) ? "RoD" : "window",
                            (k % 2) == 0 ? " at the end guard" : " at the start guard", b);
            }
            bad += b;
            ++n;
        }
    }
    check(failed == 0 && bad == 0, "%d renders of narrow windows: %d failed, %d wrong values", n, failed, bad);
    // windows partly or entirely out of the RoD, in an image that covers the RoD and the window (a reader that does
    // not support tiles gets an image that has the whole RoD, kOfxImageEffectPropSupportsTiles: "If a clip or plugin
    // does not support tiled images, then the host should supply full RoD images to the effect whenever it fetches
    // one") and, for ReadOIIO, which supports tiles, in an image of the window: transparent black out of the RoD
    failed = bad = n = 0;
    for (const OfxRectI& w : outsideWindows(r.rod)) {
        for (int k = 0; k < (isOIIO ? 4 : 2); ++k) {
            r.setImage((k < 2) ? boundingBox(r.rod, w) : w, scale, (k % 2) == 0);
            gPersistentError.clear();
            st = r.render(w, scale, false);
            failed += (ok(st) && gPersistentError.empty()) ? 0 : 1;
            const int b = compareWindow(r, ref, w);
            if (b) {
                std::printf("  (window %d,%d,%d,%d, %s image%s: %d wrong)\n", w.x1, w.y1, w.x2, w.y2, (k < 2) ? "RoD+window" : "window",
                            (k % 2) == 0 ? " at the end guard" : " at the start guard", b);
            }
            bad += b;
            ++n;
        }
    }
    check(failed == 0 && bad == 0, "%d renders of windows out of the RoD: %d failed, %d wrong values", n, failed, bad);

    return ref;
}

void
testFile(OfxPlugin* p,
         const std::string& dir,
         const char* name,
         const File& f)
{
    const std::string path = dir + "/" + name + extension(f.kind);
    if (!writeFile(path, f)) {
        check(false, "%s: write the file", name);

        return;
    }
    const bool isOIIO = !std::strcmp(f.plugin, "fr.inria.openfx.ReadOIIO");
    // Both paths of GenericReader::render() at scale 1: decode() into the output image (Opaque RGB files,
    // PreMultiplied RGBA files), and into GenericReader's buffer, which it then premultiplies (UnPreMultiplied
    // files). At scale 1/2, decode() always writes in the buffer.
    const int filePremults[2] = { (f.nchannels == 4) ? 2 : 0, (f.nchannels == 4) ? 1 : 2 };
    for (int filePremult : filePremults) {
        for (double scale : { 1., 0.5 }) {
            const std::string title = strprintf("%s %s, filePremult %d, scale %g", f.plugin + 16, name, filePremult, scale);
            if (!selected(title)) {
                continue;
            }
            std::printf("==== %s ====\n", title.c_str());
            testFileRenders(p, path, f, isOIIO, filePremult, OfxPointD { scale, scale });
        }
    }
}

// Render scales that are not a power of two, or not the same in x and y, or above 1 (#194). kOfxImageEffectPropRenderScale
// is "The proxy render scale currently being applied" (ofxImageEffect.h), and nothing in OFX restricts it to powers of
// two. GenericReader decoded the image at the nearest power of two and wrote it as is: at 0.75 or 2, a crop of the
// image; at 1/3, the image at 1/4; at 0.5 x 0.75 or 1 x 0.5, the image at 1/2 in x and y. Each pixel at render scale
// s must be the mean of the file's pixels that it covers (areaMean(), on the frame bounds), computed from the file's
// values (File::expected()):
//  - Opaque (RGB) or PreMultiplied (RGBA) files: the mean of the output values at scale 1 (GenericReader resamples
//    the decoded image into the output image);
//  - UnPreMultiplied RGBA files: GenericReader resamples the unpremultiplied image, then premultiplies it: the mean
//    colour times the mean alpha.
// Scale 1/2 is the power-of-two path (GenericReader halves the image), which already gives that mean: it checks the
// reference. Scale 2 covers part of one pixel: the file's pixel. The tolerance, 1e-6 relative (plus 1e-6 absolute),
// is a few float ulps of the values (up to 7): the reader computes in float (the conversion of 8-bit values, the
// halving, the premultiplication), the reference in double.
void
testScales(OfxPlugin* p,
           const std::string& dir,
           const char* name,
           const File& f)
{
    const std::string path = dir + "/scales-" + name + extension(f.kind);
    if (!writeFile(path, f)) {
        check(false, "%s: write the file", name);

        return;
    }
    const bool isOIIO = !std::strcmp(f.plugin, "fr.inria.openfx.ReadOIIO");
    const OfxRectI bounds = f.frameBounds();
    const OfxPointD scales[] = { { 0.5, 0.5 }, { 0.75, 0.75 }, { 1. / 3, 1. / 3 }, { 0.5, 0.75 }, { 1., 0.5 }, { 2., 2. } };
    std::vector<int> filePremults = { (f.nchannels == 4) ? 1 : 0 };
    if (f.nchannels == 4) {
        filePremults.push_back(2);
    }
    for (int filePremult : filePremults) {
        for (const OfxPointD& scale : scales) {
            const std::string title = strprintf("%s %s, filePremult %d, scale %s", f.plugin + 16, (std::string("scales-") + name).c_str(), filePremult,
                                                scaleName(scale).c_str());
            if (!selected(title)) {
                continue;
            }
            std::printf("==== %s ====\n", title.c_str());
            // the renders at this scale: the whole RoD, narrow windows and windows out of the RoD
            const Image scaled = testFileRenders(p, path, f, isOIIO, filePremult, scale);
            if (scaled.pixels.empty()) {
                continue;
            }
            // UnPreMultiplied: the mean of the file's (unpremultiplied) values, premultiplied
            const int meanPremult = (filePremult == 2) ? 1 : filePremult;
            int bad = 0;
            for (int y = scaled.bounds.y1; y < scaled.bounds.y2; ++y) {
                for (int x = scaled.bounds.x1; x < scaled.bounds.x2; ++x) {
                    double mean[4];
                    for (int c = 0; c < 4; ++c) {
                        mean[c] = areaMean(bounds, scale, x, y, [&](int i, int j) { return (double)f.expected(i, j, c, meanPremult); });
                    }
                    for (int c = 0; c < 4; ++c) {
                        const double v = ((filePremult == 2) && (c < 3)) ? mean[c] * mean[3] : mean[c];
                        const float got = scaled.pixel(x, y)[c];
                        if (!(std::fabs(got - v) <= 1e-6 * (1. + std::fabs(v)))) {
                            if (bad < 3) {
                                std::printf("  pixel (%d,%d) channel %d: %.9g, expected %.9g\n", x, y, c, got, v);
                            }
                            ++bad;
                        }
                    }
                }
            }
            check(bad == 0, "the render at scale %s is the file's image, scaled (%d wrong values)", scaleName(scale).c_str(), bad);
        }
    }
}

#ifdef HAVE_PNG
// A PNG file with an offset (oFFs) near the limits of an int, at render scales 1/2 and 1/4 (#371): GenericReader's
// proxy path computed the frame window at the render scale, the full-resolution render window and the offset of the
// buffer's origin in int, which overflowed (undefined behaviour: this test is built with
// -fsanitize=signed-integer-overflow). The offsets are multiples of 4, so the render is the one of the file without
// an offset, translated, bit for bit.
void
testLargeOffsets(OfxPlugin* p,
                 const std::string& dir,
                 const File& f)
{
    const std::string path0 = dir + "/offset-none" + extension(f.kind);
    if (!writeFile(path0, f)) {
        check(false, "offset-none: write the file");

        return;
    }
    struct Offset {
        const char* name;
        int x, y;
    };
    // offset-min: x1 = -2^31 + 4 (an oFFs offset is at least -INT_MAX) and y1 = 2^24, for which the offset of the
    // buffer's origin (x1 * 4 + row size * y1) is out of the int range; offset-max: x2 and y2 just below INT_MAX
    // (kOfxFlagInfiniteMax), for which x2 + 3 (the smallest enclosing window at scale 1/4) and the window at scale 1/4
    // times 4 are out of it
    const Offset offsets[] = {
        { "offset-min", -2147483644, 16777216 },
        { "offset-max", 2147483584, 2147483584 },
    };
    const int filePremult = (f.nchannels == 4) ? 2 : 0;
    for (const Offset& o : offsets) {
        File fo = f;
        fo.pngOffsetX = o.x;
        fo.pngOffsetY = o.y;
        const std::string path = dir + "/" + o.name + extension(f.kind);
        if (!writeFile(path, fo)) {
            check(false, "%s: write the file", o.name);

            continue;
        }
        // and 0.75 (#194): the offsets times 0.75 are integers, but the footprints of the pixels at scale 1 (x / 0.75)
        // are computed in double near 2^31, where an ulp is 5e-7 pixel: the weights, and the means, may differ by
        // about 1e-6 (the values are 0 to 1). The tolerance, 1e-5 relative, is a 10x margin over that.
        for (double scale : { 0.5, 0.25, 0.75 }) {
            const bool sameBitsExpected = (scale != 0.75);
            const std::string title = strprintf("%s %s, scale %g", f.plugin + 16, o.name, scale);
            if (!selected(title)) {
                continue;
            }
            std::printf("==== %s ====\n", title.c_str());
            Reader r0, r;
            if (!r0.create(p, path0, filePremult) || !r.create(p, path, filePremult)) {
                check(false, "create");

                continue;
            }
            OfxStatus st0 = r0.getRoD(scale);
            OfxStatus st = r.getRoD(scale);
            if (!ok(st0) || !ok(st)) {
                check(false, "getRegionOfDefinition = %d, %d", st0, st);

                continue;
            }
            const int dx = (int)(o.x * scale);
            const int dy = (int)(o.y * scale);
            const bool sameRoD = r.rod.x1 == r0.rod.x1 + dx && r.rod.y1 == r0.rod.y1 + dy && r.rod.x2 == r0.rod.x2 + dx && r.rod.y2 == r0.rod.y2 + dy;
            check(sameRoD, "RoD %d,%d,%d,%d (the RoD without an offset, translated)", r.rod.x1, r.rod.y1, r.rod.x2, r.rod.y2);
            if (!sameRoD) {
                continue; // the pixel loop below would read out of r's image
            }
            r0.setImage(r0.rod, scale, true);
            r.setImage(r.rod, scale, true);
            gPersistentError.clear();
            st0 = r0.render(r0.rod, scale, false);
            st = r.render(r.rod, scale, false);
            check(ok(st0) && ok(st) && gPersistentError.empty(), "render = %d, %d%s%s", st0, st,
                  gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
            int bad = 0;
            for (int y = r0.rod.y1; y < r0.rod.y2; ++y) {
                for (int x = r0.rod.x1; x < r0.rod.x2; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        const float got = r.pixel(x + dx, y + dy)[c];
                        const float v = r0.pixel(x, y)[c];
                        if (sameBitsExpected ? !sameBits(&got, &v) : !(std::fabs(got - v) <= 1e-5f * (1.f + std::fabs(v)))) {
                            if (bad < 3) {
                                std::printf("  pixel (%d,%d) channel %d: %.9g, expected %.9g\n", x, y, c, r.pixel(x + dx, y + dy)[c], r0.pixel(x, y)[c]);
                            }
                            ++bad;
                        }
                    }
                }
            }
            check(bad == 0, "the pixels of the file without an offset (%d wrong)", bad);
        }
    }
}
#endif

// A synthetic tiled image format for testTiledNearIntMax() (#466), registered with OIIO::declare_imageio_format(): a
// file "*.rw466tile" holds two integers, the origin of the data window (x = y, also the display window) and the tile
// size; the image is kTiledW x kTiledH, 3 float channels, with the pixel values floatValue(x - origin, y - origin, c).
// No real format can carry such an origin on every OpenImageIO: OpenEXR rejects data windows beyond INT_MAX / 2, the
// TIFF reader drops origins of 2^30 or more from OpenImageIO 3.1.18, and check_open() rejects origin + size >
// INT_MAX from 3.1.19 and 3.2.2.
const int kTiledW = 37, kTiledH = 9, kTiledNch = 3;

class NearIntMaxInput : public OIIO::ImageInput {
public:
    const char* format_name() const override { return "rw466tile"; }
    using OIIO::ImageInput::open; // (the other overloads, not hidden by the override below)
    bool open(const std::string& name, OIIO::ImageSpec& newspec) override
    {
        std::FILE* file = std::fopen(name.c_str(), "r");
        if (!file) {
            return false;
        }
        const bool read = std::fscanf(file, "%d %d", &_origin, &_tile) == 2;
        std::fclose(file);
        if (!read || (_tile <= 0)) {
            return false;
        }
        m_spec = OIIO::ImageSpec(kTiledW, kTiledH, kTiledNch, OIIO::TypeDesc::FLOAT);
        m_spec.x = m_spec.y = m_spec.full_x = m_spec.full_y = _origin;
        m_spec.tile_width = m_spec.tile_height = _tile;
        m_spec.tile_depth = 1;
        newspec = m_spec;

        return true;
    }
    bool close() override { return true; }
    bool read_native_scanline(int, int, int, int, void*) override { return false; } // a tiled image
    bool read_native_tile(int subimage, int miplevel, int x, int y, int /*z*/, void* data) override
    {
        if ((subimage != 0) || (miplevel != 0)) {
            return false;
        }
        // (in 64 bits: past the data window, x + i and y + j may be past INT_MAX)
        float* out = static_cast<float*>(data);
        for (int j = 0; j < _tile; ++j) {
            for (int i = 0; i < _tile; ++i) {
                const std::int64_t fx = (std::int64_t)x + i - _origin;
                const std::int64_t fy = (std::int64_t)y + j - _origin;
                const bool in = fx >= 0 && fx < kTiledW && fy >= 0 && fy < kTiledH;
                for (int c = 0; c < kTiledNch; ++c) {
                    *out++ = in ? floatValue((int)fx, (int)fy, c) : 0.f;
                }
            }
        }

        return true;
    }

private:
    int _origin = 0, _tile = 1;
};

OIIO::ImageInput*
createNearIntMaxInput()
{
    return new NearIntMaxInput;
}

// Tiled data windows that end near INT_MAX (#466), in x and in y, read through the synthetic format above. The display
// window is the data window, which ReadOIIO shifts to the origin.
//  - origin INT_MAX - 128, tiles of 128: the last tile ends at INT_MAX. The renders of the narrow windows and of the
//    RoD are the ones of the same pixels at x = y = 128, bit for bit.
//  - origin INT_MAX - 128, tiles of 129: the last tile ends past INT_MAX. ReadOIIO rounded a render window that is not
//    on the tile grid to the tiles in int, which overflowed (undefined behaviour: this test is built with
//    -fsanitize=signed-integer-overflow), and OpenImageIO's read_tiles() before 3.1.19 and 3.2.2 steps from tile to
//    tile in int too: every render must fail, with an error message, and write nothing.
void
testTiledNearIntMax(OfxPlugin* p,
                    const std::string& dir)
{
    const std::string title = "ReadOIIO tiled data window near INT_MAX";
    if (!selected(title)) {
        return;
    }
    std::printf("==== %s ====\n", title.c_str());
    static const char* extensions[] = { "rw466tile", nullptr };
    OIIO::declare_imageio_format("rw466tile", createNearIntMaxInput, extensions, nullptr, nullptr, "1");
    struct TiledFile {
        const char* name;
        int origin, tile;
    };
    const TiledFile files[] = {
        { "tiled128-origin128", 128, 128 },
        { "tiled128-near-intmax", INT_MAX - 128, 128 },
        { "tiled129-near-intmax", INT_MAX - 128, 129 },
    };
    Reader r[3];
    for (int k = 0; k < 3; ++k) {
        const std::string path = dir + "/" + files[k].name + ".rw466tile";
        std::FILE* file = std::fopen(path.c_str(), "w");
        const bool written = file && (std::fprintf(file, "%d %d\n", files[k].origin, files[k].tile) > 0);
        if (!file || (std::fclose(file) != 0) || !written) {
            check(false, "write %s", path.c_str());

            return;
        }
        if (!r[k].create(p, path, 0)) {
            check(false, "create %s", files[k].name);

            return;
        }
        const OfxStatus st = r[k].getRoD(1.);
        const bool rodOK = ok(st) && r[k].rod.x1 == 0 && r[k].rod.y1 == 0 && r[k].rod.x2 == kTiledW && r[k].rod.y2 == kTiledH;
        check(rodOK, "%s: RoD %d,%d,%d,%d (status %d)", files[k].name, r[k].rod.x1, r[k].rod.y1, r[k].rod.x2, r[k].rod.y2, st);
        if (!rodOK) {
            return;
        }
    }
    std::vector<OfxRectI> wins = windows(r[0].rod);
    wins.push_back(r[0].rod);
    int failed = 0, bad = 0, succeeded = 0, noError = 0, written = 0;
    for (const OfxRectI& win : wins) {
        for (int k = 0; k < 3; ++k) {
            r[k].setImage(win, 1., true);
            gPersistentError.clear();
            const OfxStatus st = r[k].render(win, 1., false);
            if (k < 2) {
                failed += (ok(st) && gPersistentError.empty()) ? 0 : 1;
            } else {
                succeeded += ok(st) ? 1 : 0;
                noError += gPersistentError.empty() ? 1 : 0;
                const size_t n = (size_t)(win.x2 - win.x1) * (win.y2 - win.y1) * 4;
                for (size_t i = 0; i < n; ++i) {
                    written += isUnwrittenBits(&r[k].data[i]) ? 0 : 1;
                }
            }
        }
        for (int y = win.y1; y < win.y2; ++y) {
            for (int x = win.x1; x < win.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    if (!sameBits(&r[1].pixel(x, y)[c], &r[0].pixel(x, y)[c])) {
                        if (bad < 3) {
                            std::printf("  window %d,%d,%d,%d pixel (%d,%d) channel %d: %.9g, expected %.9g\n", win.x1, win.y1,
                                        win.x2, win.y2, x, y, c, r[1].pixel(x, y)[c], r[0].pixel(x, y)[c]);
                        }
                        ++bad;
                    }
                }
            }
        }
    }
    check(failed == 0 && bad == 0, "tiles of 128, ending at INT_MAX: %d renders, %d failed, %d values differ from the file at 128",
          (int)wins.size() * 2, failed, bad);
    check(succeeded == 0 && noError == 0 && written == 0,
          "tiles of 129, ending past INT_MAX: %d renders, %d succeeded, %d without an error message, %d values written", (int)wins.size(),
          succeeded, noError, written);
}

// An RGB or grey file (no alpha) read as RGBA with every filePremult, without and with an OCIO conversion (#234): the
// output is the file's colours (converted, if OCIO is not identity; R = G = B for a grey file) and alpha is 1, as
// GenericReader sets it for Opaque files. ReadPFM used to write alpha = 0, which GenericReader premultiplied by
// (UnPreMultiplied, or PreMultiplied with OCIO: unpremultiply, convert, premultiply), so the colours were 0. The conversion is from x2 to x1 of swap.ocio (next to $OCIO): the colours are multiplied by 2.
void
testRGBAlpha(OfxPlugin* p,
             const std::string& dir,
             const char* name,
             const File& f)
{
    const std::string path = dir + "/" + name + extension(f.kind);
    if (!writeFile(path, f)) {
        check(false, "%s: write the file", name);

        return;
    }
    const std::string ocio = std::getenv("OCIO") ? std::getenv("OCIO") : "";
    const size_t slash = ocio.find_last_of("/\\");
    const std::string swapOCIO = ((slash == std::string::npos) ? std::string() : ocio.substr(0, slash + 1)) + "swap.ocio";
    for (int filePremult : { 0, 1, 2 }) {
        for (bool withOCIO : { false, true }) {
            const std::string title = strprintf("%s %s alpha, filePremult %d%s", f.plugin + 16, name, filePremult, withOCIO ? ", OCIO x2 to x1" : "");
            if (!selected(title)) {
                continue;
            }
            std::printf("==== %s ====\n", title.c_str());
            Reader r;
            if (withOCIO) {
                r.ocioConfig = swapOCIO;
                r.ocioInput = "x2";
                r.ocioOutput = "x1";
            }
            if (!r.create(p, path, filePremult)) {
                check(false, "create");

                continue;
            }
            OfxStatus st = r.getRoD(1.);
            if (!ok(st)) {
                check(false, "getRegionOfDefinition = %d", st);

                continue;
            }
            r.setImage(r.rod, 1., true);
            gPersistentError.clear();
            st = r.render(r.rod, 1., false);
            check(ok(st) && gPersistentError.empty(), "render = %d%s%s", st, gPersistentError.empty() ? "" : ", error: ",
                  gPersistentError.c_str());
            const float k = withOCIO ? 2.f : 1.f;
            int bad = 0;
            for (int y = r.rod.y1; y < r.rod.y2; ++y) {
                for (int x = r.rod.x1; x < r.rod.x2; ++x) {
                    // PFM is stored bottom-up, the others top-down
                    const int fy = (f.kind == ePFM) ? y : f.height - 1 - y;
                    for (int c = 0; c < 4; ++c) {
                        const float v = (c == 3) ? 1.f : k * f.fileValue(x, fy, (f.nchannels == 1) ? 0 : c);
                        const float px = r.pixel(x, y)[c];
                        if (!(std::fabs(px - v) <= 1e-6f * (1.f + std::fabs(v)))) {
                            if (bad < 3) {
                                std::printf("  pixel (%d,%d) channel %d: %.9g, expected %.9g\n", x, y, c, px, v);
                            }
                            ++bad;
                        }
                    }
                }
            }
            check(bad == 0, "the file's colours%s, and alpha 1 (%d wrong)", withOCIO ? " times 2" : "", bad);
        }
    }
}

// The file is replaced by a smaller or a larger one after the host got the RoD: the renders with the old RoD only
// write their window (GenericReader fills the part out of the new frame bounds with black, decode() gets a window
// that may not be in the file it opened).
void
testRewrite(OfxPlugin* p,
            const std::string& dir,
            const char* name,
            const File& a,
            const File& b)
{
    const std::string path = dir + "/" + name + extension(a.kind);
    for (double scale : { 1., 0.5 }) {
        const std::string title = strprintf("%s %s, file rewritten, scale %g", a.plugin + 16, name, scale);
        if (!selected(title)) {
            continue;
        }
        std::printf("==== %s ====\n", title.c_str());
        Reader r;
        if (!writeFile(path, a) || !r.create(p, path, (a.nchannels == 4) ? 2 : 0) || !ok(r.getRoD(scale))) {
            check(false, "first file");

            return;
        }
        const OfxRectI rod = r.rod;
        if (!writeFile(path, b)) {
            check(false, "second file");

            return;
        }
        std::vector<OfxRectI> ws = windows(rod);
        ws.push_back(rod);
        // the frame bounds of the new file, in full-resolution pixels
        const OfxRectI nb = b.frameBounds();
        int bad = 0, badAlpha = 0, n = 0;
        for (const OfxRectI& w : ws) {
            for (int k = 0; k < 2; ++k) {
                r.rod = rod;
                r.setImage(rod, scale, k == 0);
                gPersistentError.clear();
                const bool rendered = ok(r.render(w, scale, false)); // (the render may fail)
                for (int y = r.img.y1; y < r.img.y2; ++y) {
                    for (int x = r.img.x1; x < r.img.x2; ++x) {
                        if (x >= w.x1 && x < w.x2 && y >= w.y1 && y < w.y2) {
                            // An RGB file is Opaque: alpha is 1 in the new file's image, and the part of the
                            // window out of it is transparent black (it is out of the RoD of the file). At
                            // scale 1/2, only the pixels entirely in or out of the image.
                            if (!rendered || (b.nchannels == 4)) {
                                continue;
                            }
                            const double fx1 = x / scale, fy1 = y / scale, fx2 = (x + 1) / scale, fy2 = (y + 1) / scale;
                            const bool in = (fx1 >= nb.x1) && (fy1 >= nb.y1) && (fx2 <= nb.x2) && (fy2 <= nb.y2);
                            const bool out = (fx2 <= nb.x1) || (fy2 <= nb.y1) || (fx1 >= nb.x2) || (fy1 >= nb.y2);
                            const float* px = r.pixel(x, y);
                            if ((in && (px[3] != 1.f)) || (out && ((px[0] != 0.f) || (px[1] != 0.f) || (px[2] != 0.f) || (px[3] != 0.f)))) {
                                if (badAlpha < 3) {
                                    std::printf("  window %d,%d,%d,%d: pixel (%d,%d) %s the new image: (%g,%g,%g,%g)\n", w.x1, w.y1, w.x2, w.y2, x, y, in ? "in" : "out of",
                                                px[0], px[1], px[2], px[3]);
                                }
                                ++badAlpha;
                            }
                            continue;
                        }
                        for (int c = 0; c < 4; ++c) {
                            if (!isUnwrittenBits(&r.pixel(x, y)[c])) {
                                if (bad < 3) {
                                    std::printf("  window %d,%d,%d,%d: pixel (%d,%d) channel %d out of the window was written\n", w.x1, w.y1, w.x2, w.y2, x, y, c);
                                }
                                ++bad;
                            }
                        }
                    }
                }
                ++n;
            }
        }
        check(bad == 0, "%d renders with the old RoD: %d values written out of the window", n, bad);
        if (b.nchannels < 4) {
            check(badAlpha == 0, "Opaque file: alpha = 1 in the new image, transparent black out of it (%d wrong pixels)", badAlpha);
        }
    }
}

// Concurrent renders of narrow windows by several instances, while another thread replaces the file by a smaller
// and a larger one (rename(), atomic): no render writes out of its window (#163). inPlace: the file is overwritten in
// place instead (truncated, then written), so that a reader may see either file, or a part of one: renders may fail,
// but they must not crash or write out of their window (#242). A render that a write overlapped may have read the
// header of one file and pixels of the other, which no reader can detect without locking the file: its pixels (alpha)
// are only checked if no write overlapped it. At least one replacement must succeed (on MS Windows,
// a file that is open can't be replaced, nor maybe written: there, the test is reported as skipped if none did).
void
testConcurrentRewrite(OfxPlugin* p,
                      const std::string& dir,
                      const char* name,
                      const File& a,
                      const File& b,
                      bool inPlace = false)
{
    const std::string title = strprintf("%s %s, concurrent renders while the file is %s", a.plugin + 16, name,
                                        inPlace ? "overwritten in place" : "replaced");
    if (!selected(title)) {
        return;
    }
    std::printf("==== %s ====\n", title.c_str());
    const std::string path = dir + "/" + name + extension(a.kind);
    const std::string pathA = dir + "/" + name + ".a" + extension(a.kind);
    const std::string pathB = dir + "/" + name + ".b" + extension(a.kind);
    if (!writeFile(path, a) || !writeFile(pathA, a) || !writeFile(pathB, b)) {
        check(false, "files");

        return;
    }
    const int kThreads = 4;
    const int kRenders = 50;
    std::vector<Reader> readers(kThreads);
    for (Reader& r : readers) {
        if (!r.create(p, path, (a.nchannels == 4) ? 2 : 0) || !ok(r.getRoD(1.))) {
            check(false, "create");

            return;
        }
    }
    std::atomic<bool> done(false);
    std::atomic<int> rewrites(0);
    // in place: the writes started and finished (a render that none overlapped is one that began with them equal,
    // and ended with the same number of writes started)
    std::atomic<int> writesBegun(0), writesEnded(0);
    std::thread rewriter([&]() {
        // copy one of the files and rename it over the file read, or copy it over the file read (in place)
        for (int i = 0; !done; ++i) {
            const std::string tmp = inPlace ? path : path + ".tmp";
            std::FILE* in = std::fopen(((i % 2) ? pathA : pathB).c_str(), "rb");
            ++writesBegun;
            std::FILE* out = std::fopen(tmp.c_str(), "wb");
            bool written = in && out;
            if (written) {
                char buf[65536];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
                    written = written && (std::fwrite(buf, 1, n, out) == n);
                }
            }
            if (in) {
                std::fclose(in);
            }
            if (out) {
                written = (std::fclose(out) == 0) && written;
            }
            if (written && !inPlace) {
#ifdef _WIN32
                // (std::rename() fails if the file exists on MS Windows)
                written = MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
                written = std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
            }
            ++writesEnded;
            if (written) {
                ++rewrites;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::atomic<int> bad(0), badAlpha(0), renders(0), failed(0), raced(0), racedBadAlpha(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            Reader& r = readers[t];
            const OfxRectI rod = r.rod;
            const std::vector<OfxRectI> ws = windows(rod);
            for (int i = 0; i < kRenders; ++i) {
                const OfxRectI& w = ws[(size_t)(i + t) % ws.size()];
                r.setImage(rod, 1., (i % 2) == 0);
                const int begunBefore = writesBegun;
                const bool quiet = (begunBefore == writesEnded); // (no write in progress)
                const bool rendered = ok(r.render(w, 1., (i + t) % 2)); // (the render may fail)
                const bool overlapped = inPlace && !(quiet && (writesBegun == begunBefore));
                if (!rendered) {
                    ++failed;
                } else if (overlapped) {
                    ++raced;
                }
                for (int y = r.img.y1; y < r.img.y2; ++y) {
                    for (int x = r.img.x1; x < r.img.x2; ++x) {
                        if (x >= w.x1 && x < w.x2 && y >= w.y1 && y < w.y2) {
                            // Opaque RGB files, whose green is never 0 (floatValue()): a pixel of one of the files
                            // has alpha 1, the black out of the file that decode() opened (rewritten smaller since
                            // getFrameBounds()) is transparent
                            const float* px = r.pixel(x, y);
                            if (rendered && (a.nchannels < 4) && (px[3] != ((px[1] != 0.f) ? 1.f : 0.f))) {
                                ++(overlapped ? racedBadAlpha : badAlpha);
                            }
                            continue;
                        }
                        for (int c = 0; c < 4; ++c) {
                            if (!isUnwrittenBits(&r.pixel(x, y)[c])) {
                                ++bad;
                            }
                        }
                    }
                }
                ++renders;
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    done = true;
    rewriter.join();
    std::printf("  %d renders, %d failed, %d files %s\n", (int)renders, (int)failed, (int)rewrites,
                inPlace ? "overwritten" : "replaced");
    if (inPlace) {
        // (e.g. the header of the small file and the first strip of the large one, if it is at the same offset,
        // decode without an error into garbage)
        std::printf("  %d renders succeeded with no write in between (checked), %d while the file was written (not "
                    "checked: %d pixels with alpha != (green != 0))\n",
                    (int)renders - (int)failed - (int)raced, (int)raced, (int)racedBadAlpha);
    }
#ifdef _WIN32
    if (rewrites == 0) {
        std::printf("  SKIPPED: the file could not be %s while it was read\n", inPlace ? "overwritten" : "replaced");
    }
#else
    check(rewrites > 0, "the file was %s (%d times)", inPlace ? "overwritten" : "replaced", (int)rewrites);
#endif
    check(bad == 0, "%d renders: %d values written out of the windows", (int)renders, (int)bad);
    if (a.nchannels < 4) {
        check(badAlpha == 0, "Opaque files: alpha = 1 in the image, 0 in the black (%d wrong pixels)", (int)badAlpha);
    }
}

// A 2-part EXR (in a Natron-like host: dynamic choices and the plane suite): part 0 is a full-frame RGB image, part 1
// ("aov") a smaller RGB one. ReadOIIO reads part 1 into the color plane, Opaque. The frame bounds are the union of the
// parts' (with the black border of part 1's data window), as getFrameBounds() computes them: alpha is 1 in the whole
// RoD, also out of part 1, and the colors are part 1's, black out of its data window (#232 review: decodePlane() must
// not compute the frame bounds of the file from the spec that seek_subimage() wrote over subimages[0]).
void
testMultiPart(OfxPlugin* p,
              const std::string& dir)
{
    const std::string path = dir + "/multipart.exr";
    const int W = 61, H = 47;
    const int ax = 9, ay = 7, aw = 23, ah = 17; // part 1's data window, in the display window (y down)
    {
        OIIO::ImageSpec specs[2] = { OIIO::ImageSpec(W, H, 3, OIIO::TypeDesc::FLOAT), OIIO::ImageSpec(aw, ah, 3, OIIO::TypeDesc::FLOAT) };
        specs[0].attribute("oiio:subimagename", "rgba");
        specs[1].x = ax;
        specs[1].y = ay;
        specs[1].full_x = specs[1].full_y = 0;
        specs[1].full_width = W;
        specs[1].full_height = H;
        specs[1].channelnames = { "aov.R", "aov.G", "aov.B" };
        specs[1].attribute("oiio:subimagename", "aov");
        auto out = OIIO::ImageOutput::create(path);
        bool written = out && out->open(path, 2, specs);
        for (int part = 0; written && (part < 2); ++part) {
            if (part == 1) {
                written = out->open(path, specs[1], OIIO::ImageOutput::AppendSubimage);
            }
            std::vector<float> pix;
            for (int y = 0; y < specs[part].height; ++y) {
                for (int x = 0; x < specs[part].width; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        pix.push_back(floatValue(x + specs[part].x, y + specs[part].y, c) + (part ? 10.f : 0.f));
                    }
                }
            }
            written = written && out->write_image(OIIO::TypeDesc::FLOAT, pix.data());
        }
        if (!out || !out->close() || !written) {
            check(false, "write a 2-part EXR: %s", out ? out->geterror().c_str() : OIIO::geterror().c_str());

            return;
        }
    }
    const std::string title = "ReadOIIO 2-part EXR, part 1 (smaller) in the color plane, Opaque";
    if (!selected(title)) {
        return;
    }
    std::printf("==== %s ====\n", title.c_str());
    Reader r;
    if (!r.create(p, path, 0, "aov.RGB") || !ok(r.getRoD(1.))) {
        check(false, "create");

        return;
    }
    check(r.rod.x1 == 0 && r.rod.y1 == 0 && r.rod.x2 == W && r.rod.y2 == H, "RoD %d,%d,%d,%d (the union of the parts)", r.rod.x1, r.rod.y1, r.rod.x2, r.rod.y2);
    r.setImage(r.rod, 1., true);
    gPersistentError.clear();
    const OfxStatus st = r.render(r.rod, 1., false);
    {
        Image image;
        image.bounds = r.rod;
        image.pixels.assign(r.data, r.data + (size_t)(r.rod.x2 - r.rod.x1) * (r.rod.y2 - r.rod.y1) * 4);
        dumpImage("ReadOIIO multipart.exr aov", image);
    }
    int bad = 0;
    for (int y = r.rod.y1; y < r.rod.y2; ++y) {
        for (int x = r.rod.x1; x < r.rod.x2; ++x) {
            const int fy = H - 1 - y;
            const bool inAOV = (x >= ax) && (x < ax + aw) && (fy >= ay) && (fy < ay + ah);
            const float* px = r.pixel(x, y);
            for (int c = 0; c < 4; ++c) {
                const float v = (c == 3) ? 1.f : inAOV ? floatValue(x, fy, c) + 10.f : 0.f;
                if (!(std::fabs(px[c] - v) <= 1e-6f * (1.f + std::fabs(v)))) {
                    if (bad < 3) {
                        std::printf("  pixel (%d,%d) channel %d: %.9g, expected %.9g\n", x, y, c, px[c], v);
                    }
                    ++bad;
                }
            }
        }
    }
    check(ok(st) && gPersistentError.empty() && bad == 0, "render = %d%s%s: part 1's pixels, alpha 1 in the whole RoD (%d wrong)", st,
          gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str(), bad);
}

} // namespace

int
main(int argc,
     char** argv)
{
    if (argc != 2) {
        std::printf("usage: %s <scratch dir>\n", argv[0]);

        return 2;
    }
    const std::string dir = argv[1];
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // (the output before a crash)
    initHost();
    // $READER_WINDOW_MULTIPLANE=1: a Natron-like host (dynamic choices, multiple planes), for the 2-part EXR test only
    const bool multiPlane = std::getenv("READER_WINDOW_MULTIPLANE") && !std::strcmp(std::getenv("READER_WINDOW_MULTIPLANE"), "1");
    if (multiPlane) {
        setMultiPlanarHost();
    }
    std::vector<const char*> ids = { "fr.inria.openfx.ReadOIIO", "fr.inria.openfx.ReadPFM" };
#ifdef HAVE_PNG
    ids.push_back("fr.inria.openfx.ReadPNG");
#endif
    std::vector<Effect> descs(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        OfxPlugin* p = findPlugin(ids[i]);
        if (!p) {
            std::printf("FAIL: %s not found\n", ids[i]);

            return 1;
        }
        p->setHost(&gOfxHost);
        if (!ok(p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr)) || !ok(p->mainEntry(kOfxActionDescribe, EH(descs[i]), nullptr, nullptr))) {
            std::printf("FAIL: %s load/describe\n", ids[i]);

            return 1;
        }
    }
    OfxPlugin* oiio = findPlugin("fr.inria.openfx.ReadOIIO");
    OfxPlugin* pfm = findPlugin("fr.inria.openfx.ReadPFM");
    if (multiPlane) {
        testMultiPart(oiio, dir);
        for (size_t i = 0; i < ids.size(); ++i) {
            findPlugin(ids[i])->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
        }
        std::printf("%s (%d failure%s)\n", gFailures ? "FAIL" : "PASS", gFailures, gFailures == 1 ? "" : "s");

        return gFailures ? 1 : 0;
    }

    // (odd sizes: the windows at scale 1/2 are rounded)
    const File tiffRGB = { "fr.inria.openfx.ReadOIIO", eTIFF, 61, 47, 3, 0, 0, 0, 0 };
    const File tiffRGBA = { "fr.inria.openfx.ReadOIIO", eTIFF, 61, 47, 4, 0, 0, 0, 0 };
    const File exr = { "fr.inria.openfx.ReadOIIO", eEXR, 80, 60, 4, 7, 5, 51, 41 };
    const File exrTiled = { "fr.inria.openfx.ReadOIIO", eEXRTiled, 80, 60, 3, 9, 6, 53, 39 };
    const File pfmRGB = { "fr.inria.openfx.ReadPFM", ePFM, 61, 47, 3, 0, 0, 0, 0 };
    testFile(oiio, dir, "rgb", tiffRGB);
    testFile(oiio, dir, "rgba", tiffRGBA);
    testFile(oiio, dir, "datawindow", exr);
    testFile(oiio, dir, "datawindow-tiled", exrTiled);
    // a tiled data window that ends near INT_MAX (#466)
    testTiledNearIntMax(oiio, dir);
    // grey (R = G = B: the duplicated channels), and a data window with a negative origin that extends past the
    // display window (overscan)
    const File tiffGrey = { "fr.inria.openfx.ReadOIIO", eTIFF, 61, 47, 1, 0, 0, 0, 0 };
    const File exrOverscan = { "fr.inria.openfx.ReadOIIO", eEXR, 80, 60, 4, -6, -4, 95, 70 };
    testFile(oiio, dir, "grey", tiffGrey);
    testFile(oiio, dir, "overscan", exrOverscan);
    testFile(pfm, dir, "rgb", pfmRGB);
    // a grey PFM ("Pf"), read as RGBA
    const File pfmGrey = { "fr.inria.openfx.ReadPFM", ePFM, 61, 47, 1, 0, 0, 0, 0 };
    testFile(pfm, dir, "grey", pfmGrey);
    // RGB and grey files: alpha 1 with any filePremult, also with an OCIO conversion (#234)
    testRGBAlpha(pfm, dir, "rgb", pfmRGB);
    testRGBAlpha(pfm, dir, "grey", pfmGrey);
    testRGBAlpha(oiio, dir, "rgb", tiffRGB);
    testRGBAlpha(oiio, dir, "grey", tiffGrey);
#ifdef HAVE_PNG
    OfxPlugin* png = findPlugin("fr.inria.openfx.ReadPNG");
    const File pngRGB = { "fr.inria.openfx.ReadPNG", ePNG, 61, 47, 3, 0, 0, 0, 0 };
    const File pngRGBA = { "fr.inria.openfx.ReadPNG", ePNG, 61, 47, 4, 0, 0, 0, 0 };
    testFile(png, dir, "rgb", pngRGB);
    testRGBAlpha(png, dir, "rgb", pngRGB);
    testFile(png, dir, "rgba", pngRGBA);
    // read row by row, only down to the window, unless interlaced (read whole)
    const File pngGrey16 = { "fr.inria.openfx.ReadPNG", ePNG, 61, 47, 1, 0, 0, 0, 0, 16 };
    const File pngRGBA16 = { "fr.inria.openfx.ReadPNG", ePNG, 61, 47, 4, 0, 0, 0, 0, 16 };
    const File pngInterlaced = { "fr.inria.openfx.ReadPNG", ePNG, 61, 47, 3, 0, 0, 0, 0, 8, true };
    const File pngInterlaced16 = { "fr.inria.openfx.ReadPNG", ePNG, 61, 47, 4, 0, 0, 0, 0, 16, true };
    testFile(png, dir, "grey16", pngGrey16);
    testFile(png, dir, "rgba16", pngRGBA16);
    testFile(png, dir, "interlaced", pngInterlaced);
    testFile(png, dir, "interlaced16", pngInterlaced16);
    // offsets near the limits of an int, at scales 1/2 and 1/4 (#371)
    testLargeOffsets(png, dir, pngRGBA);
#endif

    // render scales that are not a power of two, or not the same in x and y (#194)
    testScales(oiio, dir, "rgb", tiffRGB);
    testScales(oiio, dir, "datawindow", exr);
    testScales(oiio, dir, "datawindow-tiled", exrTiled);
    testScales(pfm, dir, "rgb", pfmRGB);
#ifdef HAVE_PNG
    testScales(png, dir, "rgba", pngRGBA);
#endif

    // files rewritten: smaller, then larger
    const File tiffSmall = { "fr.inria.openfx.ReadOIIO", eTIFF, 23, 19, 3, 0, 0, 0, 0 };
    const File tiffLarge = { "fr.inria.openfx.ReadOIIO", eTIFF, 97, 71, 3, 0, 0, 0, 0 };
    testRewrite(oiio, dir, "rewrite-smaller", tiffRGB, tiffSmall);
    testRewrite(oiio, dir, "rewrite-larger", tiffRGB, tiffLarge);
    const File exrSmall = { "fr.inria.openfx.ReadOIIO", eEXR, 80, 60, 4, 20, 15, 11, 9 };
    testRewrite(oiio, dir, "rewrite-datawindow", exr, exrSmall);
    // RGB (Opaque): alpha = 1 in the black border of the data window, which is in the RoD
    const File exrTiledSmall = { "fr.inria.openfx.ReadOIIO", eEXRTiled, 80, 60, 3, 20, 15, 11, 9 };
    testRewrite(oiio, dir, "rewrite-datawindow-rgb", exrTiled, exrTiledSmall);
    const File pfmSmall = { "fr.inria.openfx.ReadPFM", ePFM, 23, 19, 3, 0, 0, 0, 0 };
    const File pfmLarge = { "fr.inria.openfx.ReadPFM", ePFM, 97, 71, 3, 0, 0, 0, 0 };
    testRewrite(pfm, dir, "rewrite-smaller", pfmRGB, pfmSmall);
    testRewrite(pfm, dir, "rewrite-larger", pfmRGB, pfmLarge);
#ifdef HAVE_PNG
    const File pngSmall = { "fr.inria.openfx.ReadPNG", ePNG, 23, 19, 3, 0, 0, 0, 0 };
    const File pngLarge = { "fr.inria.openfx.ReadPNG", ePNG, 97, 71, 3, 0, 0, 0, 0 };
    testRewrite(png, dir, "rewrite-smaller", pngRGB, pngSmall);
    testRewrite(png, dir, "rewrite-larger", pngRGB, pngLarge);
    const File pngInterlacedSmall = { "fr.inria.openfx.ReadPNG", ePNG, 23, 19, 3, 0, 0, 0, 0, 8, true };
    testRewrite(png, dir, "rewrite-interlaced-smaller", pngInterlaced, pngInterlacedSmall);
#endif
    testConcurrentRewrite(oiio, dir, "concurrent", tiffSmall, tiffLarge);
    testConcurrentRewrite(pfm, dir, "concurrent", pfmSmall, pfmLarge);
    testConcurrentRewrite(oiio, dir, "concurrent-in-place", tiffSmall, tiffLarge, true);

    for (size_t i = 0; i < ids.size(); ++i) {
        findPlugin(ids[i])->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    }
    std::printf("%s (%d failure%s)\n", gFailures ? "FAIL" : "PASS", gFailures, gFailures == 1 ? "" : "s");

    return gFailures ? 1 : 0;
}
