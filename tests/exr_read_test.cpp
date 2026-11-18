/*
 * exr_read_test — ReadEXR (EXR/ReadEXR.cpp, issue #123), driven by the in-process host (ofx_test_host.h):
 *
 *  - pixels: EXR files written here with OpenEXR (FLOAT, ZIP, PIZ and no compression), with various data and
 *    display windows, read back exactly: the display window is moved to (0,0), y is flipped, the 1-pixel border
 *    around the data window (when the windows differ) is black, and the channels that are not in the file are 0
 *  - UINT channels are normalized by 2^32-1, as in ReadOIIO
 *  - luminance/chroma files (Y, RY, BY): read as before #123 (not converted)
 *  - a render window narrower than the data window (in blocks of lines), render scale 1/2, and a file rewritten
 *    with other windows (the header cache is updated by the next render, or emptied by the purge caches action),
 *    including renders with the RoD the host got before the rewrite (GenericReader then fills with black the part
 *    of the render window out of the new frame bounds)
 *  - a truncated file: the render fails, with an error message (it used to succeed with rows left unwritten)
 *  - concurrent renders (ReadEXR is kOfxImageEffectRenderFullySafe): several instances render the same file
 *    and another one from several threads at once, and each must get the same image as a single render. Build
 *    with ThreadSanitizer (make test-exr-tsan) to check for data races too.
 *
 * Usage: exr_read_test <scratch dir>             (needs $OCIO, see tests/Makefile)
 *        exr_read_test --bench <scratch dir> [n]  times n renders (default 5) of 2048x1556 RGBA half files,
 *                                                 ZIP, PIZ and DWAA (OpenEXR's thread count: $EXR_THREADS)
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfThreading.h>
#include <half.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

// The base address of an OpenEXR slice: the address of pixel (0, 0), offset bytes before the start of the buffer,
// which may be out of the buffer. It is computed as an integer: pointer arithmetic out of an array is undefined
// behaviour (found by UBSan).
char*
sliceBase(const void* buffer,
          ptrdiff_t offset)
{
    return reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(buffer) - (uintptr_t)offset);
}

int gFailures = 0;

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

// ------------------------------------------------------------------ EXR files

// The value of EXR pixel (x, y) (OpenEXR coordinates, y down), channel c, in the test files: exact in half
// (multiples of 1/16 below 64) and distinct for each pixel and channel of the small files.
float
pixelValue(int x, int y, int c)
{
    const int v = ((x + 3 * y + 7 * c) % 97 + 97) % 97;

    return v / 16.f + c * 0.0625f;
}

// a value that compresses like a real image: a gradient and some noise
float
benchValue(int x, int y, int c, uint32_t* seed)
{
    *seed = *seed * 1664525u + 1013904223u;

    return (float)x / 2048.f * (c == 0) + (float)y / 1556.f * (c == 1) + 0.5f * (c == 2) + (c == 3 ? 1.f : 0.f)
        + ((*seed >> 16) & 0xff) / 4096.f;
}

struct ExrSpec {
    Imath::Box2i display;
    Imath::Box2i data;
    Imf::Compression compression = Imf::ZIP_COMPRESSION;
    Imf::PixelType type = Imf::FLOAT;
    std::string channels = "RGBA"; // among R, G, B, A
    bool uintRed = false;          // R is UINT (x * 100000 + y * 1000000)
    bool bench = false;            // benchValue() instead of pixelValue()
};

uint32_t
uintValue(int x, int y)
{
    return (uint32_t)(x * 100000 + y * 1000000) + 123u;
}

bool
writeExr(const std::string& path, const ExrSpec& s)
{
    try {
        Imf::Header header(s.display, s.data);
        header.compression() = s.compression;
        const int w = s.data.max.x - s.data.min.x + 1;
        const int h = s.data.max.y - s.data.min.y + 1;
        const int nc = (int)s.channels.size();
        std::vector<float> f((size_t)w * h * nc);
        std::vector<half> hf(s.type == Imf::HALF ? f.size() : 0);
        std::vector<uint32_t> u(s.uintRed ? (size_t)w * h : 0);
        uint32_t seed = 1;
        for (int y = s.data.min.y; y <= s.data.max.y; ++y) {
            for (int x = s.data.min.x; x <= s.data.max.x; ++x) {
                const size_t i = (size_t)(y - s.data.min.y) * w + (x - s.data.min.x);
                for (int k = 0; k < nc; ++k) {
                    const int c = (int)std::string("RGBA").find(s.channels[k]);
                    f[i * nc + k] = s.bench ? benchValue(x, y, c, &seed) : pixelValue(x, y, c);
                    if (!hf.empty()) {
                        hf[i * nc + k] = half(f[i * nc + k]);
                    }
                }
                if (!u.empty()) {
                    u[i] = uintValue(x, y);
                }
            }
        }
        Imf::FrameBuffer fb;
        for (int k = 0; k < nc; ++k) {
            const std::string name(1, s.channels[k]);
            if (s.uintRed && name == "R") {
                header.channels().insert(name, Imf::Channel(Imf::UINT));
                fb.insert(name, Imf::Slice(Imf::UINT, sliceBase(u.data(), (ptrdiff_t)(s.data.min.x + (ptrdiff_t)s.data.min.y * w) * (ptrdiff_t)sizeof(uint32_t)), sizeof(uint32_t), sizeof(uint32_t) * w));
                continue;
            }
            header.channels().insert(name, Imf::Channel(s.type));
            char* base;
            size_t ps;
            if (s.type == Imf::HALF) {
                base = (char*)(hf.data() + k);
                ps = sizeof(half);
            } else {
                base = (char*)(f.data() + k);
                ps = sizeof(float);
            }
            base = sliceBase(base, (ptrdiff_t)(s.data.min.x + (ptrdiff_t)s.data.min.y * w) * (ptrdiff_t)(nc * ps));
            fb.insert(name, Imf::Slice(s.type, base, nc * ps, (size_t)w * nc * ps));
        }
        Imf::OutputFile out(path.c_str(), header);
        out.setFrameBuffer(fb);
        out.writePixels(h);
    } catch (const std::exception& e) {
        std::printf("  cannot write %s: %s\n", path.c_str(), e.what());

        return false;
    }

    return true;
}

Imath::Box2i
box(int x1, int y1, int x2, int y2)
{
    return Imath::Box2i(Imath::V2i(x1, y1), Imath::V2i(x2, y2));
}

// ------------------------------------------------------------------ a ReadEXR instance

// An instance of ReadEXR in the Reader context, with its own clips and params (separate instances can render
// concurrently), and an output image that covers the RoD.
struct Reader {
    OfxPlugin* p = nullptr;
    Effect ctx; // context descriptor: clips and params
    Effect inst;
    bool created = false;
    Clip* out = nullptr;
    OfxRectI rod = { 0, 0, 0, 0 };
    OfxRectI img = { 0, 0, 0, 0 }; // the bounds of the output image

    bool create(OfxPlugin* plugin, const std::string& filename)
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
        out->connected = true;
        setupClipImage(out, 1, 1, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        PropSet o;
        if (!ok(p->mainEntry(kOfxImageEffectActionGetClipPreferences, H(inst), nullptr, PH(&o)))) {
            std::printf("  getClipPreferences failed\n");

            return false;
        }

        return true;
    }

    // getRegionOfDefinition, then an output image (RGBA float) that covers the RoD at the given render scale
    // (GenericReader asserts it, for a reader that doesn't support tiles), filled with NaN (so that unwritten pixels
    // are seen). rod is in canonical coordinates, img in pixels at that scale.
    OfxStatus prepare(double scale = 1.)
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
        rod = { (int)std::floor(r[0]), (int)std::floor(r[1]), (int)std::ceil(r[2]), (int)std::ceil(r[3]) };
        setImage({ (int)std::floor(r[0] * scale), (int)std::floor(r[1] * scale), (int)std::ceil(r[2] * scale), (int)std::ceil(r[3] * scale) }, scale);

        return kOfxStatOK;
    }

    // an output image with the given bounds (in pixels at the render scale), filled with NaN, without
    // getRegionOfDefinition (as a host that cached the RoD)
    void setImage(const OfxRectI& bounds, double scale)
    {
        img = bounds;
        double rs[2] = { scale, scale };
        const int w = img.x2 - img.x1;
        const int h = img.y2 - img.y1;
        const int rowBytes = w * 4 * (int)sizeof(float);
        out->buffer.assign((size_t)rowBytes * h, 0);
        fillUnwritten((float*)out->buffer.data(), (size_t)w * h * 4);
        OfxPropertySetHandle ip = PH(&out->image);
        pSetPointer(ip, kOfxImagePropData, 0, out->buffer.data());
        int b[4] = { img.x1, img.y1, img.x2, img.y2 };
        pSetIntN(ip, kOfxImagePropBounds, 4, b);
        pSetIntN(ip, kOfxImagePropRegionOfDefinition, 4, b);
        pSetInt(ip, kOfxImagePropRowBytes, 0, rowBytes);
        pSetDoubleN(ip, kOfxImageEffectPropRenderScale, 2, rs);
        double r[4] = { img.x1 / scale, img.y1 / scale, img.x2 / scale, img.y2 / scale };
        pSetDoubleN(PH(&out->props), kOfxImageEffectPropRegionOfDefinition, 4, r);
    }

    // render the whole image (prepare() first)
    OfxStatus render() { return render(img); }

    // render a window (in pixels at the render scale) of the image (prepare(scale) first)
    OfxStatus render(const OfxRectI& window, double scale = 1.)
    {
        PropSet ra = makeRenderInArgs(1., 0, 0);
        int win[4] = { window.x1, window.y1, window.x2, window.y2 };
        pSetIntN(PH(&ra), kOfxImageEffectPropRenderWindow, 4, win);
        double rs[2] = { scale, scale };
        pSetDoubleN(PH(&ra), kOfxImageEffectPropRenderScale, 2, rs);

        return p->mainEntry(kOfxImageEffectActionRender, H(inst), PH(&ra), nullptr);
    }

    // the purge caches action
    OfxStatus purgeCaches() { return p->mainEntry(kOfxActionPurgeCaches, H(inst), nullptr, nullptr); }

    OfxStatus prepareAndRender()
    {
        OfxStatus st = prepare();

        return ok(st) ? render() : st;
    }

    const float* pixel(int x, int y) const
    {
        return reinterpret_cast<const float*>(out->buffer.data()) + ((size_t)(y - img.y1) * (img.x2 - img.x1) + (x - img.x1)) * 4;
    }

    std::vector<float> image() const
    {
        const float* d = reinterpret_cast<const float*>(out->buffer.data());

        return std::vector<float>(d, d + out->buffer.size() / sizeof(float));
    }

    ~Reader()
    {
        if (created) {
            p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
        }
    }
};

// bitwise equality (NaN included)
bool
sameImage(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || !std::memcmp(a.data(), b.data(), a.size() * sizeof(float)));
}

// ------------------------------------------------------------------ tests

void
checkRoD(const Reader& r, const OfxRectI& expectedRoD)
{
    check(r.rod.x1 == expectedRoD.x1 && r.rod.y1 == expectedRoD.y1 && r.rod.x2 == expectedRoD.x2 && r.rod.y2 == expectedRoD.y2,
          "RoD %d,%d,%d,%d (expected %d,%d,%d,%d)", r.rod.x1, r.rod.y1, r.rod.x2, r.rod.y2,
          expectedRoD.x1, expectedRoD.y1, expectedRoD.x2, expectedRoD.y2);
}

// The number of wrong pixel values in a render of a file written with spec s, at render scale 1. In the window:
// the file's pixels (EXR pixel (x, y) is OFX pixel (x - display.min.x, display.max.y - y)), and 0 out of the data
// window and in the channels that are not in the file. Out of the window: NaN (not written).
int
wrongPixels(const Reader& r, const ExrSpec& s, const OfxRectI& window)
{
    int bad = 0;
    for (int y = r.img.y1; y < r.img.y2; ++y) {
        for (int x = r.img.x1; x < r.img.x2; ++x) {
            const int ex = x + s.display.min.x;
            const int ey = s.display.max.y - y;
            const bool inData = ex >= s.data.min.x && ex <= s.data.max.x && ey >= s.data.min.y && ey <= s.data.max.y;
            const bool inWindow = x >= window.x1 && x < window.x2 && y >= window.y1 && y < window.y2;
            const float* px = r.pixel(x, y);
            for (int c = 0; c < 4; ++c) {
                if (!inWindow) {
                    if (!isUnwritten(&px[c])) {
                        if (bad < 5) {
                            std::printf("  pixel (%d,%d) channel %d, out of the render window: %.9g, expected NaN\n", x, y, c, px[c]);
                        }
                        ++bad;
                    }
                    continue;
                }
                float expected = 0.f;
                if (inData && s.channels.find("RGBA"[c]) != std::string::npos) {
                    if (c == 0 && s.uintRed) {
                        expected = (float)((float)uintValue(ex, ey) * (1. / 4294967295.));
                    } else {
                        expected = pixelValue(ex, ey, c);
                    }
                }
                const bool good = (c == 0 && s.uintRed) ? std::fabs(px[c] - expected) <= 1e-6f * std::fabs(expected) : px[c] == expected;
                if (!good) {
                    if (bad < 5) {
                        std::printf("  pixel (%d,%d) channel %d: %.9g, expected %.9g\n", x, y, c, px[c], expected);
                    }
                    ++bad;
                }
            }
        }
    }

    return bad;
}

// Reads a file written with spec s, and checks its RoD and every pixel.
void
testPixels(OfxPlugin* p, const std::string& dir, const char* name, const ExrSpec& s, const OfxRectI& expectedRoD)
{
    std::printf("==== %s ====\n", name);
    const std::string path = dir + "/" + name + ".exr";
    if (!writeExr(path, s)) {
        ++gFailures;

        return;
    }
    Reader r;
    if (!r.create(p, path)) {
        ++gFailures;

        return;
    }
    gPersistentError.clear();
    OfxStatus st = r.prepareAndRender();
    check(ok(st) && gPersistentError.empty(), "render = %d%s%s", st, gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
    if (!ok(st)) {
        return;
    }
    checkRoD(r, expectedRoD);
    const int bad = wrongPixels(r, s, r.img);
    check(bad == 0, "every pixel, channel and border is the expected value (%d wrong)", bad);
}

// The render window is narrower than the data window: decode() then reads the lines into a buffer, and copies the
// part that is in the render window (the pixels out of it must not be written).
void
testWindow(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== render window narrower than the data window ====\n");
    const std::string path = dir + "/window.exr";
    ExrSpec s;
    s.display = box(5, 7, 104, 66);
    s.data = box(0, 3, 109, 69);
    if (!writeExr(path, s)) {
        ++gFailures;

        return;
    }
    Reader r;
    if (!r.create(p, path) || !ok(r.prepare())) {
        check(false, "getRegionOfDefinition");

        return;
    }
    const OfxRectI window = { r.rod.x1 + 7, r.rod.y1 + 4, r.rod.x2 - 9, r.rod.y2 - 2 };
    gPersistentError.clear();
    OfxStatus st = r.render(window);
    check(ok(st) && gPersistentError.empty(), "render of %d,%d,%d,%d in the RoD %d,%d,%d,%d = %d", window.x1, window.y1, window.x2,
          window.y2, r.rod.x1, r.rod.y1, r.rod.x2, r.rod.y2, st);
    const int bad = wrongPixels(r, s, window);
    check(bad == 0, "the render window has the expected values, the rest is not written (%d wrong)", bad);
}

// Render scale 1/2: GenericReader passes the render scale to decode() with a full-resolution render window (which
// a debug build of ReadEXR 1.0 asserted against), then halves the image.
void
testScale(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== render scale 1/2 ====\n");
    const std::string path = dir + "/scale.exr";
    ExrSpec s;
    s.display = box(0, 0, 99, 59);
    s.data = box(10, 20, 69, 49);
    if (!writeExr(path, s)) {
        ++gFailures;

        return;
    }
    Reader r;
    if (!r.create(p, path) || !ok(r.prepare(0.5))) {
        check(false, "getRegionOfDefinition");

        return;
    }
    gPersistentError.clear();
    OfxStatus st = r.render(r.img, 0.5);
    check(ok(st) && gPersistentError.empty(), "render = %d%s%s", st, gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
    int nan = 0;
    for (const float& v : r.image()) {
        nan += isUnwritten(&v) ? 1 : 0;
    }
    check(nan == 0, "image %d,%d,%d,%d: no unwritten pixel (%d NaN)", r.img.x1, r.img.y1, r.img.x2, r.img.y2, nan);
}

// The file is rewritten with other windows while an instance reads it: no render may fail (or assert), and the RoD
// and the image are those of the new file after the next render (which updates the header cache), or after the
// purge caches action.
void
testRewrite(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== file rewritten with other windows ====\n");
    const std::string path = dir + "/rewrite.exr";
    ExrSpec a, b, c;
    a.display = a.data = box(0, 0, 255, 127);
    b.display = box(0, 0, 127, 63);
    b.data = box(-10, -10, 137, 73); // (overscan: a larger RoD)
    c.display = c.data = box(0, 0, 63, 31);
    Reader r;
    if (!writeExr(path, a) || !r.create(p, path) || !ok(r.prepareAndRender())) {
        check(false, "first render");

        return;
    }
    checkRoD(r, { 0, 0, 256, 128 });
    for (int k = 0; k < 2; ++k) {
        const ExrSpec& s = k ? c : b;
        const OfxRectI expected = k ? OfxRectI { 0, 0, 64, 32 } : OfxRectI { -11, -11, 139, 75 };
        std::printf("  rewritten: RoD %d,%d,%d,%d\n", expected.x1, expected.y1, expected.x2, expected.y2);
        if (!writeExr(path, s)) {
            ++gFailures;

            return;
        }
        // the cached RoD is the old one: the image is the new one, within the old RoD
        OfxStatus st = r.prepareAndRender();
        check(ok(st), "render after the rewrite, with the old RoD = %d", st);
        // that render updated the cache
        st = r.prepareAndRender();
        check(ok(st), "next render = %d", st);
        checkRoD(r, expected);
        const int bad = wrongPixels(r, s, r.img);
        check(bad == 0, "the image is the new one (%d wrong)", bad);
    }
    // the purge caches action: the next RoD is the new one
    std::printf("  rewritten, then purge caches\n");
    if (!writeExr(path, a)) {
        ++gFailures;

        return;
    }
    check(ok(r.purgeCaches()), "purge caches");
    OfxStatus st = r.prepareAndRender();
    check(ok(st), "render = %d", st);
    checkRoD(r, { 0, 0, 256, 128 });
    const int bad = wrongPixels(r, a, r.img);
    check(bad == 0, "the image is the new one (%d wrong)", bad);
}

// The file is rewritten with a smaller data window, and the host renders with the RoD it got before (it doesn't
// call getRegionOfDefinition again): after the first render, which updates ReadEXR's header cache, GenericReader
// gets frame bounds that don't contain the render window. The render must succeed (a debug build of GenericReader
// asserted), and write the whole render window: the new image, black out of the new data window. At render scale
// 1, straight into the output image or (with an unpremultiplied file, premultiplied by GenericReader) through
// GenericReader's buffer; and at render scale 1/2 (through the buffer).
void
testRewriteCachedRoD(OfxPlugin* p, const std::string& dir, bool unpremult)
{
    std::printf("==== file rewritten with a smaller data window, the host's RoD is the old one%s ====\n",
                unpremult ? " (unpremultiplied file: GenericReader's buffer)" : "");
    const std::string path = dir + (unpremult ? "/rewrite-cached-unpremult.exr" : "/rewrite-cached.exr");
    ExrSpec a, b;
    a.display = a.data = box(0, 0, 63, 47);
    a.type = Imf::HALF;
    b.display = box(0, 0, 63, 47);
    b.data = box(20, 10, 30, 20);
    b.type = Imf::HALF;
    Reader r;
    if (!writeExr(path, a) || !r.create(p, path)) {
        check(false, "first file");

        return;
    }
    if (unpremult) {
        setOverrideInt(&r.ctx, "filePremult", 2); // eImageUnPreMultiplied
    }
    // the host gets the RoD at scale 1/2 (and so at scale 1), then the file changes
    if (!ok(r.prepare(0.5))) {
        check(false, "getRegionOfDefinition");

        return;
    }
    checkRoD(r, { 0, 0, 64, 48 });
    if (!writeExr(path, b)) {
        ++gFailures;

        return;
    }
    // render at scale 1, the old RoD: this render reads the new header, and updates the cache
    for (int k = 0; k < 2; ++k) {
        r.setImage({ 0, 0, 64, 48 }, 1.);
        gPersistentError.clear();
        OfxStatus st = r.render(r.img);
        check(ok(st) && gPersistentError.empty(), "render %d at scale 1, window 0,0,64,48 = %d%s%s", k + 1, st,
              gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
        // the new image (premultiplied if the file is unpremultiplied), black out of the new data window and its
        // 1-pixel border, in the whole window
        int bad = 0;
        for (int y = 0; y < 48; ++y) {
            for (int x = 0; x < 64; ++x) {
                const int ex = x + b.display.min.x;
                const int ey = b.display.max.y - y;
                const bool inData = ex >= b.data.min.x && ex <= b.data.max.x && ey >= b.data.min.y && ey <= b.data.max.y;
                const float* px = r.pixel(x, y);
                const float alpha = inData ? pixelValue(ex, ey, 3) : 0.f;
                for (int c = 0; c < 4; ++c) {
                    float expected = inData ? pixelValue(ex, ey, c) : 0.f;
                    if (unpremult && c < 3) {
                        expected *= alpha;
                    }
                    if (!(std::fabs(px[c] - expected) <= 1e-6f * (1.f + std::fabs(expected)))) {
                        if (bad < 5) {
                            std::printf("  pixel (%d,%d) channel %d: %.9g, expected %.9g\n", x, y, c, px[c], expected);
                        }
                        ++bad;
                    }
                }
            }
        }
        check(bad == 0, "render %d: the new image, black elsewhere (%d wrong)", k + 1, bad);
    }
    // render at scale 1/2, the old RoD
    r.setImage({ 0, 0, 32, 24 }, 0.5);
    gPersistentError.clear();
    OfxStatus st = r.render(r.img, 0.5);
    check(ok(st) && gPersistentError.empty(), "render at scale 1/2, window 0,0,32,24 = %d%s%s", st,
          gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
    // black out of the new frame bounds at scale 1/2 (the data window and its border, (19,26)-(32,39) at scale 1:
    // (9,13)-(16,20) at scale 1/2), and no unwritten pixel
    int nan = 0, notBlack = 0;
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 32; ++x) {
            const float* px = r.pixel(x, y);
            const bool inBounds = x >= 9 && x < 16 && y >= 13 && y < 20;
            for (int c = 0; c < 4; ++c) {
                nan += isUnwritten(&px[c]) ? 1 : 0;
                notBlack += (!inBounds && px[c] != 0.f) ? 1 : 0;
            }
        }
    }
    check(nan == 0 && notBlack == 0, "no unwritten value (%d NaN), black out of the new frame bounds (%d not black)", nan, notBlack);
}

// A render window narrower than a data window of more lines than a block of decode()'s buffer (256 lines): the
// lines are read in blocks.
void
testWindowBlocks(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== render window narrower than a data window of 600 lines (blocks) ====\n");
    const std::string path = dir + "/window-blocks.exr";
    ExrSpec s;
    s.display = box(0, 0, 39, 599);
    s.data = box(0, 3, 39, 590);
    s.compression = Imf::PIZ_COMPRESSION;
    if (!writeExr(path, s)) {
        ++gFailures;

        return;
    }
    Reader r;
    if (!r.create(p, path) || !ok(r.prepare())) {
        check(false, "getRegionOfDefinition");

        return;
    }
    const OfxRectI window = { r.rod.x1 + 5, r.rod.y1, r.rod.x2 - 3, r.rod.y2 };
    OfxStatus st = r.render(window);
    check(ok(st), "render of %d,%d,%d,%d in the RoD %d,%d,%d,%d = %d", window.x1, window.y1, window.x2, window.y2,
          r.rod.x1, r.rod.y1, r.rod.x2, r.rod.y2, st);
    const int bad = wrongPixels(r, s, window);
    check(bad == 0, "the render window has the expected values, the rest is not written (%d wrong)", bad);
}

// A luminance/chroma file: Y, and RY and BY subsampled 2x2. They are not converted (#123 keeps the behaviour of
// ReadEXR 1.0): Y is R; RY and BY are G and B on the lines that have samples (y even), side by side from the left
// of the data window (the sample at x is at column datawin.min.x + (x - datawin.min.x) / 2), and 0 elsewhere. There
// is no alpha: the image is opaque (GenericReader sets A to 1).
void
testLumaChroma(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== luminance/chroma (Y, RY, BY) ====\n");
    const std::string path = dir + "/lumachroma.exr";
    const Imath::Box2i display = box(0, 0, 47, 31);
    const Imath::Box2i data = box(-4, 2, 41, 29); // (min.x and min.y even, as OpenEXR requires)
    const int w = data.max.x - data.min.x + 1, h = data.max.y - data.min.y + 1;
    try {
        Imf::Header header(display, data);
        header.compression() = Imf::ZIP_COMPRESSION;
        header.channels().insert("Y", Imf::Channel(Imf::FLOAT));
        header.channels().insert("RY", Imf::Channel(Imf::FLOAT, 2, 2));
        header.channels().insert("BY", Imf::Channel(Imf::FLOAT, 2, 2));
        std::vector<float> y((size_t)w * h), ry((size_t)(w / 2) * (h / 2)), by(ry.size());
        for (int j = data.min.y; j <= data.max.y; ++j) {
            for (int i = data.min.x; i <= data.max.x; ++i) {
                y[(size_t)(j - data.min.y) * w + (i - data.min.x)] = pixelValue(i, j, 0);
                if (i % 2 == 0 && j % 2 == 0) {
                    const size_t k = (size_t)(j - data.min.y) / 2 * (w / 2) + (i - data.min.x) / 2;
                    ry[k] = pixelValue(i, j, 1);
                    by[k] = pixelValue(i, j, 2);
                }
            }
        }
        Imf::FrameBuffer fb;
        fb.insert("Y", Imf::Slice(Imf::FLOAT, sliceBase(y.data(), (ptrdiff_t)(data.min.x + (ptrdiff_t)data.min.y * w) * (ptrdiff_t)sizeof(float)), sizeof(float), sizeof(float) * w));
        const ptrdiff_t off = data.min.x / 2 + (ptrdiff_t)(data.min.y / 2) * (w / 2);
        fb.insert("RY", Imf::Slice(Imf::FLOAT, sliceBase(ry.data(), off * (ptrdiff_t)sizeof(float)), sizeof(float), sizeof(float) * (w / 2), 2, 2));
        fb.insert("BY", Imf::Slice(Imf::FLOAT, sliceBase(by.data(), off * (ptrdiff_t)sizeof(float)), sizeof(float), sizeof(float) * (w / 2), 2, 2));
        Imf::OutputFile out(path.c_str(), header);
        out.setFrameBuffer(fb);
        out.writePixels(h);
    } catch (const std::exception& e) {
        check(false, "cannot write %s: %s", path.c_str(), e.what());

        return;
    }
    Reader r;
    if (!r.create(p, path)) {
        ++gFailures;

        return;
    }
    OfxStatus st = r.prepareAndRender();
    check(ok(st), "render = %d", st);
    if (!ok(st)) {
        return;
    }
    int bad = 0;
    for (int oy = r.rod.y1; oy < r.rod.y2; ++oy) {
        for (int ox = r.rod.x1; ox < r.rod.x2; ++ox) {
            const int ex = ox + display.min.x;
            const int ey = display.max.y - oy;
            const bool inData = ex >= data.min.x && ex <= data.max.x && ey >= data.min.y && ey <= data.max.y;
            // the chroma sample written at column ex: the one at x = data.min.x + 2 * (ex - data.min.x)
            const int sx = data.min.x + 2 * (ex - data.min.x);
            const bool hasChroma = inData && ey % 2 == 0 && sx <= data.max.x;
            const float expected[4] = { inData ? pixelValue(ex, ey, 0) : 0.f, hasChroma ? pixelValue(sx, ey, 1) : 0.f,
                                        hasChroma ? pixelValue(sx, ey, 2) : 0.f, 1.f };
            const float* px = r.pixel(ox, oy);
            for (int c = 0; c < 4; ++c) {
                if (px[c] != expected[c]) {
                    if (bad < 5) {
                        std::printf("  pixel (%d,%d) channel %d: %.9g, expected %.9g\n", ox, oy, c, px[c], expected[c]);
                    }
                    ++bad;
                }
            }
        }
    }
    check(bad == 0, "every pixel is the expected value (%d wrong)", bad);
}

// A truncated file: getRegionOfDefinition may succeed (the header is complete), but the render must fail with an
// error message.
void
testTruncated(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== truncated file ====\n");
    const std::string path = dir + "/truncated.exr";
    ExrSpec s;
    s.display = s.data = box(0, 0, 255, 127);
    s.compression = Imf::ZIP_COMPRESSION;
    s.bench = true; // (noise: the file is not tiny)
    if (!writeExr(path, s)) {
        ++gFailures;

        return;
    }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    std::vector<char> data;
    if (f) {
        std::fseek(f, 0, SEEK_END);
        data.resize((size_t)std::ftell(f));
        std::fseek(f, 0, SEEK_SET);
        data.resize(std::fread(data.data(), 1, data.size(), f));
        std::fclose(f);
    }
    f = std::fopen(path.c_str(), "wb");
    if (!f || data.size() < 1000) {
        check(false, "cannot truncate %s", path.c_str());
        if (f) {
            std::fclose(f);
        }

        return;
    }
    std::fwrite(data.data(), 1, data.size() / 2, f);
    std::fclose(f);
    std::printf("  %s: %zu bytes, truncated to %zu\n", path.c_str(), data.size(), data.size() / 2);

    Reader r;
    if (!r.create(p, path)) {
        ++gFailures;

        return;
    }
    gPersistentError.clear();
    OfxStatus st = r.prepare();
    if (ok(st)) {
        st = r.render();
    }
    check(!ok(st), "render = %d (must fail)", st);
    check(!gPersistentError.empty(), "error message: %s", gPersistentError.c_str());
}

// Several instances render two files (the same file in more than one instance) from several threads at once:
// each render must give the same image as a single-threaded one.
void
testThreads(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== concurrent renders ====\n");
    const int kFiles = 2, kInstancesPerFile = 3, kRenders = 8;
    std::vector<std::string> paths;
    std::vector<std::vector<float>> refs;
    for (int k = 0; k < kFiles; ++k) {
        ExrSpec s;
        s.display = box(0, 0, 319, 239);
        s.data = box(-4 * k, 3, 319 + 4 * k, 230);
        s.compression = k ? Imf::PIZ_COMPRESSION : Imf::ZIP_COMPRESSION;
        s.type = Imf::HALF;
        s.bench = true;
        paths.push_back(dir + "/threads" + std::to_string(k) + ".exr");
        if (!writeExr(paths.back(), s)) {
            ++gFailures;

            return;
        }
        Reader r;
        if (!r.create(p, paths.back()) || !ok(r.prepareAndRender())) {
            check(false, "single-threaded render of %s", paths.back().c_str());

            return;
        }
        refs.push_back(r.image());
    }
    std::vector<Reader> readers(kFiles * kInstancesPerFile);
    for (size_t i = 0; i < readers.size(); ++i) {
        if (!readers[i].create(p, paths[i % kFiles]) || !ok(readers[i].prepare())) {
            check(false, "instance %zu", i);

            return;
        }
    }
    std::atomic<int> failures(0), mismatches(0), started(0);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < readers.size(); ++i) {
        threads.emplace_back([&, i]() {
            // start together
            ++started;
            while (started < (int)readers.size()) {
                std::this_thread::yield();
            }
            for (int n = 0; n < kRenders; ++n) {
                Reader& r = readers[i];
                fillUnwritten((float*)r.out->buffer.data(), r.out->buffer.size() / sizeof(float));
                if (!ok(r.render())) {
                    ++failures;
                } else if (!sameImage(r.image(), refs[i % kFiles])) {
                    ++mismatches;
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    check(failures == 0, "%d threads x %d renders: %d failed", (int)readers.size(), kRenders, (int)failures);
    check(mismatches == 0, "%d threads x %d renders: %d differ from the single-threaded render", (int)readers.size(), kRenders, (int)mismatches);
}

// Times the renders of 2048x1556 RGBA half files.
int
bench(OfxPlugin* p, const std::string& dir, int n)
{
    if (const char* t = std::getenv("EXR_THREADS")) {
        Imf::setGlobalThreadCount(std::atoi(t));
    }
    std::printf("OpenEXR global thread count: %d\n", Imf::globalThreadCount());
    const struct {
        const char* name;
        Imf::Compression c;
    } comps[] = { { "ZIP", Imf::ZIP_COMPRESSION }, { "PIZ", Imf::PIZ_COMPRESSION }, { "DWAA", Imf::DWAA_COMPRESSION } };
    for (const auto& c : comps) {
        ExrSpec s;
        s.display = s.data = box(0, 0, 2047, 1555);
        s.compression = c.c;
        s.type = Imf::HALF;
        s.bench = true;
        const std::string path = dir + "/bench-" + c.name + ".exr";
        if (!writeExr(path, s)) {
            return 1;
        }
        Reader r;
        if (!r.create(p, path) || !ok(r.prepareAndRender())) {
            std::printf("%s: render failed\n", c.name);

            return 1;
        }
        double best = 1e30, total = 0.;
        for (int k = 0; k < n; ++k) {
            const auto t0 = std::chrono::steady_clock::now();
            const OfxStatus st = r.render();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (!ok(st)) {
                std::printf("%s: render failed\n", c.name);

                return 1;
            }
            best = std::min(best, ms);
            total += ms;
        }
        std::printf("%-5s 2048x1556 RGBA half: min %.1f ms, mean %.1f ms (%d renders)\n", c.name, best, total / n, n);
    }

    return 0;
}

} // namespace

int
main(int argc, char** argv)
{
    const bool isBench = argc >= 3 && !std::strcmp(argv[1], "--bench");
    if (argc < 2 || (!isBench && argc != 2)) {
        std::printf("usage: %s <scratch dir> | --bench <scratch dir> [n]\n", argv[0]);

        return 2;
    }
    const std::string dir = isBench ? argv[2] : argv[1];
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // (the output before a crash)
    initHost();
    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadEXR");
    if (!p) {
        std::printf("FAIL: ReadEXR not found\n");

        return 1;
    }
    Effect desc;
    p->setHost(&gOfxHost);
    if (!ok(p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr)) || !ok(p->mainEntry(kOfxActionDescribe, H(desc), nullptr, nullptr))) {
        std::printf("FAIL: ReadEXR load/describe\n");

        return 1;
    }
    const char* safety = "";
    pGetString(PH(&desc.props), kOfxImageEffectPluginRenderThreadSafety, 0, &safety);
    if (isBench) {
        const int rc = bench(p, dir, argc > 3 ? std::atoi(argv[3]) : 5);
        p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

        return rc;
    }
    std::printf("==== describe ====\n");
    check(!std::strcmp(safety, kOfxImageEffectRenderFullySafe), "render thread safety: %s", safety);

    // the windows of tests/hostdemo/run.sh (#115), and more channel types
    ExrSpec s;
    s.display = s.data = box(0, 0, 99, 59);
    testPixels(p, dir, "default", s, { 0, 0, 100, 60 });
    s.display = s.data = box(5, 7, 104, 66);
    s.compression = Imf::PIZ_COMPRESSION;
    testPixels(p, dir, "offset", s, { 0, 0, 100, 60 });
    s.display = box(0, 0, 719, 575);
    s.data = box(10, 20, 109, 69);
    s.compression = Imf::NO_COMPRESSION;
    testPixels(p, dir, "crop", s, { 9, 505, 111, 557 });
    s.display = box(0, 0, 99, 59);
    s.data = box(-10, -10, 109, 69);
    s.compression = Imf::ZIP_COMPRESSION;
    testPixels(p, dir, "overscan", s, { -11, -11, 111, 71 });
    s.display = box(5, 7, 104, 66);
    s.data = box(20, 17, 59, 46);
    s.type = Imf::HALF;
    testPixels(p, dir, "offsetcrop-half", s, { 14, 19, 56, 51 });
    // missing channels are 0
    s = ExrSpec();
    s.display = s.data = box(0, 0, 63, 31);
    s.channels = "RA";
    testPixels(p, dir, "missing-channels", s, { 0, 0, 64, 32 });
    // UINT, normalized
    s.channels = "RGBA";
    s.uintRed = true;
    testPixels(p, dir, "uint", s, { 0, 0, 64, 32 });

    testLumaChroma(p, dir);
    testWindow(p, dir);
    testScale(p, dir);
    testRewrite(p, dir);
    testRewriteCachedRoD(p, dir, false);
    testRewriteCachedRoD(p, dir, true);
    testWindowBlocks(p, dir);
    testTruncated(p, dir);
    testThreads(p, dir);

    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::printf("%s (%d failure%s)\n", gFailures ? "FAIL" : "PASS", gFailures, gFailures == 1 ? "" : "s");

    return gFailures ? 1 : 0;
}
