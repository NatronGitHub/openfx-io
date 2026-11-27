/*
 * exr_read_test — ReadEXR (EXR/ReadEXR.cpp, issue #123), driven by the in-process host (ofx_test_host.h):
 *
 *  - pixels: EXR files written here with OpenEXR (FLOAT, ZIP, PIZ and no compression), with various data and
 *    display windows, read back exactly: the display window is moved to (0,0), y is flipped, the 1-pixel border
 *    around the data window (when the windows differ) is black, and the channels that are not in the file are 0
 *  - UINT channels are normalized by 2^32-1, as in ReadOIIO
 *  - luminance/chroma files (Y, RY, BY): read as before #123 (not converted)
 *  - render scales 0.75 and 1/2: the image, scaled (the area-weighted mean of the pixels at scale 1, #194)
 *  - a render window narrower than the data window (in blocks of lines), render scale 1/2, and a file rewritten
 *    with other windows (the header cache is updated by the next render, or emptied by the purge caches action),
 *    including renders with the RoD the host got before the rewrite (GenericReader then fills with black the part
 *    of the render window out of the new frame bounds), and a file reverted to the header that the host got the
 *    RoD from, while the cache has another one (the header is read again, the render is not cropped)
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

#include "ExrThreads.h"
#include "OIIOGlobal.h"
#include "ofxsMultiPlane.h"

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>
#include <ImfStandardAttributes.h> // Imf::addMultiView()
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
    std::vector<std::string> extra; // more channels, constant 7, that ReadEXR must ignore
    std::string prefix;             // the R, G, B and A channels are named prefix + "R" + suffix... ("diffuse.R")
    std::string suffix;
};

uint32_t
uintValue(int x, int y)
{
    return (uint32_t)(x * 100000 + y * 1000000) + 123u;
}

// Writes the EXR file s to path: false (a failed check) if it can't
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
            const std::string name = s.prefix + s.channels[k] + s.suffix;
            if (s.uintRed && s.channels[k] == 'R') {
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
        const std::vector<float> seven(s.extra.empty() ? 0 : (size_t)w * h, 7.f);
        for (const std::string& name : s.extra) {
            header.channels().insert(name, Imf::Channel(Imf::FLOAT));
            fb.insert(name, Imf::Slice(Imf::FLOAT, sliceBase(seven.data(), (ptrdiff_t)(s.data.min.x + (ptrdiff_t)s.data.min.y * w) * (ptrdiff_t)sizeof(float)), sizeof(float), sizeof(float) * w));
        }
        Imf::OutputFile out(path.c_str(), header);
        out.setFrameBuffer(fb);
        out.writePixels(h);
    } catch (const std::exception& e) {
        check(false, "cannot write %s: %s", path.c_str(), e.what());

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
        if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, EH(ctx), PH(&inArgs), nullptr))) {
            std::printf("  describeInContext failed\n");

            return false;
        }
        setOverrideString(&ctx, kOfxImageEffectFileParamName, filename);
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
        out->connected = true;
        setupClipImage(out, 1, 1, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        PropSet o;
        if (!ok(p->mainEntry(kOfxImageEffectActionGetClipPreferences, EH(inst), nullptr, PH(&o)))) {
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
        OfxStatus st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, EH(inst), PH(&a), PH(&o));
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

        return p->mainEntry(kOfxImageEffectActionRender, EH(inst), PH(&ra), nullptr);
    }

    // the purge caches action
    OfxStatus purgeCaches() { return p->mainEntry(kOfxActionPurgeCaches, EH(inst), nullptr, nullptr); }

    // the planes of the output clip (the getClipComponents action of a multi-planar host), none if it fails
    std::vector<std::string> planes(int view = 0)
    {
        PropSet a;
        pSetDouble(PH(&a), kOfxPropTime, 0, 1.);
        pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, view);
        PropSet o;
        std::vector<std::string> r;
        if (p->mainEntry(kFnOfxImageEffectActionGetClipComponents, EH(inst), PH(&a), PH(&o)) != kOfxStatOK) {
            return r;
        }
        const std::string name = std::string(kFnOfxImageEffectActionGetClipComponentsPropString) + kOfxImageEffectOutputClipName;
        int n = 0;
        pGetDimension(PH(&o), name.c_str(), &n);
        for (int i = 0; i < n; ++i) {
            const char* plane = "";
            pGetString(PH(&o), name.c_str(), i, &plane);
            r.push_back(plane);
        }

        return r;
    }

    // the user selects another file (instanceChanged, on this thread: the main thread)
    OfxStatus changeFile(const std::string& filename)
    {
        {
            HostWrite lock;
            ctx.params.at(kOfxImageEffectFileParamName).ov.s = { filename };
        }
        return instanceChanged(p, inst, kOfxImageEffectFileParamName);
    }

    // render a plane (kFnOfxImagePlaneColour, or the string of a plane) of a view into an image of nComps
    // components (comps: RGBA, or the string of the plane) that covers the RoD (prepare() first), filled with NaN
    OfxStatus renderPlane(const std::string& plane, const std::string& comps, int nComps, int view)
    {
        const int w = img.x2 - img.x1;
        const int h = img.y2 - img.y1;
        const int rowBytes = w * nComps * (int)sizeof(float);
        out->buffer.assign((size_t)rowBytes * h, 0);
        fillUnwritten((float*)out->buffer.data(), (size_t)w * h * nComps);
        OfxPropertySetHandle ip = PH(&out->image);
        pSetPointer(ip, kOfxImagePropData, 0, out->buffer.data());
        pSetInt(ip, kOfxImagePropRowBytes, 0, rowBytes);
        pSetString(ip, kOfxImageEffectPropComponents, 0, comps.c_str());
        PropSet ra = makeRenderInArgs(1., 0, 0);
        int win[4] = { img.x1, img.y1, img.x2, img.y2 };
        pSetIntN(PH(&ra), kOfxImageEffectPropRenderWindow, 4, win);
        pSetString(PH(&ra), kOfxImageEffectPropRenderPlanes, 0, plane.c_str());
        pSetInt(PH(&ra), kFnOfxImageEffectPropView, 0, view);
        const OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, EH(inst), PH(&ra), nullptr);
        pSetString(ip, kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);

        return st;
    }

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
            p->mainEntry(kOfxActionDestroyInstance, EH(inst), nullptr, nullptr);
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
// window and in the channels that are not in the file, but A is 1 if the file has no A (GenericReader makes an RGB
// image opaque). Out of the window: NaN (not written).
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
                float expected = (c == 3 && s.channels.find('A') == std::string::npos) ? 1.f : 0.f;
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

// Render scales 0.75 (not a power of two) and 1/2 (#194): each pixel at render scale s covers [x / s, (x + 1) / s) x
// [y / s, (y + 1) / s) at scale 1 (ofxCoordSystem.rst, "Mapping Between The Spatial Coordinate Systems"), and must be
// the mean of the pixels of the render at scale 1 that it covers, weighted by the covered area (areaMean()). An RGB
// file (Opaque, so that the mean is not premultiplied afterwards) whose data window is its display window: the frame
// bounds are the RoD. GenericReader used to read the nearest power-of-two level (#428): at 0.75, a crop of the image. The tolerance,
// 1e-6 relative, is a few float ulps (the reader sums in double and stores floats; the values are below 7).
void
testScales(OfxPlugin* p, const std::string& dir)
{
    const std::string path = dir + "/scales.exr";
    ExrSpec s;
    s.display = s.data = box(0, 0, 99, 59);
    s.channels = "RGB";
    Reader r1;
    if (!writeExr(path, s) || !r1.create(p, path) || !ok(r1.prepareAndRender())) {
        check(false, "render at scale 1");

        return;
    }
    for (double scale : { 0.75, 0.5 }) {
        std::printf("==== render scale %g: the image, scaled ====\n", scale);
        Reader r;
        if (!r.create(p, path) || !ok(r.prepare(scale))) {
            check(false, "getRegionOfDefinition");

            continue;
        }
        gPersistentError.clear();
        const OfxStatus st = r.render(r.img, scale);
        check(ok(st) && gPersistentError.empty(), "render = %d%s%s", st, gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
        int bad = 0;
        const OfxPointD renderScale = { scale, scale };
        for (int y = r.img.y1; y < r.img.y2; ++y) {
            for (int x = r.img.x1; x < r.img.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const double expected = areaMean(r1.img, renderScale, x, y, [&](int i, int j) { return (double)r1.pixel(i, j)[c]; });
                    const float got = r.pixel(x, y)[c];
                    if (!(std::fabs(got - expected) <= 1e-6 * (1. + std::fabs(expected)))) { // (NaN: unwritten)
                        if (bad < 5) {
                            std::printf("  pixel (%d,%d) channel %d: %.9g, expected %.9g\n", x, y, c, got, expected);
                        }
                        ++bad;
                    }
                }
            }
        }
        check(bad == 0, "image %d,%d,%d,%d: every pixel is the area-weighted mean of the pixels at scale 1 (%d wrong)", r.img.x1, r.img.y1, r.img.x2,
              r.img.y2, bad);
    }
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

// The file is rewritten with a smaller data window, rendered (which updates the header cache), then reverted to the
// first header, which the host still has the RoD of (#163): GenericReader asks ReadEXR to read the header again
// (refreshFrameBounds()) when the render window is out of the cached frame bounds, and the render is the first image,
// not cropped to the cached data window.
void
testRevert(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== file rewritten, rendered, then reverted: the host's RoD is the first one ====\n");
    const std::string path = dir + "/revert.exr";
    ExrSpec a, b;
    a.display = a.data = box(0, 0, 63, 47);
    b.display = box(0, 0, 63, 47);
    b.data = box(20, 10, 30, 20);
    Reader r;
    if (!writeExr(path, a) || !r.create(p, path) || !ok(r.prepare())) {
        check(false, "first file");

        return;
    }
    checkRoD(r, { 0, 0, 64, 48 });
    const OfxRectI rod = r.img;
    // the file changes: this render reads the new header, and updates the cache
    if (!writeExr(path, b)) {
        return;
    }
    r.setImage(rod, 1.);
    gPersistentError.clear();
    OfxStatus st = r.render(rod);
    check(ok(st) && gPersistentError.empty(), "render of the second file = %d%s%s", st, gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
    check(wrongPixels(r, b, rod) == 0, "the second file's pixels");
    // the file reverts to the first header (the host keeps the first RoD): the first image, not cropped
    if (!writeExr(path, a)) {
        return;
    }
    for (int k = 0; k < 2; ++k) {
        r.setImage(rod, 1.);
        gPersistentError.clear();
        st = r.render(rod);
        check(ok(st) && gPersistentError.empty(), "render %d of the reverted file = %d%s%s", k + 1, st, gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
        const int bad = wrongPixels(r, a, rod);
        check(bad == 0, "render %d: the first file's pixels (%d wrong)", k + 1, bad);
    }
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
testLumaChroma(OfxPlugin* p, const std::string& dir, const std::string& prefix)
{
    std::printf("==== luminance/chroma (%sY, %sRY, %sBY) ====\n", prefix.c_str(), prefix.c_str(), prefix.c_str());
    const std::string path = dir + "/lumachroma" + (prefix.empty() ? "" : "-layer") + ".exr";
    const std::string yName = prefix + "Y";
    const std::string ryName = prefix + "RY";
    const std::string byName = prefix + "BY";
    const Imath::Box2i display = box(0, 0, 47, 31);
    const Imath::Box2i data = box(-4, 2, 41, 29); // (min.x and min.y even, as OpenEXR requires)
    const int w = data.max.x - data.min.x + 1, h = data.max.y - data.min.y + 1;
    try {
        Imf::Header header(display, data);
        header.compression() = Imf::ZIP_COMPRESSION;
        header.channels().insert(yName, Imf::Channel(Imf::FLOAT));
        header.channels().insert(ryName, Imf::Channel(Imf::FLOAT, 2, 2));
        header.channels().insert(byName, Imf::Channel(Imf::FLOAT, 2, 2));
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
        fb.insert(yName, Imf::Slice(Imf::FLOAT, sliceBase(y.data(), (ptrdiff_t)(data.min.x + (ptrdiff_t)data.min.y * w) * (ptrdiff_t)sizeof(float)), sizeof(float), sizeof(float) * w));
        const ptrdiff_t off = data.min.x / 2 + (ptrdiff_t)(data.min.y / 2) * (w / 2);
        fb.insert(ryName, Imf::Slice(Imf::FLOAT, sliceBase(ry.data(), off * (ptrdiff_t)sizeof(float)), sizeof(float), sizeof(float) * (w / 2), 2, 2));
        fb.insert(byName, Imf::Slice(Imf::FLOAT, sliceBase(by.data(), off * (ptrdiff_t)sizeof(float)), sizeof(float), sizeof(float) * (w / 2), 2, 2));
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

// ------------------------------------------------------------------ layers and views (#386)

// The value of the k-th channel written by writeChannels() at EXR pixel (x, y): exact in FLOAT, distinct for each
// channel
float
channelValue(int x, int y, int k)
{
    return pixelValue(x, y, 0) + 8.f * k;
}

const int kLayersW = 24, kLayersH = 16;

// A kLayersW x kLayersH FLOAT file with these channels (the k-th one is channelValue(x, y, k)), and the multiView
// attribute if views is not empty
bool
writeChannels(const std::string& path, const std::vector<std::string>& channels, const std::vector<std::string>& views)
{
    try {
        Imf::Header header(kLayersW, kLayersH);
        if (!views.empty()) {
            Imf::addMultiView(header, views);
        }
        std::vector<std::vector<float>> data(channels.size(), std::vector<float>((size_t)kLayersW * kLayersH));
        Imf::FrameBuffer fb;
        for (size_t k = 0; k < channels.size(); ++k) {
            for (int y = 0; y < kLayersH; ++y) {
                for (int x = 0; x < kLayersW; ++x) {
                    data[k][(size_t)y * kLayersW + x] = channelValue(x, y, (int)k);
                }
            }
            header.channels().insert(channels[k], Imf::Channel(Imf::FLOAT));
            fb.insert(channels[k], Imf::Slice(Imf::FLOAT, (char*)data[k].data(), sizeof(float), sizeof(float) * kLayersW));
        }
        Imf::OutputFile out(path.c_str(), header);
        out.setFrameBuffer(fb);
        out.writePixels(kLayersH);
    } catch (const std::exception& e) {
        std::printf("  cannot write %s: %s\n", path.c_str(), e.what());

        return false;
    }

    return true;
}

// The index of a channel in the list (-1 if absent)
int
channelIndex(const std::vector<std::string>& channels, const std::string& name)
{
    std::vector<std::string>::const_iterator it = std::find(channels.begin(), channels.end(), name);

    return it == channels.end() ? -1 : (int)(it - channels.begin());
}

// The number of wrong values in the render of a plane (renderPlane()) of a file written by writeChannels(): each
// component c is the channel expected[c] of the file, or 1 if it is "1", or 0 if it is "0"
int
wrongPlaneValues(const Reader& r, const std::vector<std::string>& channels, const std::vector<std::string>& expected)
{
    const int nComps = (int)expected.size();
    const float* d = reinterpret_cast<const float*>(r.out->buffer.data());
    int bad = 0;
    for (int y = r.img.y1; y < r.img.y2; ++y) {
        for (int x = r.img.x1; x < r.img.x2; ++x) {
            const float* px = d + ((size_t)(y - r.img.y1) * (r.img.x2 - r.img.x1) + (x - r.img.x1)) * nComps;
            for (int c = 0; c < nComps; ++c) {
                const int k = channelIndex(channels, expected[c]);
                const float v = (expected[c] == "1") ? 1.f : (expected[c] == "0") ? 0.f
                                                                                  : (k < 0 ? -1.f : channelValue(x, kLayersH - 1 - y, k));
                if (isUnwritten(&px[c]) || px[c] != v) {
                    if (bad < 5) {
                        std::printf("  pixel (%d,%d) component %d: %.9g, expected %.9g (%s)\n", x, y, c, px[c], v, expected[c].c_str());
                    }
                    ++bad;
                }
            }
        }
    }

    return bad;
}

// The string of a plane (as the plug-ins name it, see GenericReaderPlugin::setClipPlanes())
std::string
planeString(const std::string& layer, const std::vector<std::string>& channels)
{
    return OFX::MultiPlane::ImagePlaneDesc::mapPlaneToOFXPlaneString(OFX::MultiPlane::ImagePlaneDesc(layer, "", "", channels));
}

std::string
joined(const std::vector<std::string>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        s += (i ? " " : "") + v[i];
    }

    return s.empty() ? "(none)" : s;
}

// the views of the project (see ofx_test_host.h)
void
setViews(const std::vector<std::string>& names)
{
    gViewNames = names;
    gViewCount = (int)names.size();
}

struct PlaneRender {
    int view;
    std::string layer; // "" for the color plane
    std::vector<std::string> channels; // the channels of the plane
    std::vector<std::string> expected; // the channels of the file read in each component ("1": 1, "0": 0)
};

// A multi-planar host (OFX_EXTENSIONS_NUKE plane suite, as Natron): ReadEXR's planes (getClipComponents) are the
// layers of the file, as ReadOIIO's (for the views 0 and 1), and each plane of each view reads the right channels.
void
testPlanes(OfxPlugin* p, OfxPlugin* oiio, const std::string& dir, const char* name, const std::vector<std::string>& channels,
           const std::vector<std::string>& views, const std::vector<std::string>& expectedLayers, const std::vector<PlaneRender>& renders)
{
    std::printf("==== planes and views: %s ====\n", name);
    const std::string path = dir + "/" + name + ".exr";
    if (!writeChannels(path, channels, views)) {
        ++gFailures;

        return;
    }
    Reader r;
    if (!r.create(p, path)) {
        ++gFailures;

        return;
    }
    const std::vector<std::string> planes = r.planes();
    std::vector<std::string> expectedPlanes = expectedLayers;
    expectedPlanes.push_back(kFnOfxImagePlaneColour); // (last)
    check(planes == expectedPlanes, "planes: %s (expected %s)", joined(planes).c_str(), joined(expectedPlanes).c_str());
    if (oiio) {
        Reader ro;
        if (!ro.create(oiio, path)) {
            ++gFailures;
        } else {
            for (int view = 0; view < 2; ++view) {
                const std::vector<std::string> oiioPlanes = ro.planes(view);
                check(r.planes(view) == oiioPlanes, "view %d: the same planes as ReadOIIO (%s)", view, joined(oiioPlanes).c_str());
            }
        }
    }
    const OfxStatus st = r.prepare();
    check(ok(st), "getRegionOfDefinition = %d", st);
    if (!ok(st)) {
        return;
    }
    for (const PlaneRender& pr : renders) {
        const std::string plane = pr.layer.empty() ? std::string(kFnOfxImagePlaneColour) : planeString(pr.layer, pr.channels);
        const std::string comps = pr.layer.empty() ? std::string(kOfxImageComponentRGBA) : plane;
        gPersistentError.clear();
        const OfxStatus rs = r.renderPlane(plane, comps, (int)pr.expected.size(), pr.view);
        const int bad = ok(rs) ? wrongPlaneValues(r, channels, pr.expected) : -1;
        check(ok(rs) && bad == 0 && gPersistentError.empty(), "view %d (%s), plane %s: %s (render = %d, %d wrong values%s%s)", pr.view,
              pr.view < (int)gViewNames.size() ? gViewNames[pr.view].c_str() : "?", pr.layer.empty() ? "color" : pr.layer.c_str(),
              joined(pr.expected).c_str(), rs, bad, gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
    }
}

// The layers and views in a multi-planar host (#386)
void
testLayersViews(OfxPlugin* p, OfxPlugin* oiio, const std::string& dir)
{
    const std::vector<std::string> rgba = { "R", "G", "B", "A" }, rgb = { "R", "G", "B" }, z = { "Z" };
    // layers only: diffuse.RGBA is the color plane (#384: one layer, the one with the most of R, G, B and A)
    {
        const std::vector<std::string> ch = { "diffuse.R", "diffuse.G", "diffuse.B", "diffuse.A", "specular.R", "specular.G", "specular.B", "depth.Z" };
        testPlanes(p, oiio, dir, "layers", ch, { }, { planeString("depth", z), planeString("diffuse", rgba), planeString("specular", rgb) },
                   { { 0, "", rgba, { "diffuse.R", "diffuse.G", "diffuse.B", "diffuse.A" } },
                     { 0, "diffuse", rgba, { "diffuse.R", "diffuse.G", "diffuse.B", "diffuse.A" } },
                     { 0, "specular", rgb, { "specular.R", "specular.G", "specular.B" } },
                     { 0, "depth", z, { "depth.Z" } },
                     // a layer without alpha, in 4 components: alpha 1 (as ReadOIIO)
                     { 0, "specular", rgba, { "specular.R", "specular.G", "specular.B", "1" } } });
    }
    // plain R, G, B, A (the color plane) and layers
    {
        const std::vector<std::string> ch = { "R", "G", "B", "A", "specular.R", "specular.G", "specular.B", "Z" };
        testPlanes(p, oiio, dir, "color-layers", ch, { }, { planeString("depth", z), planeString("specular", rgb) },
                   { { 0, "", rgba, { "R", "G", "B", "A" } },
                     { 0, "specular", rgb, { "specular.R", "specular.G", "specular.B" } },
                     { 0, "depth", z, { "Z" } } });
    }
    // luminance/chroma: Y is the color plane, RY and BY are layers (as with OpenImageIO's OpenEXR core reader); the
    // color plane is Y, RY, BY (not converted, #123), with alpha 1 (no alpha)
    testPlanes(p, oiio, dir, "luma-chroma-planes", { "Y", "RY", "BY" }, { }, { planeString("BY", { "BY" }), planeString("RY", { "RY" }) },
               { { 0, "", rgba, { "Y", "RY", "BY", "1" } }, { 0, "RY", { "RY" }, { "RY" } }, { 0, "BY", { "BY" }, { "BY" } } });
    // a layer whose channels sort apart (P.X, P.Xa.R, P.Y, P.Z): P is one layer, X, Y, Z (OpenImageIO sorts by layer)
    testPlanes(p, oiio, dir, "split-layer", { "R", "G", "B", "P.X", "P.Xa.R", "P.Y", "P.Z" }, { },
               { planeString("P", { "X", "Y", "Z" }), planeString("P.Xa", { "R" }) }, { { 0, "P", { "X", "Y", "Z" }, { "P.X", "P.Y", "P.Z" } } });
    // views: left (the default view, unprefixed) and right, as written by OpenEXR (layer.view.channel) and by Nuke
    // (view.layer.channel); spec is in the left view only: black in the right one
    setViews({ "left", "right" });
    {
        const std::vector<std::string> ch = { "R", "G", "B", "A", "depth.Z", "right.R", "right.G", "right.B", "right.A", "depth.right.Z", "spec.R" };
        const std::vector<std::string> layers = { planeString("depth", z), planeString("spec", { "R" }) };
        const std::vector<PlaneRender> renders = { { 0, "", rgba, { "R", "G", "B", "A" } },
                                                   { 1, "", rgba, { "right.R", "right.G", "right.B", "right.A" } },
                                                   { 0, "depth", z, { "depth.Z" } },
                                                   { 1, "depth", z, { "depth.right.Z" } },
                                                   { 0, "spec", { "R" }, { "spec.R" } },
                                                   { 1, "spec", { "R" }, { "0" } } };
        testPlanes(p, oiio, dir, "views", ch, { "left", "right" }, layers, renders);
        const std::vector<std::string> nuke = { "R", "G", "B", "A", "depth.Z", "right.R", "right.G", "right.B", "right.A", "right.depth.Z", "spec.R" };
        std::vector<PlaneRender> nukeRenders = renders;
        nukeRenders[3].expected = { "right.depth.Z" };
        testPlanes(p, oiio, dir, "views-nuke", nuke, { "left", "right" }, layers, nukeRenders);
        // the views are matched by name: the host's view 0 is right
        setViews({ "right", "left" });
        testPlanes(p, oiio, dir, "views-by-name", ch, { "left", "right" }, layers,
                   { { 0, "", rgba, { "right.R", "right.G", "right.B", "right.A" } },
                     { 1, "", rgba, { "R", "G", "B", "A" } },
                     { 0, "depth", z, { "depth.right.Z" } } });
        setViews({ "left", "right" });
        // the view is not part of the layer: in the right view (right.R, right.G, right.B, without right.A, and Nuke's
        // right.diffuse.RGBA), the color plane is the plain right.R, G, B (#384), with alpha 1 (none)
        const std::vector<std::string> noRightA = { "R", "G", "B", "A", "diffuse.R", "diffuse.G", "diffuse.B", "diffuse.A", "right.R", "right.G", "right.B",
                                                    "right.diffuse.R", "right.diffuse.G", "right.diffuse.B", "right.diffuse.A" };
        testPlanes(p, oiio, dir, "views-no-right-alpha", noRightA, { "left", "right" }, { planeString("diffuse", rgba) },
                   { { 0, "", rgba, { "R", "G", "B", "A" } },
                     { 1, "", rgba, { "right.R", "right.G", "right.B", "1" } },
                     { 1, "diffuse", rgba, { "right.diffuse.R", "right.diffuse.G", "right.diffuse.B", "right.diffuse.A" } } });
    }
    // a file without views: every view reads the same channels ("right." is a layer)
    {
        const std::vector<std::string> ch = { "R", "G", "B", "A", "right.R", "right.G", "right.B", "right.A" };
        testPlanes(p, oiio, dir, "no-views", ch, { }, { planeString("right", rgba) },
                   { { 0, "", rgba, { "R", "G", "B", "A" } },
                     { 1, "", rgba, { "R", "G", "B", "A" } },
                     { 1, "right", rgba, { "right.R", "right.G", "right.B", "right.A" } } });
    }
    // (the host's default views)
    gViewNames = { "main", "right" };
    gViewCount = 1;
}

// getClipComponents and renders on other threads while the main thread switches files (instanceChanged reads the
// layers of the new file): getClipComponents returns the planes of one of the files, and nothing else, and a render
// that succeeds is the color plane of one of them (the same values: diffuse.RGBA and R, G, B, A are their first 4
// channels). The data races are caught only under ThreadSanitizer (make test-exr-tsan).
void
testPlanesWhileFileChanges(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== planes and renders while the file changes ====\n");
    const std::string files[2] = { dir + "/layers.exr", dir + "/views.exr" }; // (written by testLayersViews())
    Reader r;
    if (!r.create(p, files[0]) || !ok(r.prepare())) {
        ++gFailures;

        return;
    }
    // a proxy threshold of 1/2: at scale 1, GenericReader::render() doesn't read the file param a second time (for a
    // proxy file), and doesn't assert that it is still the same (another race with a file change, #373)
    {
        ParamRec& threshold = r.ctx.params.at("proxyThreshold");
        threshold.hasOv = true;
        threshold.ov.d = { 0.5, 0.5 };
    }
    const std::vector<std::string> planes0 = r.planes();
    if (!ok(r.changeFile(files[1]))) {
        ++gFailures;

        return;
    }
    const std::vector<std::string> planes1 = r.planes();
    check(planes0 != planes1 && !planes0.empty() && !planes1.empty(), "the files have other planes (%zu and %zu)", planes0.size(), planes1.size());
    std::atomic<int> running(2), wrongPlanes(0), calls(0), okRenders(0), wrongRenders(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 200; ++i) {
                if (t == 0) {
                    const std::vector<std::string> planes = r.planes();
                    if (planes != planes0 && planes != planes1) {
                        ++wrongPlanes;
                    }
                } else {
                    PropSet ra = makeRenderInArgs(1., 0, 0);
                    int win[4] = { r.img.x1, r.img.y1, r.img.x2, r.img.y2 };
                    pSetIntN(PH(&ra), kOfxImageEffectPropRenderWindow, 4, win);
                    // (may fail: the file changed)
                    if (ok(p->mainEntry(kOfxImageEffectActionRender, EH(r.inst), PH(&ra), nullptr))) {
                        ++okRenders;
                        if (wrongPlaneValues(r, { "R", "G", "B", "A" }, { "R", "G", "B", "A" }) != 0) {
                            ++wrongRenders;
                        }
                    }
                }
                ++calls;
            }
            --running;
        });
    }
    int changes = 0, failedChanges = 0;
    while (running > 0) {
        if (!ok(r.changeFile(files[changes % 2]))) {
            ++failedChanges;
        }
        ++changes;
    }
    for (std::thread& t : threads) {
        t.join();
    }
    check(wrongPlanes == 0 && failedChanges == 0 && okRenders > 0 && wrongRenders == 0,
          "%d getClipComponents and renders, %d file changes (%d wrong planes, %d failed changes, %d renders succeeded, %d wrong)", (int)calls,
          changes, (int)wrongPlanes, failedChanges, (int)okRenders, (int)wrongRenders);
}

// A host that is not multi-planar: the color plane only, of the view of the render (the default view of the host's
// view 0, as ReadEXR 1)
void
testViewsColorOnly(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== a multi-view file in a host without planes ====\n");
    const std::string path = dir + "/views-color.exr";
    const std::vector<std::string> ch = { "R", "G", "B", "A", "right.R", "right.G", "right.B", "right.A" };
    if (!writeChannels(path, ch, { "left", "right" })) {
        ++gFailures;

        return;
    }
    Reader r;
    if (!r.create(p, path)) {
        ++gFailures;

        return;
    }
    gPersistentError.clear();
    const OfxStatus st = r.prepare();
    const OfxStatus rs = ok(st) ? r.renderPlane(kFnOfxImagePlaneColour, kOfxImageComponentRGBA, 4, 0) : st;
    const int bad = ok(rs) ? wrongPlaneValues(r, ch, { "R", "G", "B", "A" }) : -1;
    check(ok(rs) && bad == 0, "view 0: R, G, B, A (render = %d, %d wrong values)", rs, bad);
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

// OpenEXR's global thread pool is counted by its users' instances (ExrThreads.h, #250, #132): a ReadEXR instance
// starts it, even if no OIIO plug-in instance ever existed (ReadEXR never sizes it itself); on MS Windows, the
// destruction of the last user stops it, elsewhere it keeps its threads; a new instance starts it again.
void
testPoolRefcount(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== OpenEXR's thread pool, counted by its users' instances ====\n");
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    const bool stopsAtZero = true;
#else
    const bool stopsAtZero = false;
#endif
    const int hw = (int)std::thread::hardware_concurrency();
    if (hw <= 0) {
        std::printf("  SKIP: hardware_concurrency() is unknown\n");

        return;
    }
    const std::string path = dir + "/pool.exr";
    ExrSpec s;
    s.display = s.data = box(0, 0, 63, 31);
    if (!writeExr(path, s)) {
        return;
    }
    check(liveExrThreadsUsers() == 0, "no OpenEXR pool user alive (%d)", liveExrThreadsUsers());
    // as in a process where no OIIO plug-in instance ever existed (OpenImageIO starts it with exr_threads)
    Imf::setGlobalThreadCount(0);
    {
        Reader r;
        check(r.create(p, path), "ReadEXR instance");
        check(liveExrThreadsUsers() == 1, "1 OpenEXR pool user (%d)", liveExrThreadsUsers());
        check(exrPoolThreads() == hw, "a ReadEXR instance, no OIIO instance: the pool has %d threads (%d)", hw, exrPoolThreads());
        check(ok(r.prepareAndRender()), "render");
    }
    check(liveExrThreadsUsers() == 0, "no OpenEXR pool user alive (%d)", liveExrThreadsUsers());
    if (stopsAtZero) {
        check(exrPoolThreads() == 0, "the last user destroyed: the pool is stopped (MS Windows) (%d threads)", exrPoolThreads());
    } else {
        check(exrPoolThreads() == hw, "the last user destroyed: the pool keeps its threads (not MS Windows) (%d)", exrPoolThreads());
    }
    {
        Reader r;
        check(r.create(p, path), "a new ReadEXR instance");
        check(exrPoolThreads() == hw, "the pool has %d threads again (%d)", hw, exrPoolThreads());
        // an OIIO plug-in instance (its OIIOThreadsUser) created and destroyed while ReadEXR's exists, as Natron's Read
        // node does with a temporary ReadOIIO: OpenEXR's pool keeps its threads (#132)
        {
            OIIOThreadsUser oiio;
            check(liveExrThreadsUsers() == 2, "2 OpenEXR pool users (%d)", liveExrThreadsUsers());
        }
        check(exrPoolThreads() == hw, "an OIIO instance destroyed while a ReadEXR instance exists: the pool keeps its threads (%d)", exrPoolThreads());
        check(ok(r.prepareAndRender()), "render");
    }
}

// ReadEXR renders on 4 threads while another thread creates and destroys OIIO plug-in instances (their
// OIIOThreadsUser) repeatedly. Stopping OpenEXR's pool (setNumThreads(0)) while a render adds tasks may leave a task
// that no thread runs, and the render then waits forever for its TaskGroup: OpenEXR's pool must never be stopped
// while a ReadEXR instance exists (#250). Under a watchdog: a hang fails the test (and exits) after 300 s.
void
testPoolChurn(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== ReadEXR renders while OIIO instances are created and destroyed ====\n");
    const std::string path = dir + "/churn.exr";
    ExrSpec s;
    s.display = s.data = box(0, 0, 511, 383);
    s.compression = Imf::ZIP_COMPRESSION;
    s.type = Imf::HALF;
    s.bench = true;
    if (!writeExr(path, s)) {
        return;
    }
    const int kReaders = 4, kRenders = 20, kChurn = 400;
    std::vector<Reader> readers(kReaders);
    for (Reader& r : readers) {
        if (!r.create(p, path) || !ok(r.prepare())) {
            check(false, "ReadEXR instance");

            return;
        }
    }
    std::atomic<bool> done(false);
    std::thread watchdog([&done]() {
        for (int i = 0; i < 3000 && !done; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!done) {
            std::printf("  FAIL: renders and OIIO instances still running after 300 s: hung\n");
            std::fflush(stdout);
            std::_Exit(1);
        }
    });
    std::atomic<int> failures(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < kReaders; ++t) {
        threads.emplace_back([&, t]() {
            for (int n = 0; n < kRenders; ++n) {
                if (!ok(readers[t].render())) {
                    ++failures;
                }
            }
        });
    }
    threads.emplace_back([&]() {
        for (int n = 0; n < kChurn; ++n) {
            OIIOThreadsUser oiio;
            std::this_thread::yield();
        }
    });
    for (std::thread& t : threads) {
        t.join();
    }
    done = true;
    watchdog.join();
    check(failures == 0, "%d threads x %d renders while %d OIIO instances were created and destroyed: %d failed", kReaders, kRenders,
          kChurn, (int)failures);
    check(exrPoolThreads() > 0, "the pool has threads while the ReadEXR instances exist (%d)", exrPoolThreads());
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
    const bool multiPlanar = argc == 3 && !std::strcmp(argv[1], "--multiplanar");
    if (argc < 2 || (!isBench && !multiPlanar && argc != 2)) {
        std::printf("usage: %s <scratch dir> | --multiplanar <scratch dir> | --bench <scratch dir> [n]\n", argv[0]);

        return 2;
    }
    const std::string dir = (isBench || multiPlanar) ? argv[2] : argv[1];
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // (the output before a crash)
    initHost();
    if (multiPlanar) {
        // a Natron-like host: multiple planes (and dynamic choices, for ReadOIIO's output-layer menu)
        setMultiPlanarHost();
    }
    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadEXR");
    if (!p) {
        std::printf("FAIL: ReadEXR not found\n");

        return 1;
    }
    Effect desc;
    p->setHost(&gOfxHost);
    if (!ok(p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr)) || !ok(p->mainEntry(kOfxActionDescribe, EH(desc), nullptr, nullptr))) {
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
    check(p->pluginVersionMajor == 1 && p->pluginVersionMinor == 1, "version %u.%u (1.1: planes and views, #386, are compatible with 1.0)", p->pluginVersionMajor, p->pluginVersionMinor);
    if (multiPlanar) {
        int multiPlanarDesc = 0;
        pGetInt(PH(&desc.props), kFnOfxImageEffectPropMultiPlanar, 0, &multiPlanarDesc);
        check(multiPlanarDesc == 1, "multi-planar");
        // ReadOIIO, for the planes that it presents for the same files
        OfxPlugin* oiio = findPlugin("fr.inria.openfx.ReadOIIO");
        Effect oiioDesc;
        if (oiio) {
            oiio->setHost(&gOfxHost);
            if (!ok(oiio->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr)) || !ok(oiio->mainEntry(kOfxActionDescribe, EH(oiioDesc), nullptr, nullptr))) {
                oiio = nullptr;
            }
        }
        check(oiio != nullptr, "ReadOIIO, loaded");
        // a file without layers and views reads as in a host without planes
        ExrSpec s;
        s.display = s.data = box(0, 0, 99, 59);
        testPixels(p, dir, "default", s, { 0, 0, 100, 60 });
        testLayersViews(p, oiio, dir);
        testPlanesWhileFileChanges(p, dir);
        testThreads(p, dir);
        if (oiio) {
            oiio->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
        }
        p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
        std::printf("%s (%d failure%s)\n", gFailures ? "FAIL" : "PASS", gFailures, gFailures == 1 ? "" : "s");

        return gFailures ? 1 : 0;
    }

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
    // channel names that are empty once the digits and dots are removed, and non-ASCII bytes (#293): ignored
    s.uintRed = false;
    s.extra = { "1", "2.3", ".", "..", "4.", ".5", "layer.6", "\xc3\xa9" };
    testPixels(p, dir, "digit-dot-channels", s, { 0, 0, 64, 32 });
    // R, G, B and A in a layer, with a suffix, in a view, or after digits: read from the file's channels, not as
    // zeros (#358)
    s.extra.clear();
    s.prefix = "diffuse.";
    testPixels(p, dir, "layer-channels", s, { 0, 0, 64, 32 });
    s.uintRed = true;
    testPixels(p, dir, "layer-uint", s, { 0, 0, 64, 32 });
    s.uintRed = false;
    s.prefix = "right."; // a layer prefix, as in a multi-view file, but without the multiView attribute (#386)
    testPixels(p, dir, "right-layer-channels", s, { 0, 0, 64, 32 });
    s.prefix = "1";
    testPixels(p, dir, "digit-prefix-channels", s, { 0, 0, 64, 32 });
    s.prefix.clear();
    s.suffix = ".1";
    testPixels(p, dir, "suffix-channels", s, { 0, 0, 64, 32 });
    // plain R, G, B and A win over the others, wherever these are in the (sorted) channel list: 1G and Beauty.R
    // sort before the plain G and R, B.1, Beauty.A and diffuse.G after the plain A and B
    s.suffix.clear();
    s.extra = { "1G", "Beauty.R", "Beauty.A", "B.1", "diffuse.G" };
    testPixels(p, dir, "plain-and-layer-channels", s, { 0, 0, 64, 32 });
    // one group per image: a plain RGB image stays opaque (matte.A is not its alpha), and albedo.G (which sorts
    // first) is not mixed with the diffuse layer, which has more channels
    s.channels = "RGB";
    s.extra = { "matte.A" };
    testPixels(p, dir, "plain-rgb-layer-alpha", s, { 0, 0, 64, 32 });
    s.channels = "RGBA";
    s.prefix = "diffuse.";
    s.extra = { "albedo.G" };
    testPixels(p, dir, "two-layers", s, { 0, 0, 64, 32 });
    s.extra = { "specular.R", "specular.G", "specular.B", "specular.A" }; // a tie: the first layer wins
    testPixels(p, dir, "two-layers-tie", s, { 0, 0, 64, 32 });
    s.channels = "A";
    s.extra.clear();
    testPixels(p, dir, "layer-alpha-only", s, { 0, 0, 64, 32 });

    testViewsColorOnly(p, dir);
    testLumaChroma(p, dir, "");
    testLumaChroma(p, dir, "diffuse."); // (#358)
    testWindow(p, dir);
    testScale(p, dir);
    testScales(p, dir);
    testRewrite(p, dir);
    testRewriteCachedRoD(p, dir, false);
    testRewriteCachedRoD(p, dir, true);
    testRevert(p, dir);
    testWindowBlocks(p, dir);
    testTruncated(p, dir);
    testThreads(p, dir);
    testPoolRefcount(p, dir);
    testPoolChurn(p, dir);

    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::printf("%s (%d failure%s)\n", gFailures ? "FAIL" : "PASS", gFailures, gFailures == 1 ? "" : "s");

    return gFailures ? 1 : 0;
}
