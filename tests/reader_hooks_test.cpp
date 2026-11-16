/*
 * reader_hooks_test — unit test for GenericReaderPlugin's hooks for readers whose output
 * depends on animated parameters (IOSupport/GenericReader.{h,cpp}, issue #96):
 *
 *  - isOutputFrameVarying(): when it returns true, getClipPreferences() must set
 *    kOfxImageEffectFrameVarying on the output, even for a still image held before
 *    and after (which is not frame varying with the default implementation).
 *  - getRenderTime(): inside decode(), the time of the render action being processed
 *    (not the sequence/file time passed to decode()), restored when render() returns
 *    or throws, and per thread.
 *
 * Two minimal test readers are defined below (a GenericReaderPlugin subclass with the
 * default isOutputFrameVarying(), and a subclass returning true), and are driven
 * through the OFX actions by the in-process host of ofx_test_host.h. decode() records
 * getRenderTime() and its time argument, and fills the image with a value computed
 * from getRenderTime(), as a reader with an animated param would.
 *
 * Usage: reader_hooks_test <scratch file prefix>   (needs $OCIO, see tests/Makefile)
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include "GenericOCIO.h"
#include "GenericReader.h"
#include "ofxsMacros.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace OFX;
using namespace OFX::IO;

#define kPluginGrouping "Image/Readers"
#define kPluginIdentifierDefault "fr.inria.openfx.tests.ReaderHooks"
#define kPluginIdentifierFrameVarying "fr.inria.openfx.tests.ReaderHooksFrameVarying"
#define kPluginIdentifierMultiPlanar "fr.inria.openfx.tests.ReaderHooksMultiPlanar"
#define kSupportsRGBA true
#define kSupportsRGB false
#define kSupportsXY false
#define kSupportsAlpha false
#define kSupportsTiles false

namespace {

const int kWidth = 8;
const int kHeight = 4;
const OfxRangeI kVideoRange = { 1, 10 }; // the frame range of "video" files

// the value decode() writes to R, G and B at render time t
float
pixelValue(double t)
{
    return (float)(0.01 * t + 0.5);
}

struct DecodeRecord {
    double renderTime; // getRenderTime() in decode()
    double time; // decode()'s time argument (the sequence time)
    bool plane; // recorded by decodePlane() rather than decode()
};

// A reader of fake files: "video" files (whose name contains "video") have kVideoRange frames,
// others are still images. The file only needs to exist.
class TestReaderPlugin
    : public GenericReaderPlugin {
public:
    TestReaderPlugin(OfxImageEffectHandle handle, const std::vector<std::string>& extensions, bool multiPlanar = false)
        : GenericReaderPlugin(handle, extensions, kSupportsRGBA, kSupportsRGB, kSupportsXY, kSupportsAlpha, kSupportsTiles, multiPlanar)
    {
    }

    // what decode() or decodePlane() saw, in call order
    std::vector<DecodeRecord> records;
    // called from decode(), after recording
    std::function<void()> onDecode;
    // decode() throws (after recording and onDecode)
    bool throwInDecode = false;

    // getRenderTime() is protected
    static double renderTime() { return getRenderTime(); }

private:
    virtual bool isVideoStream(const std::string& filename) OVERRIDE FINAL
    {
        return filename.find("video") != std::string::npos;
    }

    virtual bool getSequenceTimeDomain(const std::string& filename, OfxRangeI& range) OVERRIDE FINAL
    {
        if (!isVideoStream(filename)) {
            return false;
        }
        range = kVideoRange;

        return true;
    }

    virtual void decode(const std::string& /*filename*/, OfxTime time, int /*view*/, bool /*isPlayback*/, const OfxRectI& renderWindow, const OfxPointD& /*renderScale*/, float* pixelData, const OfxRectI& bounds, PixelComponentEnum /*pixelComponents*/, int pixelComponentCount, int rowBytes) OVERRIDE FINAL
    {
        decodeImpl(time, false, renderWindow, pixelData, bounds, pixelComponentCount, rowBytes);
    }

    // called instead of decode() by multi-planar readers
    virtual void decodePlane(const std::string& /*filename*/, OfxTime time, int /*view*/, bool /*isPlayback*/, const OfxRectI& renderWindow, const OfxPointD& /*renderScale*/, float* pixelData, const OfxRectI& bounds,
                             PixelComponentEnum /*pixelComponents*/, PixelComponentEnum /*remappedComponents*/,
                             int pixelComponentCount, const std::string& /*rawComponents*/, int rowBytes) OVERRIDE FINAL
    {
        decodeImpl(time, true, renderWindow, pixelData, bounds, pixelComponentCount, rowBytes);
    }

    void decodeImpl(OfxTime time, bool plane, const OfxRectI& renderWindow, float* pixelData, const OfxRectI& bounds, int pixelComponentCount, int rowBytes)
    {
        const double t = getRenderTime();
        records.push_back({ t, time, plane });
        if (onDecode) {
            onDecode();
        }
        if (throwInDecode) {
            throwSuiteStatusException(kOfxStatFailed);
        }
        // the output depends on the render time, like a reader with an animated param evaluated at getRenderTime()
        const float v = pixelValue(t);
        for (int y = renderWindow.y1; y < renderWindow.y2; ++y) {
            float* pix = (float*)((char*)pixelData + (ptrdiff_t)(y - bounds.y1) * rowBytes) + (ptrdiff_t)(renderWindow.x1 - bounds.x1) * pixelComponentCount;
            for (int x = renderWindow.x1; x < renderWindow.x2; ++x, pix += pixelComponentCount) {
                for (int c = 0; c < pixelComponentCount; ++c) {
                    pix[c] = (c == 3) ? 1.f : v;
                }
            }
        }
    }

    virtual bool getFrameBounds(const std::string& /*filename*/, OfxTime /*time*/, int /*view*/, OfxRectI* bounds, OfxRectI* format, double* par, std::string* /*error*/, int* tile_width, int* tile_height) OVERRIDE FINAL
    {
        bounds->x1 = bounds->y1 = 0;
        bounds->x2 = kWidth;
        bounds->y2 = kHeight;
        *format = *bounds;
        *par = 1.;
        *tile_width = *tile_height = 0;

        return true;
    }

    virtual bool guessParamsFromFilename(const std::string& /*filename*/, std::string* colorspace, PreMultiplicationEnum* filePremult, PixelComponentEnum* components, int* componentCount) OVERRIDE FINAL
    {
        *colorspace = "scene_linear";
        *filePremult = eImagePreMultiplied;
        *components = ePixelComponentRGBA;
        *componentCount = 4;

        return true;
    }
};

// Same, but the output may change over time even for a still image (e.g. an animated param)
class FrameVaryingTestReaderPlugin
    : public TestReaderPlugin {
public:
    FrameVaryingTestReaderPlugin(OfxImageEffectHandle handle, const std::vector<std::string>& extensions)
        : TestReaderPlugin(handle, extensions)
    {
    }

private:
    virtual bool isOutputFrameVarying() const OVERRIDE FINAL { return true; }
};

// Same as FrameVaryingTestReaderPlugin, but multi-planar: GenericReader calls decodePlane() instead of decode()
class MultiPlanarTestReaderPlugin
    : public TestReaderPlugin {
public:
    MultiPlanarTestReaderPlugin(OfxImageEffectHandle handle, const std::vector<std::string>& extensions)
        : TestReaderPlugin(handle, extensions, true)
    {
    }

private:
    virtual bool isOutputFrameVarying() const OVERRIDE FINAL { return true; }
};

void
describeTestReader(ImageEffectDescriptor& desc, const std::vector<std::string>& extensions, const char* label, bool multiPlanar = false)
{
    GenericReaderDescribe(desc, extensions, /*evaluation=*/-1, kSupportsTiles, multiPlanar);
    desc.setLabel(label);
}

void
describeTestReaderInContext(ImageEffectDescriptor& desc, ContextEnum context, bool isVideoStreamPlugin)
{
    PageParamDescriptor* page = GenericReaderDescribeInContextBegin(desc, context, isVideoStreamPlugin,
                                                                    kSupportsRGBA, kSupportsRGB, kSupportsXY, kSupportsAlpha, kSupportsTiles, true);
    GenericReaderDescribeInContextEnd(desc, context, page, "scene_linear", "scene_linear");
}

mDeclareReaderPluginFactory(TestReaderPluginFactory, {}, false);
void TestReaderPluginFactory::load() { _extensions.assign(1, "tst"); }
void TestReaderPluginFactory::describe(ImageEffectDescriptor& desc) { describeTestReader(desc, _extensions, "ReaderHooksTest"); }
void TestReaderPluginFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum context) { describeTestReaderInContext(desc, context, isVideoStreamPlugin()); }
ImageEffect*
TestReaderPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    TestReaderPlugin* ret = new TestReaderPlugin(handle, _extensions);
    ret->restoreStateFromParams();

    return ret;
}

mDeclareReaderPluginFactory(FrameVaryingTestReaderPluginFactory, {}, false);
void FrameVaryingTestReaderPluginFactory::load() { _extensions.assign(1, "tst"); }
void FrameVaryingTestReaderPluginFactory::describe(ImageEffectDescriptor& desc) { describeTestReader(desc, _extensions, "ReaderHooksFrameVaryingTest"); }
void FrameVaryingTestReaderPluginFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum context) { describeTestReaderInContext(desc, context, isVideoStreamPlugin()); }
ImageEffect*
FrameVaryingTestReaderPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    TestReaderPlugin* ret = new FrameVaryingTestReaderPlugin(handle, _extensions);
    ret->restoreStateFromParams();

    return ret;
}

mDeclareReaderPluginFactory(MultiPlanarTestReaderPluginFactory, {}, false);
void MultiPlanarTestReaderPluginFactory::load() { _extensions.assign(1, "tst"); }
void MultiPlanarTestReaderPluginFactory::describe(ImageEffectDescriptor& desc) { describeTestReader(desc, _extensions, "ReaderHooksMultiPlanarTest", true); }
void MultiPlanarTestReaderPluginFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum context) { describeTestReaderInContext(desc, context, isVideoStreamPlugin()); }
ImageEffect*
MultiPlanarTestReaderPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    TestReaderPlugin* ret = new MultiPlanarTestReaderPlugin(handle, _extensions);
    ret->restoreStateFromParams();

    return ret;
}

TestReaderPluginFactory gDefaultFactory(kPluginIdentifierDefault, 1, 0);
mRegisterPluginFactoryInstance(gDefaultFactory)
FrameVaryingTestReaderPluginFactory gFrameVaryingFactory(kPluginIdentifierFrameVarying, 1, 0);
mRegisterPluginFactoryInstance(gFrameVaryingFactory)
MultiPlanarTestReaderPluginFactory gMultiPlanarFactory(kPluginIdentifierMultiPlanar, 1, 0);
mRegisterPluginFactoryInstance(gMultiPlanarFactory)

// ------------------------------------------------------------------ host side

int gFailures = 0;

void
check(bool cond, const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    std::printf(cond ? "  ok:   " : "  FAIL: ");
    std::vprintf(fmt, a);
    std::printf("\n");
    va_end(a);
    if (!cond) {
        ++gFailures;
    }
}

OfxImageEffectHandle
H(Effect& e)
{
    return reinterpret_cast<OfxImageEffectHandle>(&e);
}

// Load and describe a plugin once; contexts and instances are created by Reader below.
OfxPlugin*
loadPlugin(const char* id, Effect& describeEffect)
{
    OfxPlugin* p = findPlugin(id);
    if (!p) {
        return nullptr;
    }
    p->setHost(&gOfxHost);
    if (!ok(p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr)) || !ok(p->mainEntry(kOfxActionDescribe, H(describeEffect), nullptr, nullptr))) {
        return nullptr;
    }

    return p;
}

void
setParamInt(Effect& ctx, const char* name, int v)
{
    auto it = ctx.params.find(name);
    if (it == ctx.params.end()) {
        std::printf("  FAIL: no param %s\n", name);
        ++gFailures;

        return;
    }
    it->second.hasOv = true;
    it->second.ov.i = { v };
}

// An instance of a test reader in the Reader context, with its own clips and params
// (separate instances can render concurrently).
struct Reader {
    OfxPlugin* p = nullptr;
    Effect ctx; // context descriptor: clips and params
    Effect inst;
    TestReaderPlugin* plugin = nullptr;
    Clip* out = nullptr;

    bool create(OfxPlugin* plugin_, const std::string& filename)
    {
        p = plugin_;
        PropSet inArgs;
        pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextReader);
        if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, H(ctx), PH(&inArgs), nullptr))) {
            std::printf("  FAIL: describeInContext\n");

            return false;
        }
        setOverrideString(&ctx, kOfxImageEffectFileParamName, filename);
        inst.delegate = &ctx;
        setInstanceProps(inst, kWidth, kHeight, kVideoRange.max, kOfxImageEffectContextReader);
        if (!ok(p->mainEntry(kOfxActionCreateInstance, H(inst), nullptr, nullptr))) {
            std::printf("  FAIL: createInstance\n");

            return false;
        }
        void* data = nullptr;
        pGetPointer(PH(&inst.props), kOfxPropInstanceData, 0, &data);
        plugin = dynamic_cast<TestReaderPlugin*>(reinterpret_cast<ImageEffect*>(data));
        out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
        if (!plugin || !out) {
            std::printf("  FAIL: no plugin instance or output clip\n");

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
            std::printf("  FAIL: instanceChanged(filename)\n");

            return false;
        }
        out->connected = true;
        setupClipImage(out, kWidth, kHeight, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);

        return true;
    }

    // kOfxImageEffectFrameVarying set by getClipPreferences, or -1
    int frameVarying()
    {
        PropSet outArgs;
        if (!ok(p->mainEntry(kOfxImageEffectActionGetClipPreferences, H(inst), nullptr, PH(&outArgs)))) {
            return -1;
        }
        int v = -1;
        pGetInt(PH(&outArgs), kOfxImageEffectFrameVarying, 0, &v);

        return v;
    }

    OfxStatus render(double t)
    {
        PropSet a;
        pSetDouble(PH(&a), kOfxPropTime, 0, t);
        double rs[2] = { 1., 1. };
        pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, 0);
        PropSet o;
        OfxStatus st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, H(inst), PH(&a), PH(&o));
        if (!ok(st)) {
            return st;
        }
        PropSet r = makeRenderInArgs(t, kWidth, kHeight);

        return p->mainEntry(kOfxImageEffectActionRender, H(inst), PH(&r), nullptr);
    }

    // R of the first output pixel
    float outputValue() const { return reinterpret_cast<const float*>(out->buffer.data())[0]; }

    void destroy()
    {
        if (plugin) {
            p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
            plugin = nullptr;
        }
    }

    ~Reader() { destroy(); }
};

// 1. getClipPreferences sets kOfxImageEffectFrameVarying
void
testFrameVarying(OfxPlugin* pDefault, OfxPlugin* pFrameVarying, const std::string& still, const std::string& video)
{
    std::printf("==== isOutputFrameVarying(): kOfxImageEffectFrameVarying ====\n");
    {
        Reader r;
        if (r.create(pDefault, still)) {
            check(r.frameVarying() == 0, "default reader, held still image: frame varying = 0");
        } else {
            ++gFailures;
        }
    }
    {
        Reader r;
        if (r.create(pFrameVarying, still)) {
            check(r.frameVarying() == 1, "isOutputFrameVarying() = true, held still image: frame varying = 1");
        } else {
            ++gFailures;
        }
    }
    {
        Reader r;
        if (r.create(pDefault, video)) {
            check(r.frameVarying() == 1, "default reader, video: frame varying = 1");
        } else {
            ++gFailures;
        }
    }
}

// 2. getRenderTime() in decode() is the render time; it is restored after render()
void
testRenderTime(OfxPlugin* pFrameVarying, const std::string& still, const std::string& video)
{
    std::printf("==== getRenderTime() in decode(), still image held before/after ====\n");
    {
        Reader r;
        if (!r.create(pFrameVarying, still)) {
            ++gFailures;

            return;
        }
        const double times[] = { 1., 3.5, -2.25, 10.75, 1. };
        for (double t : times) {
            r.plugin->records.clear();
            OfxStatus st = r.render(t);
            check(ok(st), "render(%g) = %d", t, st);
            check(r.plugin->records.size() == 1, "render(%g): decode() called once (%zu)", t, r.plugin->records.size());
            if (!r.plugin->records.empty()) {
                const DecodeRecord& rec = r.plugin->records[0];
                check(rec.renderTime == t, "render(%g): getRenderTime() = %g, decode time = %g (held)", t, rec.renderTime, rec.time);
                check(rec.time == 1., "render(%g): decode time is the held frame 1", t);
            }
            check(std::fabs(r.outputValue() - pixelValue(t)) < 1e-6f, "render(%g): output %g = value at the render time %g", t, r.outputValue(), pixelValue(t));
            check(TestReaderPlugin::renderTime() == 0., "after render(%g) returned: getRenderTime() = %g (restored to 0)", t, TestReaderPlugin::renderTime());
        }
    }

    std::printf("==== getRenderTime() in decode(), video with time offset and frame range ====\n");
    {
        Reader r;
        if (!r.create(pFrameVarying, video)) {
            ++gFailures;

            return;
        }
        // time offset 5, frames 2..8 of 1..10, held before the first and after the last frame
        setParamInt(r.ctx, "timeOffset", 5);
        setParamInt(r.ctx, "firstFrame", 2);
        setParamInt(r.ctx, "lastFrame", 8);
        struct {
            double t, sequenceTime;
        } const cases[] = { { 6., 2. }, { 7., 2. }, { 9.5, 4.5 }, { 12., 7. }, { 20., 8. } };
        for (const auto& c : cases) {
            r.plugin->records.clear();
            OfxStatus st = r.render(c.t);
            check(ok(st), "render(%g) = %d", c.t, st);
            if (r.plugin->records.size() != 1) {
                check(false, "render(%g): decode() called once (%zu)", c.t, r.plugin->records.size());
                continue;
            }
            const DecodeRecord& rec = r.plugin->records[0];
            check(rec.renderTime == c.t, "render(%g): getRenderTime() = %g", c.t, rec.renderTime);
            check(rec.time == c.sequenceTime, "render(%g): decode time = %g (expected sequence time %g)", c.t, rec.time, c.sequenceTime);
            check(std::fabs(r.outputValue() - pixelValue(c.t)) < 1e-6f, "render(%g): output %g = value at the render time %g", c.t, r.outputValue(), pixelValue(c.t));
        }
        check(TestReaderPlugin::renderTime() == 0., "after the renders: getRenderTime() = %g (restored to 0)", TestReaderPlugin::renderTime());
    }

    std::printf("==== getRenderTime() restored after render() throws ====\n");
    {
        Reader r;
        if (!r.create(pFrameVarying, still)) {
            ++gFailures;

            return;
        }
        r.plugin->throwInDecode = true;
        OfxStatus st = r.render(4.5);
        check(st == kOfxStatFailed, "render(4.5) with decode() throwing = %d (kOfxStatFailed)", st);
        check(r.plugin->records.size() == 1 && r.plugin->records[0].renderTime == 4.5, "decode() saw getRenderTime() = 4.5 before throwing");
        check(TestReaderPlugin::renderTime() == 0., "after render(4.5) threw: getRenderTime() = %g (restored to 0)", TestReaderPlugin::renderTime());
    }

    std::printf("==== getRenderTime() restored after a nested render() returns or throws ====\n");
    {
        // a render action processed by the same thread during another one (e.g. the host renders
        // an upstream node from the same thread): the outer render time must be restored
        Reader outer, inner;
        if (!outer.create(pFrameVarying, still) || !inner.create(pFrameVarying, still)) {
            ++gFailures;

            return;
        }
        std::vector<double> seen; // getRenderTime() in the outer decode(): before, after the nested render, after the nested throwing render
        OfxStatus innerSt = kOfxStatOK, innerThrowSt = kOfxStatOK;
        outer.plugin->onDecode = [&]() {
            seen.push_back(TestReaderPlugin::renderTime());
            innerSt = inner.render(8.);
            seen.push_back(TestReaderPlugin::renderTime());
            inner.plugin->throwInDecode = true;
            innerThrowSt = inner.render(9.);
            inner.plugin->throwInDecode = false;
            seen.push_back(TestReaderPlugin::renderTime());
        };
        OfxStatus st = outer.render(3.);
        check(ok(st), "outer render(3) = %d", st);
        check(ok(innerSt) && innerThrowSt == kOfxStatFailed, "nested render(8) = %d, nested throwing render(9) = %d", innerSt, innerThrowSt);
        check(inner.plugin->records.size() == 2 && inner.plugin->records[0].renderTime == 8. && inner.plugin->records[1].renderTime == 9.,
              "nested decode() saw getRenderTime() = 8, then 9");
        check(seen.size() == 3 && seen[0] == 3., "outer decode(): getRenderTime() = %g before the nested render", seen.empty() ? -1. : seen[0]);
        check(seen.size() == 3 && seen[1] == 3., "outer decode(): getRenderTime() = %g after the nested render returned (restored)", seen.size() < 2 ? -1. : seen[1]);
        check(seen.size() == 3 && seen[2] == 3., "outer decode(): getRenderTime() = %g after the nested render threw (restored)", seen.size() < 3 ? -1. : seen[2]);
        check(std::fabs(outer.outputValue() - pixelValue(3.)) < 1e-6f, "outer output %g = value at 3 (%g)", outer.outputValue(), pixelValue(3.));
        check(TestReaderPlugin::renderTime() == 0., "after the outer render: getRenderTime() = %g (restored to 0)", TestReaderPlugin::renderTime());
    }
}

// 2d. the same through decodePlane(), for multi-planar readers
void
testDecodePlane(OfxPlugin* pMultiPlanar, const std::string& still, const std::string& video)
{
    std::printf("==== getRenderTime() in decodePlane() (multi-planar reader) ====\n");
    {
        Reader r;
        if (!r.create(pMultiPlanar, still)) {
            ++gFailures;

            return;
        }
        check(r.frameVarying() == 1, "multi-planar reader, isOutputFrameVarying() = true, held still image: frame varying = 1");
        const double times[] = { 2.5, -1.25, 6. };
        for (double t : times) {
            r.plugin->records.clear();
            OfxStatus st = r.render(t);
            check(ok(st), "render(%g) = %d", t, st);
            if (r.plugin->records.size() != 1) {
                check(false, "render(%g): decodePlane() called once (%zu)", t, r.plugin->records.size());
                continue;
            }
            const DecodeRecord& rec = r.plugin->records[0];
            check(rec.plane, "render(%g): decodePlane() was called, not decode()", t);
            check(rec.renderTime == t, "render(%g): getRenderTime() = %g, decodePlane time = %g (held)", t, rec.renderTime, rec.time);
            check(rec.time == 1., "render(%g): decodePlane time is the held frame 1", t);
            check(std::fabs(r.outputValue() - pixelValue(t)) < 1e-6f, "render(%g): output %g = value at the render time %g", t, r.outputValue(), pixelValue(t));
            check(TestReaderPlugin::renderTime() == 0., "after render(%g) returned: getRenderTime() = %g (restored to 0)", t, TestReaderPlugin::renderTime());
        }
        r.plugin->records.clear();
        r.plugin->throwInDecode = true;
        OfxStatus st = r.render(4.5);
        check(st == kOfxStatFailed, "render(4.5) with decodePlane() throwing = %d (kOfxStatFailed)", st);
        check(r.plugin->records.size() == 1 && r.plugin->records[0].renderTime == 4.5, "decodePlane() saw getRenderTime() = 4.5 before throwing");
        check(TestReaderPlugin::renderTime() == 0., "after render(4.5) threw: getRenderTime() = %g (restored to 0)", TestReaderPlugin::renderTime());
    }
    {
        Reader r;
        if (!r.create(pMultiPlanar, video)) {
            ++gFailures;

            return;
        }
        setParamInt(r.ctx, "timeOffset", 5);
        r.plugin->records.clear();
        OfxStatus st = r.render(9.5);
        check(ok(st), "video, time offset 5: render(9.5) = %d", st);
        check(r.plugin->records.size() == 1 && r.plugin->records[0].plane && r.plugin->records[0].renderTime == 9.5 && r.plugin->records[0].time == 4.5,
              "video, time offset 5: decodePlane() saw getRenderTime() = %g, time = %g (expected 9.5, 4.5)",
              r.plugin->records.empty() ? -1. : r.plugin->records[0].renderTime, r.plugin->records.empty() ? -1. : r.plugin->records[0].time);
    }
}

// Waits until n threads arrived, or a timeout (if the renders are serialized), so that both
// threads are in decode() at the same time.
class Rendezvous {
public:
    explicit Rendezvous(int n)
        : _n(n)
    {
    }

    bool arriveAndWait()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        if (++_arrived == _n) {
            _cond.notify_all();

            return true;
        }

        return _cond.wait_for(lock, std::chrono::seconds(10), [this]() { return _arrived >= _n; });
    }

private:
    std::mutex _mutex;
    std::condition_variable _cond;
    const int _n;
    int _arrived = 0;
};

// 2c. two threads rendering different times concurrently each see their own render time
void
testThreads(OfxPlugin* pFrameVarying, const std::string& still)
{
    std::printf("==== getRenderTime() with two concurrent renders ====\n");
    Reader a, b;
    if (!a.create(pFrameVarying, still) || !b.create(pFrameVarying, still)) {
        ++gFailures;

        return;
    }
    Rendezvous rendezvous(2), rendezvous2(2);
    // in decode(): getRenderTime() before and after both threads have set their render time
    std::vector<double> seenA, seenB;
    bool metA = false, metB = false;
    // (the second rendezvous keeps both renders running until both threads have read it again)
    a.plugin->onDecode = [&]() {
        seenA.push_back(TestReaderPlugin::renderTime());
        metA = rendezvous.arriveAndWait();
        seenA.push_back(TestReaderPlugin::renderTime());
        metA = rendezvous2.arriveAndWait() && metA;
    };
    b.plugin->onDecode = [&]() {
        seenB.push_back(TestReaderPlugin::renderTime());
        metB = rendezvous.arriveAndWait();
        seenB.push_back(TestReaderPlugin::renderTime());
        metB = rendezvous2.arriveAndWait() && metB;
    };
    const double tA = 2.25, tB = 7.75;
    OfxStatus stA = kOfxStatFailed, stB = kOfxStatFailed;
    double afterA = -1., afterB = -1.;
    std::thread threadA([&]() { stA = a.render(tA); afterA = TestReaderPlugin::renderTime(); });
    std::thread threadB([&]() { stB = b.render(tB); afterB = TestReaderPlugin::renderTime(); });
    threadA.join();
    threadB.join();
    check(ok(stA) && ok(stB), "render(%g) = %d and render(%g) = %d in two threads", tA, stA, tB, stB);
    check(metA && metB, "both threads were in decode() at the same time");
    check(seenA.size() == 2 && seenA[0] == tA && seenA[1] == tA, "thread A: getRenderTime() = %g, %g (expected %g)",
          seenA.size() > 0 ? seenA[0] : -1., seenA.size() > 1 ? seenA[1] : -1., tA);
    check(seenB.size() == 2 && seenB[0] == tB && seenB[1] == tB, "thread B: getRenderTime() = %g, %g (expected %g)",
          seenB.size() > 0 ? seenB[0] : -1., seenB.size() > 1 ? seenB[1] : -1., tB);
    check(std::fabs(a.outputValue() - pixelValue(tA)) < 1e-6f && std::fabs(b.outputValue() - pixelValue(tB)) < 1e-6f,
          "outputs %g, %g = values at %g, %g", a.outputValue(), b.outputValue(), tA, tB);
    check(afterA == 0. && afterB == 0., "after render() returned in each thread: getRenderTime() = %g, %g (0)", afterA, afterB);
}

} // namespace

int
main(int argc, char** argv)
{
    initHost();
    // GenericReader only calls decodePlane() for multi-planar readers on a multi-planar host
    pSetInt(PH(&gHostProps), kFnOfxImageEffectPropMultiPlanar, 0, 1);
    const std::string prefix = (argc > 1) ? argv[1] : "/tmp/reader_hooks_test";
    // the test readers only check that the files exist
    const std::string still = prefix + ".still.tst";
    const std::string video = prefix + ".video.tst";
    for (const std::string& f : { still, video }) {
        std::FILE* fp = std::fopen(f.c_str(), "wb");
        if (!fp) {
            std::printf("FAIL: cannot create %s\n", f.c_str());

            return 1;
        }
        std::fclose(fp);
    }

    Effect descDefault, descFrameVarying, descMultiPlanar;
    OfxPlugin* pDefault = loadPlugin(kPluginIdentifierDefault, descDefault);
    OfxPlugin* pFrameVarying = loadPlugin(kPluginIdentifierFrameVarying, descFrameVarying);
    OfxPlugin* pMultiPlanar = loadPlugin(kPluginIdentifierMultiPlanar, descMultiPlanar);
    if (!pDefault || !pFrameVarying || !pMultiPlanar) {
        std::printf("FAIL: cannot load the test readers\n");

        return 1;
    }

    testFrameVarying(pDefault, pFrameVarying, still, video);
    testRenderTime(pFrameVarying, still, video);
    testThreads(pFrameVarying, still);
    testDecodePlane(pMultiPlanar, still, video);

    pDefault->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    pFrameVarying->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    pMultiPlanar->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::remove(still.c_str());
    std::remove(video.c_str());

    std::printf("Result: failures=%d\n", gFailures);
    std::printf(gFailures == 0 ? "PASS\n" : "FAIL\n");

    return gFailures == 0 ? 0 : 1;
}
