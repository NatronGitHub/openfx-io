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
 * reader_window_test — render windows and render scales of ReadOIIO, ReadPFM and ReadPNG (issues #160 and #163),
 * driven by the in-process host (ofx_test_host.h). Built with AddressSanitizer (make test-reader-window), and the
 * output image is between two guard pages (PROT_NONE), flush against one of them, so that a write out of it crashes.
 *
 *  - render scale 1/2: GenericReader decodes a full-resolution window into its buffer, then downscales it; decode()
 *    gets a render scale of 1 (ReadOIIO and ReadPFM assert it, #160)
 *  - render windows narrower than the image (a few columns, a few rows, a box, a pixel, at the edges), at scale 1
 *    (decode() writes in the output image, or in GenericReader's buffer if the image is premultiplied by
 *    GenericReader) and 1/2 (GenericReader's buffer): only the window is written, with the pixels of the whole
 *    image's render (#163). ReadOIIO supports tiles: also with an output image that is the render window only.
 *    (ReadOIIO reads with OpenImageIO's ImageInput: its ImageCache is only used in Natron < 2.2.)
 *  - the files: ReadOIIO, float TIFF (RGB, RGBA), scanline and tiled EXR with a data window smaller than the display
 *    window; ReadPFM, a PFM (color); ReadPNG (if built with libpng), 8-bit RGB and RGBA PNG. At scale 1, the whole
 *    image is the file's pixels (premultiplied by GenericReader for the RGBA files, read as unpremultiplied).
 *  - a file rewritten with a smaller or larger image, rendered with the RoD that the host got before (decode() then
 *    gets a render window that is not the file's): only the window is written
 *  - concurrent renders while another thread replaces the file by smaller and larger ones (#163): only the
 *    windows are written
 *
 * Usage: reader_window_test <scratch dir>   (needs $OCIO, see tests/Makefile)
 *        $READER_WINDOW_ONLY: run only the tests whose title contains it (e.g. "ReadPFM", "rewrite", "scale 0.5")
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <OpenImageIO/imageio.h>

#include <atomic>
#include <chrono>
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

int gFailures = 0;

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

void
check(bool cond, const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    std::printf("  %s: ", cond ? "PASS" : "FAIL");
    std::vprintf(fmt, a);
    std::printf("\n");
    va_end(a);
    if (!cond) {
        ++gFailures;
    }
}

inline OfxImageEffectHandle
H(Effect& e)
{
    return reinterpret_cast<OfxImageEffectHandle>(&e);
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

// The value of the pixels that the plug-in did not write, compared on its bits (never as a float)
const uint32_t kUnwrittenBits = 0xc49a5a5au; // -1234.8...

void
fillUnwrittenBits(float* p,
                  size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        std::memcpy(p + i, &kUnwrittenBits, sizeof(kUnwrittenBits));
    }
}

bool
isUnwrittenBits(const float* p)
{
    uint32_t bits;
    std::memcpy(&bits, p, sizeof(bits));

    return bits == kUnwrittenBits;
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

    bool create(OfxPlugin* plugin, const std::string& filename, bool unpremult)
    {
        p = plugin;
        PropSet inArgs;
        pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextReader);
        if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, H(ctx), PH(&inArgs), nullptr))) {
            std::printf("  describeInContext failed\n");

            return false;
        }
        setOverrideString(&ctx, kOfxImageEffectFileParamName, filename);
        inst.delegate = &ctx;
        setInstanceProps(inst, 100, 100, 1, kOfxImageEffectContextReader);
        if (!ok(p->mainEntry(kOfxActionCreateInstance, H(inst), nullptr, nullptr))) {
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
        PropSet a;
        OfxPropertySetHandle ap = PH(&a);
        pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
        pSetString(ap, kOfxPropName, 0, kOfxImageEffectFileParamName);
        pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
        pSetDouble(ap, kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. };
        pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        if (!ok(p->mainEntry(kOfxActionInstanceChanged, H(inst), PH(&a), nullptr))) {
            std::printf("  instanceChanged(filename) failed\n");

            return false;
        }
        // RGBA files: unpremultiplied, the output premultiplied (by GenericReader, from its buffer); RGB files: opaque
        setOverrideInt(&ctx, "filePremult", unpremult ? 2 : 0);
        setOverrideInt(&ctx, "outputPremult", 1);
        setOverrideInt(&ctx, "outputComponents", 0); // RGBA
        out->connected = true;
        setupClipImage(out, 1, 1, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        PropSet o;
        if (!ok(p->mainEntry(kOfxImageEffectActionGetClipPreferences, H(inst), nullptr, PH(&o)))) {
            std::printf("  getClipPreferences failed\n");

            return false;
        }

        return true;
    }

    // getRegionOfDefinition at the given render scale (rod, in pixels at that scale)
    OfxStatus getRoD(double scale)
    {
        PropSet a;
        pSetDouble(PH(&a), kOfxPropTime, 0, 1.);
        double rs[2] = { scale, scale };
        pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, 0);
        PropSet o;
        OfxStatus st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, H(inst), PH(&a), PH(&o));
        if (!ok(st)) {
            return st;
        }
        double r[4] = { 0., 0., 0., 0. };
        pGetDoubleN(PH(&o), kOfxImageEffectPropRegionOfDefinition, 4, r);
        rod = { (int)std::floor(r[0] * scale), (int)std::floor(r[1] * scale), (int)std::ceil(r[2] * scale), (int)std::ceil(r[3] * scale) };

        return kOfxStatOK;
    }

    // an output image with the given bounds (in pixels at the render scale), between guard pages, every value
    // unwritten; the RoD of the image is rod
    void setImage(const OfxRectI& bounds, double scale, bool atEnd)
    {
        img = bounds;
        double rs[2] = { scale, scale };
        const int w = img.x2 - img.x1;
        const int h = img.y2 - img.y1;
        rowBytes = w * 4 * (int)sizeof(float);
        const size_t n = (size_t)rowBytes * h;
        data = (float*)buffer.alloc(n, atEnd);
        fillUnwrittenBits(data, n / sizeof(float));
        OfxPropertySetHandle ip = PH(&out->image);
        pSetPointer(ip, kOfxImagePropData, 0, data);
        int b[4] = { img.x1, img.y1, img.x2, img.y2 };
        pSetIntN(ip, kOfxImagePropBounds, 4, b);
        int r[4] = { rod.x1, rod.y1, rod.x2, rod.y2 };
        pSetIntN(ip, kOfxImagePropRegionOfDefinition, 4, r);
        pSetInt(ip, kOfxImagePropRowBytes, 0, rowBytes);
        pSetDoubleN(ip, kOfxImageEffectPropRenderScale, 2, rs);
        double rd[4] = { rod.x1 / scale, rod.y1 / scale, rod.x2 / scale, rod.y2 / scale };
        pSetDoubleN(PH(&out->props), kOfxImageEffectPropRegionOfDefinition, 4, rd);
    }

    // render a window (in pixels at the render scale)
    OfxStatus render(const OfxRectI& window, double scale, bool sequential)
    {
        PropSet ra = makeRenderInArgs(1., 0, 0);
        int win[4] = { window.x1, window.y1, window.x2, window.y2 };
        pSetIntN(PH(&ra), kOfxImageEffectPropRenderWindow, 4, win);
        double rs[2] = { scale, scale };
        pSetDoubleN(PH(&ra), kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(PH(&ra), kOfxImageEffectPropSequentialRenderStatus, 0, sequential ? 1 : 0);

        return p->mainEntry(kOfxImageEffectActionRender, H(inst), PH(&ra), nullptr);
    }

    const float* pixel(int x, int y) const
    {
        return reinterpret_cast<const float*>(reinterpret_cast<const char*>(data) + (size_t)(y - img.y1) * rowBytes) + (size_t)(x - img.x1) * 4;
    }

    ~Reader()
    {
        if (created) {
            p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
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
        return 0.25f + ((x + 2 * y) % 4) * 0.25f; // 0.25 to 1
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

    bool isFloat() const { return kind != ePNG; }
    bool hasData(int fx, int fy) const
    {
        if ((kind != eEXR) && (kind != eEXRTiled)) {
            return fx >= 0 && fx < width && fy >= 0 && fy < height;
        }

        return fx >= dataX && fx < dataX + dataW && fy >= dataY && fy < dataY + dataH;
    }

    // the value of OFX pixel (x, y) at render scale 1 of the RGBA output, channel c (RGBA files: premultiplied by
    // GenericReader; RGB files: alpha is not checked)
    bool expected(int x, int y, int c, float* v) const
    {
        // PFM is stored bottom-up, the others top-down
        const int fx = x;
        const int fy = (kind == ePFM) ? y : height - 1 - y;
        if ((nchannels == 3) && (c == 3)) {
            // opaque: GenericReader sets alpha to 1
            return false;
        }
        if (!hasData(fx, fy)) {
            *v = 0.f;

            return true;
        }
        float value = isFloat() ? floatValue(fx, fy, c) : byteValue(fx, fy, c) * (1.f / 255);
        if ((nchannels == 4) && (c < 3)) {
            const float alpha = isFloat() ? floatValue(fx, fy, 3) : byteValue(fx, fy, 3) * (1.f / 255);
            value *= alpha;
        }
        *v = value;

        return true;
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
    std::fprintf(file, "PF\n%d %d\n%s\n", f.width, f.height, littleEndian ? "-1.0" : "1.0");
    std::vector<float> line((size_t)f.width * 3);
    for (int y = 0; y < f.height; ++y) { // bottom-up
        for (int x = 0; x < f.width; ++x) {
            for (int c = 0; c < 3; ++c) {
                line[(size_t)x * 3 + c] = floatValue(x, y, c);
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

bool
writeFile(const std::string& path,
          const File& f)
{
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

// The pixels of a render of window into r's image, compared to ref (the render of the whole RoD): the same bits in
// the window, unwritten elsewhere. Returns the number of wrong values.
int
compareWindow(const Reader& r,
              const Image& ref,
              const OfxRectI& window)
{
    int bad = 0;
    for (int y = r.img.y1; y < r.img.y2; ++y) {
        for (int x = r.img.x1; x < r.img.x2; ++x) {
            const bool inWindow = x >= window.x1 && x < window.x2 && y >= window.y1 && y < window.y2;
            const float* px = r.pixel(x, y);
            for (int c = 0; c < 4; ++c) {
                const bool good = inWindow ? sameBits(&px[c], &ref.pixel(x, y)[c]) : isUnwrittenBits(&px[c]);
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
    for (double scale : { 1., 0.5 }) {
        // (the sequential render status is decode()'s isPlayback)
        for (int sequential = 0; sequential <= 1; ++sequential) {
            const std::string title = strprintf("%s %s, scale %g, sequential %d", f.plugin + 16, name, scale, sequential);
            if (!selected(title)) {
                continue;
            }
            std::printf("==== %s ====\n", title.c_str());
            Reader r;
            if (!r.create(p, path, f.nchannels == 4)) {
                check(false, "create");

                return;
            }
            OfxStatus st = r.getRoD(scale);
            if (!ok(st)) {
                check(false, "getRegionOfDefinition = %d", st);

                continue;
            }
            // the whole image
            r.setImage(r.rod, scale, true);
            gPersistentError.clear();
            st = r.render(r.rod, scale, sequential);
            check(ok(st) && gPersistentError.empty(), "render %d,%d,%d,%d = %d%s%s", r.rod.x1, r.rod.y1, r.rod.x2, r.rod.y2, st,
                  gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
            Image ref;
            ref.bounds = r.rod;
            ref.pixels.assign(r.data, r.data + (size_t)(r.rod.x2 - r.rod.x1) * (r.rod.y2 - r.rod.y1) * 4);
            if (scale == 1.) {
                int bad = 0;
                for (int y = r.rod.y1; y < r.rod.y2; ++y) {
                    for (int x = r.rod.x1; x < r.rod.x2; ++x) {
                        for (int c = 0; c < 4; ++c) {
                            float v;
                            const float* px = ref.pixel(x, y);
                            if (f.expected(x, y, c, &v) && !(std::fabs(px[c] - v) <= 1e-6f * (1.f + std::fabs(v)))) {
                                if (bad < 3) {
                                    std::printf("  pixel (%d,%d) channel %d: %.9g, expected %.9g\n", x, y, c, px[c], v);
                                }
                                ++bad;
                            }
                        }
                    }
                }
                check(bad == 0, "the file's pixels (%d wrong)", bad);
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
                    st = r.render(w, scale, sequential);
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
        for (int sequential = 0; sequential <= 1; ++sequential) {
            const std::string title = strprintf("%s %s, file rewritten, scale %g, sequential %d", a.plugin + 16, name, scale, sequential);
            if (!selected(title)) {
                continue;
            }
            std::printf("==== %s ====\n", title.c_str());
            Reader r;
            if (!writeFile(path, a) || !r.create(p, path, a.nchannels == 4) || !ok(r.getRoD(scale))) {
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
            int bad = 0, n = 0;
            for (const OfxRectI& w : ws) {
                for (int k = 0; k < 2; ++k) {
                    r.rod = rod;
                    r.setImage(rod, scale, k == 0);
                    gPersistentError.clear();
                    r.render(w, scale, sequential); // (the render may fail)
                    // nothing out of the window
                    for (int y = r.img.y1; y < r.img.y2; ++y) {
                        for (int x = r.img.x1; x < r.img.x2; ++x) {
                            if (x >= w.x1 && x < w.x2 && y >= w.y1 && y < w.y2) {
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
        }
    }
}

// Concurrent renders of narrow windows by several instances, while another thread replaces the file by a smaller
// and a larger one (rename(), atomic): no render writes out of its window (#163).
void
testConcurrentRewrite(OfxPlugin* p,
                      const std::string& dir,
                      const char* name,
                      const File& a,
                      const File& b)
{
    const std::string title = strprintf("%s %s, concurrent renders while the file is replaced", a.plugin + 16, name);
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
        if (!r.create(p, path, a.nchannels == 4) || !ok(r.getRoD(1.))) {
            check(false, "create");

            return;
        }
    }
    std::atomic<bool> done(false);
    std::thread rewriter([&]() {
        // copy one of the files and rename it over the file read
        for (int i = 0; !done; ++i) {
            const std::string tmp = path + ".tmp";
            std::FILE* in = std::fopen(((i % 2) ? pathA : pathB).c_str(), "rb");
            std::FILE* out = std::fopen(tmp.c_str(), "wb");
            if (in && out) {
                char buf[65536];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
                    std::fwrite(buf, 1, n, out);
                }
            }
            if (in) {
                std::fclose(in);
            }
            if (out) {
                std::fclose(out);
            }
            std::rename(tmp.c_str(), path.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::atomic<int> bad(0), renders(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            Reader& r = readers[t];
            const OfxRectI rod = r.rod;
            const std::vector<OfxRectI> ws = windows(rod);
            for (int i = 0; i < kRenders; ++i) {
                const OfxRectI& w = ws[(size_t)(i + t) % ws.size()];
                r.setImage(rod, 1., (i % 2) == 0);
                r.render(w, 1., (i + t) % 2); // (the render may fail)
                for (int y = r.img.y1; y < r.img.y2; ++y) {
                    for (int x = r.img.x1; x < r.img.x2; ++x) {
                        if (x >= w.x1 && x < w.x2 && y >= w.y1 && y < w.y2) {
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
    check(bad == 0, "%d renders: %d values written out of the windows", (int)renders, (int)bad);
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
        if (!ok(p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr)) || !ok(p->mainEntry(kOfxActionDescribe, H(descs[i]), nullptr, nullptr))) {
            std::printf("FAIL: %s load/describe\n", ids[i]);

            return 1;
        }
    }
    OfxPlugin* oiio = findPlugin("fr.inria.openfx.ReadOIIO");
    OfxPlugin* pfm = findPlugin("fr.inria.openfx.ReadPFM");

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
    testFile(pfm, dir, "rgb", pfmRGB);
#ifdef HAVE_PNG
    OfxPlugin* png = findPlugin("fr.inria.openfx.ReadPNG");
    const File pngRGB = { "fr.inria.openfx.ReadPNG", ePNG, 61, 47, 3, 0, 0, 0, 0 };
    const File pngRGBA = { "fr.inria.openfx.ReadPNG", ePNG, 61, 47, 4, 0, 0, 0, 0 };
    testFile(png, dir, "rgb", pngRGB);
    testFile(png, dir, "rgba", pngRGBA);
#endif

    // files rewritten: smaller, then larger
    const File tiffSmall = { "fr.inria.openfx.ReadOIIO", eTIFF, 23, 19, 3, 0, 0, 0, 0 };
    const File tiffLarge = { "fr.inria.openfx.ReadOIIO", eTIFF, 97, 71, 3, 0, 0, 0, 0 };
    testRewrite(oiio, dir, "rewrite-smaller", tiffRGB, tiffSmall);
    testRewrite(oiio, dir, "rewrite-larger", tiffRGB, tiffLarge);
    const File exrSmall = { "fr.inria.openfx.ReadOIIO", eEXR, 80, 60, 4, 20, 15, 11, 9 };
    testRewrite(oiio, dir, "rewrite-datawindow", exr, exrSmall);
    const File pfmSmall = { "fr.inria.openfx.ReadPFM", ePFM, 23, 19, 3, 0, 0, 0, 0 };
    const File pfmLarge = { "fr.inria.openfx.ReadPFM", ePFM, 97, 71, 3, 0, 0, 0, 0 };
    testRewrite(pfm, dir, "rewrite-smaller", pfmRGB, pfmSmall);
    testRewrite(pfm, dir, "rewrite-larger", pfmRGB, pfmLarge);
#ifdef HAVE_PNG
    const File pngSmall = { "fr.inria.openfx.ReadPNG", ePNG, 23, 19, 3, 0, 0, 0, 0 };
    const File pngLarge = { "fr.inria.openfx.ReadPNG", ePNG, 97, 71, 3, 0, 0, 0, 0 };
    testRewrite(png, dir, "rewrite-smaller", pngRGB, pngSmall);
    testRewrite(png, dir, "rewrite-larger", pngRGB, pngLarge);
#endif
    testConcurrentRewrite(oiio, dir, "concurrent", tiffSmall, tiffLarge);
    testConcurrentRewrite(pfm, dir, "concurrent", pfmSmall, pfmLarge);

    for (size_t i = 0; i < ids.size(); ++i) {
        findPlugin(ids[i])->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    }
    std::printf("%s (%d failure%s)\n", gFailures ? "FAIL" : "PASS", gFailures, gFailures == 1 ? "" : "s");

    return gFailures ? 1 : 0;
}
