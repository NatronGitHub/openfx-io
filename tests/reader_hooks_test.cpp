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
 * It also checks the downscaling done by GenericReader for render scales below 1 (issue #104):
 *
 *  - render scale 1/2, 1/4 and 1/8 (mipmap levels 1 to 3) with a reader that doesn't
 *    support render scale (decode() always reads full-resolution pixels), so that
 *    GenericReader halves the image once per level (scalePixelData, buildMipMapLevel).
 *    Before the fix, level 2 or more dereferenced a null temporary buffer.
 *  - refreshFrameBounds() (#163) is only called when the part of the render window in the host's RoD is out of the
 *    frame bounds, not for a render window larger than an up-to-date RoD (e.g. a display window).
 *  - clipToFile() (#232) clears the part of the window out of the file, with any depth and row bytes; an Opaque file
 *    rewritten smaller between getFrameBounds() and decode() gets alpha = 1 only where decode() wrote the image
 *    (clipToFile()'s window, or the one reported with setDecodedWindow()), transparent black elsewhere.
 *
 * And that render() reads the file param once (#373): renders on several threads while the main thread switches the
 * file param between two files of different sizes give one of the two files (a debug build used to abort), and a
 * proxy file is read at render scale 1/2, the full-resolution file at scale 1.
 *
 * And getClipPreferences() for an image sequence whose starting time is not its first frame
 * (issue #147): the PAR must be read from the first file of the sequence, looked up at the
 * first frame of the sequence (not at the host time of that frame). A file with a PAR of 0
 * gives an output PAR of 1. getLevelFromScale() and upscalePowerOfTwo() (IOUtility.h) are
 * checked for degenerate inputs that gave NaN.
 *
 * And that startingTime and timeOffset stay consistent (issue #154): timeOffset = startingTime - firstFrame, the time
 * domain starts at startingTime and render maps it to the first frame, when the user changes startingTime, timeOffset
 * or firstFrame, when the plug-in changes startingTime for a new file (as Natron sets originalFrameRange), and on a
 * project load (where no persistent param may be set).
 *
 * Minimal test readers are defined below (a GenericReaderPlugin subclass with the
 * default isOutputFrameVarying(), and a subclass returning true), and are driven
 * through the OFX actions by the in-process host of ofx_test_host.h. decode() records
 * getRenderTime() and its time argument, and fills the image with a value computed
 * from getRenderTime(), as a reader with an animated param would. The mipmap test
 * reader instead fills the image with known content (a function of x, y and the channel)
 * over frame bounds that have an odd size and an odd offset.
 *
 * Usage: reader_hooks_test <scratch file prefix>   (needs $OCIO, see tests/Makefile)
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include "GenericOCIO.h"
#include "GenericReader.h"
#include "IOUtility.h"
#include "ofxsMacros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace OFX;
using namespace OFX::IO;

#define kPluginGrouping "Image/Readers"
#define kPluginIdentifierDefault "fr.inria.openfx.tests.ReaderHooks"
#define kPluginIdentifierFrameVarying "fr.inria.openfx.tests.ReaderHooksFrameVarying"
#define kPluginIdentifierMultiPlanar "fr.inria.openfx.tests.ReaderHooksMultiPlanar"
#define kPluginIdentifierMipmap "fr.inria.openfx.tests.ReaderHooksMipmap"
#define kPluginIdentifierTiles "fr.inria.openfx.tests.ReaderHooksTiles"
#define kPluginIdentifierClip "fr.inria.openfx.tests.ReaderHooksClip"
#define kSupportsRGBA true
#define kSupportsRGB false
#define kSupportsXY false
#define kSupportsAlpha false
#define kSupportsTiles false
#define kParamGuessedParamsName "ParamExistingInstance" // GenericReader's kParamGuessedParams

namespace {

const int kWidth = 8;
const int kHeight = 4;
// the size of the "big" files (whose name ends with ".big.tst"), for the file switch test (#373)
const int kBigWidth = 16; // twice the size of the others (the proxy test)
const int kBigHeight = 8;
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
    OfxRectI renderWindow; // decode()'s render window
    OfxPointD renderScale; // decode()'s render scale
};

// The image read by the mipmap test reader: its frame bounds (in full-resolution pixel
// coordinates) and its pixel values, as a function of the full-resolution pixel position
// and the channel.
struct MipmapContent {
    OfxRectI bounds;
    std::function<float(int x, int y, int c)> value;
};
MipmapContent gMipmapContent; // read by the mipmap test reader instances

// The clip test reader (#232): getFrameBounds() returns gMipmapContent.bounds, but the file that decode() opens has
// the image fileBounds (as if it had been rewritten smaller in between). decode() calls clipToFile() with it, writes
// gMipmapContent.value() in the window that it returns (alpha 0.5), and, if reportFrameBounds, then reports the whole
// frame bounds with setDecodedWindow() (as ReadOIIO does for the black border of a data window).
struct ClipContent {
    OfxRectI fileBounds;
    bool reportFrameBounds;
};
ClipContent gClipContent;

// A reader of fake files: "video" files (whose name contains "video") have kVideoRange frames,
// others are still images or image sequences. The file only needs to exist. Files whose name
// contains "par2" have a pixel aspect ratio of 2, and those whose name contains "par0" an invalid pixel aspect ratio of 0.
// Files whose name ends with ".big.tst" are kBigWidth x kBigHeight, and their R, G and B are 1 more.
class TestReaderPlugin
    : public GenericReaderPlugin {
public:
    TestReaderPlugin(OfxImageEffectHandle handle, const std::vector<std::string>& extensions, bool multiPlanar = false, const MipmapContent* content = nullptr, bool supportsTiles = kSupportsTiles, const ClipContent* clip = nullptr)
        : GenericReaderPlugin(handle, extensions, kSupportsRGBA, kSupportsRGB, kSupportsXY, kSupportsAlpha, supportsTiles, multiPlanar)
        , _content(content)
        , _clip(clip)
    {
    }

    // clipToFile() is protected
    static bool clip(const OfxRectI& renderWindow, const OfxRectI& fileBounds, void* pixelData, const OfxRectI& bounds, int pixelComponentCount, BitDepthEnum bitDepth, int rowBytes, OfxRectI* window)
    {
        return clipToFile(renderWindow, fileBounds, pixelData, bounds, pixelComponentCount, bitDepth, rowBytes, window);
    }

    // what decode() or decodePlane() saw, in call order
    std::vector<DecodeRecord> records;
    // getFrameBounds() calls: the filename and the time (the sequence time), in call order
    std::vector<std::pair<std::string, double>> frameBoundsCalls;
    // refreshFrameBounds() calls
    int refreshCalls = 0;
    // called from decode(), after recording
    std::function<void()> onDecode;
    // decode() throws (after recording and onDecode)
    bool throwInDecode = false;

    // getRenderTime() is protected
    static double renderTime() { return getRenderTime(); }

    static bool isBig(const std::string& filename)
    {
        const std::string suffix = ".big.tst";

        return filename.size() >= suffix.size() && filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

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

    virtual void decode(const std::string& filename, OfxTime time, int /*view*/, const OfxRectI& renderWindow, const OfxPointD& renderScale, float* pixelData, const OfxRectI& bounds, PixelComponentEnum /*pixelComponents*/, int pixelComponentCount, int rowBytes) OVERRIDE FINAL
    {
        decodeImpl(filename, time, false, renderWindow, renderScale, pixelData, bounds, pixelComponentCount, rowBytes);
    }

    // called instead of decode() by multi-planar readers
    virtual void decodePlane(const std::string& filename, OfxTime time, int /*view*/, const OfxRectI& renderWindow, const OfxPointD& renderScale, float* pixelData, const OfxRectI& bounds,
                             PixelComponentEnum /*pixelComponents*/, PixelComponentEnum /*remappedComponents*/,
                             int pixelComponentCount, const std::string& /*rawComponents*/, int rowBytes) OVERRIDE FINAL
    {
        decodeImpl(filename, time, true, renderWindow, renderScale, pixelData, bounds, pixelComponentCount, rowBytes);
    }

    void decodeImpl(const std::string& filename, OfxTime time, bool plane, const OfxRectI& renderWindow, const OfxPointD& renderScale, float* pixelData, const OfxRectI& bounds, int pixelComponentCount, int rowBytes)
    {
        const double t = getRenderTime();
        records.push_back({ t, time, plane, renderWindow, renderScale });
        if (_clip) {
            OfxRectI window;
            if (clipToFile(renderWindow, _clip->fileBounds, pixelData, bounds, pixelComponentCount, eBitDepthFloat, rowBytes, &window)) {
                for (int y = window.y1; y < window.y2; ++y) {
                    float* pix = (float*)((char*)pixelData + (ptrdiff_t)(y - bounds.y1) * rowBytes) + (ptrdiff_t)(window.x1 - bounds.x1) * pixelComponentCount;
                    for (int x = window.x1; x < window.x2; ++x, pix += pixelComponentCount) {
                        for (int c = 0; c < pixelComponentCount; ++c) {
                            pix[c] = (c == 3) ? 0.5f : _content->value(x, y, c);
                        }
                    }
                }
            }
            if (_clip->reportFrameBounds) {
                OfxRectI frameWindow;
                intersectWindow(renderWindow, _content->bounds, &frameWindow);
                setDecodedWindow(frameWindow);
            }

            return;
        }
        if (_content) {
            // a reader that doesn't support render scale: whatever the render scale, the render
            // window and the pixels are at full resolution, and GenericReader downscales them
            for (int y = renderWindow.y1; y < renderWindow.y2; ++y) {
                float* pix = (float*)((char*)pixelData + (ptrdiff_t)(y - bounds.y1) * rowBytes) + (ptrdiff_t)(renderWindow.x1 - bounds.x1) * pixelComponentCount;
                for (int x = renderWindow.x1; x < renderWindow.x2; ++x, pix += pixelComponentCount) {
                    for (int c = 0; c < pixelComponentCount; ++c) {
                        pix[c] = _content->value(x, y, c);
                    }
                }
            }

            return;
        }
        if (onDecode) {
            onDecode();
        }
        if (throwInDecode) {
            throwSuiteStatusException(kOfxStatFailed);
        }
        // the output depends on the render time, like a reader with an animated param evaluated at getRenderTime()
        const float v = pixelValue(t) + (isBig(filename) ? 1.f : 0.f);
        for (int y = renderWindow.y1; y < renderWindow.y2; ++y) {
            float* pix = (float*)((char*)pixelData + (ptrdiff_t)(y - bounds.y1) * rowBytes) + (ptrdiff_t)(renderWindow.x1 - bounds.x1) * pixelComponentCount;
            for (int x = renderWindow.x1; x < renderWindow.x2; ++x, pix += pixelComponentCount) {
                for (int c = 0; c < pixelComponentCount; ++c) {
                    pix[c] = (c == 3) ? 1.f : v;
                }
            }
        }
    }

    virtual bool getFrameBounds(const std::string& filename, OfxTime time, int /*view*/, OfxRectI* bounds, OfxRectI* format, double* par, std::string* /*error*/, int* tile_width, int* tile_height) OVERRIDE FINAL
    {
        frameBoundsCalls.push_back({ filename, time });
        if (_content) {
            *bounds = _content->bounds;
        } else if (isBig(filename)) {
            bounds->x1 = bounds->y1 = 0;
            bounds->x2 = kBigWidth;
            bounds->y2 = kBigHeight;
        } else {
            bounds->x1 = bounds->y1 = 0;
            bounds->x2 = kWidth;
            bounds->y2 = kHeight;
        }
        *format = *bounds;
        *par = (filename.find("par2") != std::string::npos) ? 2. : ((filename.find("par0") != std::string::npos) ? 0. : 1.);
        if (filename.find("par40_33") != std::string::npos) {
            *par = 40. / 33.;
        }
        *tile_width = *tile_height = 0;

        return true;
    }

    virtual bool refreshFrameBounds(const std::string& /*filename*/, OfxTime /*time*/, int /*view*/) OVERRIDE FINAL
    {
        ++refreshCalls;

        return false;
    }

    virtual bool guessParamsFromFilename(const std::string& /*filename*/, std::string* colorspace, PreMultiplicationEnum* filePremult, PixelComponentEnum* components, int* componentCount) OVERRIDE FINAL
    {
        *colorspace = "scene_linear";
        *filePremult = eImagePreMultiplied;
        *components = ePixelComponentRGBA;
        *componentCount = 4;

        return true;
    }

    const MipmapContent* _content; // if not null: the mipmap test reader's image
    const ClipContent* _clip; // if not null: the clip test reader's file
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
describeTestReader(ImageEffectDescriptor& desc, const std::vector<std::string>& extensions, const char* label, bool multiPlanar = false, bool supportsTiles = kSupportsTiles)
{
    GenericReaderDescribe(desc, extensions, /*evaluation=*/-1, supportsTiles, multiPlanar);
    desc.setLabel(label);
}

void
describeTestReaderInContext(ImageEffectDescriptor& desc, ContextEnum context, bool isVideoStreamPlugin, bool supportsTiles = kSupportsTiles)
{
    PageParamDescriptor* page = GenericReaderDescribeInContextBegin(desc, context, isVideoStreamPlugin,
                                                                    kSupportsRGBA, kSupportsRGB, kSupportsXY, kSupportsAlpha, supportsTiles, true);
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

// Reads gMipmapContent (set before creating the instance) at full resolution
mDeclareReaderPluginFactory(MipmapTestReaderPluginFactory, {}, false);
void MipmapTestReaderPluginFactory::load() { _extensions.assign(1, "tst"); }
void MipmapTestReaderPluginFactory::describe(ImageEffectDescriptor& desc) { describeTestReader(desc, _extensions, "ReaderHooksMipmapTest"); }
void MipmapTestReaderPluginFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum context) { describeTestReaderInContext(desc, context, isVideoStreamPlugin()); }
ImageEffect*
MipmapTestReaderPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    TestReaderPlugin* ret = new TestReaderPlugin(handle, _extensions, false, &gMipmapContent);
    ret->restoreStateFromParams();

    return ret;
}

// The same, supporting tiles: the host may give it an output image larger than the RoD (e.g. a display window)
mDeclareReaderPluginFactory(TilesTestReaderPluginFactory, {}, false);
void TilesTestReaderPluginFactory::load() { _extensions.assign(1, "tst"); }
void TilesTestReaderPluginFactory::describe(ImageEffectDescriptor& desc) { describeTestReader(desc, _extensions, "ReaderHooksTilesTest", false, true); }
void TilesTestReaderPluginFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum context) { describeTestReaderInContext(desc, context, isVideoStreamPlugin(), true); }
ImageEffect*
TilesTestReaderPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    TestReaderPlugin* ret = new TestReaderPlugin(handle, _extensions, false, &gMipmapContent, true);
    ret->restoreStateFromParams();

    return ret;
}

// Reads gMipmapContent with getFrameBounds(), and the smaller gClipContent in decode()
mDeclareReaderPluginFactory(ClipTestReaderPluginFactory, {}, false);
void ClipTestReaderPluginFactory::load() { _extensions.assign(1, "tst"); }
void ClipTestReaderPluginFactory::describe(ImageEffectDescriptor& desc) { describeTestReader(desc, _extensions, "ReaderHooksClipTest"); }
void ClipTestReaderPluginFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum context) { describeTestReaderInContext(desc, context, isVideoStreamPlugin()); }
ImageEffect*
ClipTestReaderPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    TestReaderPlugin* ret = new TestReaderPlugin(handle, _extensions, false, &gMipmapContent, false, &gClipContent);
    ret->restoreStateFromParams();

    return ret;
}

TestReaderPluginFactory gDefaultFactory(kPluginIdentifierDefault, 1, 0);
mRegisterPluginFactoryInstance(gDefaultFactory)
FrameVaryingTestReaderPluginFactory gFrameVaryingFactory(kPluginIdentifierFrameVarying, 1, 0);
mRegisterPluginFactoryInstance(gFrameVaryingFactory)
MultiPlanarTestReaderPluginFactory gMultiPlanarFactory(kPluginIdentifierMultiPlanar, 1, 0);
mRegisterPluginFactoryInstance(gMultiPlanarFactory)
MipmapTestReaderPluginFactory gMipmapFactory(kPluginIdentifierMipmap, 1, 0);
mRegisterPluginFactoryInstance(gMipmapFactory)
TilesTestReaderPluginFactory gTilesFactory(kPluginIdentifierTiles, 1, 0);
mRegisterPluginFactoryInstance(gTilesFactory)
ClipTestReaderPluginFactory gClipFactory(kPluginIdentifierClip, 1, 0);
mRegisterPluginFactoryInstance(gClipFactory)

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
    // optional, set before create(): sets the param values before createInstance, as a host loading a project does
    std::function<void(Effect& ctx)> restore;
    // the reason of the instanceChanged(filename) sent after createInstance, or nullptr for none
    const char* fileChangeReason = kOfxChangeUserEdited;

    // instanceChanged for the param `name`, with the given reason (kOfxChangeUserEdited...)
    bool changed(const char* name, const char* reason, double time = 1.)
    {
        PropSet a;
        OfxPropertySetHandle ap = PH(&a);
        pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
        pSetString(ap, kOfxPropName, 0, name);
        pSetString(ap, kOfxPropChangeReason, 0, reason);
        pSetDouble(ap, kOfxPropTime, 0, time);
        double rs[2] = { 1., 1. };
        pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        if (!ok(p->mainEntry(kOfxActionBeginInstanceChanged, H(inst), PH(&a), nullptr))) {
            return false;
        }
        const bool ret = ok(p->mainEntry(kOfxActionInstanceChanged, H(inst), PH(&a), nullptr));

        return ok(p->mainEntry(kOfxActionEndInstanceChanged, H(inst), PH(&a), nullptr)) && ret;
    }

    // fileAtTime (optional): the value of the file param at each time, as for an image sequence
    bool create(OfxPlugin* plugin_, const std::string& filename, std::function<std::string(double)> fileAtTime = nullptr)
    {
        p = plugin_;
        PropSet inArgs;
        pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextReader);
        if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, H(ctx), PH(&inArgs), nullptr))) {
            std::printf("  FAIL: describeInContext\n");

            return false;
        }
        setOverrideString(&ctx, kOfxImageEffectFileParamName, filename);
        if (fileAtTime) {
            ctx.params[kOfxImageEffectFileParamName].stringAtTime = fileAtTime;
        }
        if (restore) {
            restore(ctx);
        }
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
        if (fileChangeReason && !changed(kOfxImageEffectFileParamName, fileChangeReason)) {
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

    // the output PAR set by getClipPreferences, or -1 if it was not set (or getClipPreferences failed)
    double outputPAR()
    {
        PropSet outArgs;
        if (!ok(p->mainEntry(kOfxImageEffectActionGetClipPreferences, H(inst), nullptr, PH(&outArgs)))) {
            return -1.;
        }
        double v = -1.;
        pGetDouble(PH(&outArgs), "OfxImageClipPropPAR_" kOfxImageEffectOutputClipName, 0, &v);

        return v;
    }

    // the time domain returned by getTimeDomain, or (-1, -1) if it failed
    OfxRangeD timeDomain()
    {
        PropSet outArgs;
        OfxRangeD range = { -1., -1. };
        if (ok(p->mainEntry(kOfxImageEffectActionGetTimeDomain, H(inst), nullptr, PH(&outArgs)))) {
            double r[2] = { -1., -1. };
            pGetDoubleN(PH(&outArgs), kOfxImageEffectPropFrameRange, 2, r);
            range = { r[0], r[1] };
        }

        return range;
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

    // Render at render scale 1/2^level, with the given render window (in pixel coordinates at that
    // scale), into an output image covering the whole RoD at that scale (the readers don't support
    // tiles), filled with `fill` beforehand. The RoD returned by getRegionOfDefinition is stored in rod.
    OfxStatus renderAtScale(double t, unsigned int level, const OfxRectI& renderWindow, float fill, OfxRectD* rod, OfxRectI* bounds)
    {
        const double s = 1. / (1 << level);
        double rs[2] = { s, s };
        PropSet a;
        pSetDouble(PH(&a), kOfxPropTime, 0, t);
        pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, 0);
        PropSet o;
        OfxStatus st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, H(inst), PH(&a), PH(&o));
        if (!ok(st)) {
            return st;
        }
        // the RoD is in canonical coordinates (independent of the render scale)
        double r[4] = { 0., 0., 0., 0. };
        pGetDoubleN(PH(&o), kOfxImageEffectPropRegionOfDefinition, 4, r);
        *rod = { r[0], r[1], r[2], r[3] };
        // The output image: its bounds and RoD are in pixel coordinates, X = X' * SX / PAR (PAR = 1),
        // and cover the RoD (the smallest enclosing rectangle).
        *bounds = { (int)std::floor(r[0] * s), (int)std::floor(r[1] * s), (int)std::ceil(r[2] * s), (int)std::ceil(r[3] * s) };
        const int w = bounds->x2 - bounds->x1;
        const int h = bounds->y2 - bounds->y1;
        const int rowBytes = w * 4 * (int)sizeof(float);
        out->buffer.assign((size_t)rowBytes * h, 0);
        float* data = reinterpret_cast<float*>(out->buffer.data());
        for (size_t k = 0; k < (size_t)w * h * 4; ++k) {
            data[k] = fill;
        }
        OfxPropertySetHandle ip = PH(&out->image);
        pSetPointer(ip, kOfxImagePropData, 0, data);
        int b[4] = { bounds->x1, bounds->y1, bounds->x2, bounds->y2 };
        pSetIntN(ip, kOfxImagePropBounds, 4, b);
        pSetIntN(ip, kOfxImagePropRegionOfDefinition, 4, b);
        pSetInt(ip, kOfxImagePropRowBytes, 0, rowBytes);
        pSetDoubleN(ip, kOfxImageEffectPropRenderScale, 2, rs);
        // the clip's RoD, in canonical coordinates
        pSetDoubleN(PH(&out->props), kOfxImageEffectPropRegionOfDefinition, 4, r);

        PropSet ra = makeRenderInArgs(t, 0, 0);
        pSetDoubleN(PH(&ra), kOfxImageEffectPropRenderScale, 2, rs);
        int win[4] = { renderWindow.x1, renderWindow.y1, renderWindow.x2, renderWindow.y2 };
        pSetIntN(PH(&ra), kOfxImageEffectPropRenderWindow, 4, win);

        return p->mainEntry(kOfxImageEffectActionRender, H(inst), PH(&ra), nullptr);
    }

    // a render at scale 1 of renderWindow into an output image of the given bounds, the output clip's RoD being
    // clipRoD (in canonical coordinates, PAR = 1), without getRegionOfDefinition (as a host that cached the RoD)
    OfxStatus renderWithRoD(const OfxRectI& clipRoD, const OfxRectI& bounds, const OfxRectI& renderWindow)
    {
        const OfxRectD r = { (double)clipRoD.x1, (double)clipRoD.y1, (double)clipRoD.x2, (double)clipRoD.y2 };

        return renderWithRoD(r, 1., bounds, renderWindow);
    }

    // the same, with a RoD in canonical coordinates and a PAR
    OfxStatus renderWithRoD(const OfxRectD& clipRoD, double par, const OfxRectI& bounds, const OfxRectI& renderWindow)
    {
        double rs[2] = { 1., 1. };
        const int w = bounds.x2 - bounds.x1;
        const int h = bounds.y2 - bounds.y1;
        const int rowBytes = w * 4 * (int)sizeof(float);
        out->buffer.assign((size_t)rowBytes * h, 0);
        OfxPropertySetHandle ip = PH(&out->image);
        pSetPointer(ip, kOfxImagePropData, 0, out->buffer.data());
        int b[4] = { bounds.x1, bounds.y1, bounds.x2, bounds.y2 };
        pSetIntN(ip, kOfxImagePropBounds, 4, b);
        pSetIntN(ip, kOfxImagePropRegionOfDefinition, 4, b);
        pSetInt(ip, kOfxImagePropRowBytes, 0, rowBytes);
        pSetDoubleN(ip, kOfxImageEffectPropRenderScale, 2, rs);
        double r[4] = { clipRoD.x1, clipRoD.y1, clipRoD.x2, clipRoD.y2 };
        pSetDoubleN(PH(&out->props), kOfxImageEffectPropRegionOfDefinition, 4, r);
        pSetDouble(PH(&out->props), kOfxImagePropPixelAspectRatio, 0, par);
        pSetDouble(ip, kOfxImagePropPixelAspectRatio, 0, par);
        PropSet ra = makeRenderInArgs(1., 0, 0);
        pSetDoubleN(PH(&ra), kOfxImageEffectPropRenderScale, 2, rs);
        int win[4] = { renderWindow.x1, renderWindow.y1, renderWindow.x2, renderWindow.y2 };
        pSetIntN(PH(&ra), kOfxImageEffectPropRenderWindow, 4, win);

        return p->mainEntry(kOfxImageEffectActionRender, H(inst), PH(&ra), nullptr);
    }

    // output pixel (x, y), channel c, after renderAtScale()
    float outputPixel(const OfxRectI& bounds, int x, int y, int c) const
    {
        const float* data = reinterpret_cast<const float*>(out->buffer.data());

        return data[((size_t)(y - bounds.y1) * (bounds.x2 - bounds.x1) + (x - bounds.x1)) * 4 + c];
    }

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

// 1b. getClipPreferences reads the PAR of the first file of an image sequence, looked up at the
// first frame of the sequence, when the starting time is not the first frame (issue #147).
// Before the fix, the file was looked up at the sequence time 101 (the host time of the first
// frame), which does not exist, so the output PAR was not set.
void
testClipPreferencesSequence(OfxPlugin* pDefault, const std::string& prefix)
{
    const int kFirst = 1, kLast = 5, kStartingTime = 101;
    std::printf("==== getClipPreferences(), image sequence %d..%d with PAR 2 and starting time %d (#147) ====\n", kFirst, kLast, kStartingTime);
    // prefix.par2seq.0001.tst ... prefix.par2seq.0005.tst
    auto fileAt = [prefix](double t) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), ".par2seq.%04d.tst", (int)std::floor(t + 0.5));

        return prefix + buf;
    };
    std::vector<std::string> files;
    for (int f = kFirst; f <= kLast; ++f) {
        files.push_back(fileAt(f));
        std::FILE* fp = std::fopen(files.back().c_str(), "wb");
        if (!fp) {
            check(false, "cannot create %s", files.back().c_str());

            return;
        }
        std::fclose(fp);
    }
    {
        Reader r;
        // the host gives the name of the file of each frame at the sequence (file) time, as Natron does
        if (r.create(pDefault, fileAt(kFirst), fileAt)) {
            const int first = paramInt(&r.ctx.params["firstFrame"], 0);
            const int last = paramInt(&r.ctx.params["lastFrame"], 0);
            check(first == kFirst && last == kLast, "frame range of the sequence = %d..%d (expected %d..%d)", first, last, kFirst, kLast);
            // the first frame of the sequence is at time 101
            setParamInt(r.ctx, "startingTime", kStartingTime);
            setParamInt(r.ctx, "timeOffset", kStartingTime - kFirst);
            r.plugin->frameBoundsCalls.clear();
            const double par = r.outputPAR();
            check(par == 2., "output PAR = %g (expected 2, the PAR of the first file)", par);
            const auto& calls = r.plugin->frameBoundsCalls;
            check(calls.size() == 1 && calls[0].first == fileAt(kFirst) && calls[0].second == kFirst,
                  "getFrameBounds() called once, for the first file at sequence time %d (%zu calls, first: \"%s\" at %g)",
                  kFirst, calls.size(), calls.empty() ? "" : calls[0].first.c_str(), calls.empty() ? -1. : calls[0].second);
        } else {
            ++gFailures;
        }
    }
    for (const std::string& f : files) {
        std::remove(f.c_str());
    }
}

// 1c. A PAR of 0 in the file (invalid: the host divides by the PAR) gives an output PAR of 1, not 0.
void
testClipPreferencesPar0(OfxPlugin* pDefault, const std::string& prefix)
{
    std::printf("==== getClipPreferences(), file with a PAR of 0 ====\n");
    const std::string file = prefix + ".par0.tst";
    std::FILE* fp = std::fopen(file.c_str(), "wb");
    if (!fp) {
        check(false, "cannot create %s", file.c_str());

        return;
    }
    std::fclose(fp);
    {
        Reader r;
        if (r.create(pDefault, file)) {
            const double par = r.outputPAR();
            check(par == 1., "output PAR = %g (expected 1, the file's PAR of 0 is invalid)", par);
        } else {
            ++gFailures;
        }
    }
    std::remove(file.c_str());
}

// 1d. IOUtility.h: getLevelFromScale() for an invalid scale (0 or less: std::log() gave -inf or NaN, converted to
// int), and upscalePowerOfTwo() for an OfxRectD (it raised the coordinates to the power of the level, which was NaN
// for a negative coordinate).
void
testIOUtility()
{
    std::printf("==== IOUtility: getLevelFromScale(), upscalePowerOfTwo(OfxRectD), floatToInt() ====\n");
    const struct { double s; unsigned int level; } levels[] = { { 1., 0 }, { 0.5, 1 }, { 0.25, 2 }, { 0.125, 3 }, { 0.3, 2 }, { 2., 0 }, { 0., 0 }, { -0.5, 0 } };
    for (const auto& l : levels) {
        const unsigned int level = getLevelFromScale(l.s);
        check(level == l.level, "getLevelFromScale(%g) = %u (expected %u)", l.s, level, l.level);
    }
    const OfxRectD r = { -3., -1.5, 5., 7. };
    const OfxRectD r1 = upscalePowerOfTwo(r, 1.);
    check(r1.x1 == -6. && r1.y1 == -3. && r1.x2 == 10. && r1.y2 == 14., "upscalePowerOfTwo(r, 1) = %g,%g,%g,%g (expected -6,-3,10,14)", r1.x1, r1.y1, r1.x2, r1.y2);
    const OfxRectD r3 = upscalePowerOfTwo(r, 3.);
    check(r3.x1 == -24. && r3.y1 == -12. && r3.x2 == 40. && r3.y2 == 56., "upscalePowerOfTwo(r, 3) = %g,%g,%g,%g (expected -24,-12,40,56)", r3.x1, r3.y1, r3.x2, r3.y2);
    const OfxRectD r0 = upscalePowerOfTwo(r, 0.);
    check(r0.x1 == r.x1 && r0.y1 == r.y1 && r0.x2 == r.x2 && r0.y2 == r.y2, "upscalePowerOfTwo(r, 0) = r");

    // floatToInt() (WriteFFmpeg's 8 and 16 bit conversions): a NaN pixel gives 0, without converting NaN to int
    // (undefined behaviour, found by -fsanitize=float-cast-overflow); the other values are unchanged. The NaN is
    // built from its bits in memory (see fillUnwritten()).
    float nan;
    fillUnwritten(&nan, 1);
    const int fromNaN8 = floatToInt<256>(nan);
    const int fromNaN16 = floatToInt<65536>(nan);
    check(fromNaN8 == 0 && fromNaN16 == 0, "floatToInt(NaN) = %d, %d (expected 0, 0)", fromNaN8, fromNaN16);
    const struct { float v; int i8; int i16; } values[] = {
        { -1.f, 0, 0 }, { 0.f, 0, 0 }, { 0.5f, 128, 32768 }, { 1.f, 255, 65535 }, { 2.f, 255, 65535 }, { 1.f / 255.f, 1, 257 },
    };
    for (const auto& v : values) {
        const int i8 = floatToInt<256>(v.v);
        const int i16 = floatToInt<65536>(v.v);
        check(i8 == v.i8 && i16 == v.i16, "floatToInt(%g) = %d, %d (expected %d, %d)", v.v, i8, i16, v.i8, v.i16);
    }
}

// 1e. startingTime and timeOffset stay in sync (issue #154), whoever changes startingTime. getTimeDomain and
// getClipPreferences use startingTime, render (getSequenceTime), the RoD and isIdentity use timeOffset, so the
// invariant is timeOffset == startingTime - firstFrame. Before the fix, when Natron set originalFrameRange for a new
// file, the plug-in set startingTime (and firstFrame) to its first frame but kept the old timeOffset: the time domain
// started at the first frame, but render held the first frame of the sequence.

// the sequence frame that the reader decoded for a render at time t, or -1
double
decodedFrame(Reader& r, double t)
{
    r.plugin->records.clear();
    if (!ok(r.render(t)) || (r.plugin->records.size() != 1)) {
        return -1.;
    }

    return r.plugin->records[0].time;
}

// startingTime, timeOffset and firstFrame are consistent, the time domain starts at startingTime, and the frame
// rendered at startingTime + k is the sequence frame firstFrame + k
void
checkStartingTime(Reader& r, const char* what, int expectedFirst, int expectedLast, int expectedStartingTime)
{
    const int first = paramInt(&r.ctx.params["firstFrame"], 0);
    const int last = paramInt(&r.ctx.params["lastFrame"], 0);
    const int startingTime = paramInt(&r.ctx.params["startingTime"], 0);
    const int timeOffset = paramInt(&r.ctx.params["timeOffset"], 0);
    check(first == expectedFirst && last == expectedLast && startingTime == expectedStartingTime,
          "%s: frame range %d..%d, startingTime %d (expected %d..%d, %d)", what, first, last, startingTime, expectedFirst, expectedLast, expectedStartingTime);
    check(timeOffset == startingTime - first, "%s: timeOffset %d = startingTime - firstFrame = %d", what, timeOffset, startingTime - first);
    const OfxRangeD td = r.timeDomain();
    check(td.min == startingTime && td.max == startingTime + (last - first),
          "%s: time domain %g..%g (expected %d..%d)", what, td.min, td.max, startingTime, startingTime + (last - first));
    for (int k : { 0, last - first }) {
        const double f = decodedFrame(r, startingTime + k);
        check(f == first + k, "%s: render at time %d decodes sequence frame %g (expected %d)", what, startingTime + k, f, first + k);
    }
}

void
testStartingTime(OfxPlugin* pDefault, const std::string& prefix)
{
    std::printf("==== startingTime and timeOffset stay in sync (#154) ====\n");
    // two image sequences: prefix.stA.0001.tst ... prefix.stA.0010.tst and prefix.stB.0001.tst ... prefix.stB.0004.tst
    auto fileAtA = [prefix](double t) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), ".stA.%04d.tst", (int)std::floor(t + 0.5));

        return prefix + buf;
    };
    auto fileAtB = [prefix](double t) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), ".stB.%04d.tst", (int)std::floor(t + 0.5));

        return prefix + buf;
    };
    std::vector<std::string> files;
    for (int f = 1; f <= 10; ++f) {
        files.push_back(fileAtA(f));
        if (f <= 4) {
            files.push_back(fileAtB(f));
        }
    }
    for (const std::string& f : files) {
        std::FILE* fp = std::fopen(f.c_str(), "wb");
        if (!fp) {
            check(false, "cannot create %s", f.c_str());

            return;
        }
        std::fclose(fp);
    }
    {
        Reader r;
        if (r.create(pDefault, fileAtA(1), fileAtA)) {
            checkStartingTime(r, "new reader, sequence 1..10", 1, 10, 1);

            // the user sets the starting time
            setParamInt(r.ctx, "startingTime", 101);
            check(r.changed("startingTime", kOfxChangeUserEdited), "instanceChanged(startingTime, user edit)");
            checkStartingTime(r, "user set startingTime 101", 1, 10, 101);

            // the user sets the time offset
            setParamInt(r.ctx, "timeOffset", 50);
            check(r.changed("timeOffset", kOfxChangeUserEdited), "instanceChanged(timeOffset, user edit)");
            checkStartingTime(r, "user set timeOffset 50", 1, 10, 51);

            // the user sets the first frame: the time offset is kept, the starting time follows
            setParamInt(r.ctx, "firstFrame", 3);
            check(r.changed("firstFrame", kOfxChangeUserEdited), "instanceChanged(firstFrame, user edit)");
            checkStartingTime(r, "user set firstFrame 3", 3, 10, 53);
            setParamInt(r.ctx, "firstFrame", 1);
            check(r.changed("firstFrame", kOfxChangeUserEdited), "instanceChanged(firstFrame, user edit)");
            setParamInt(r.ctx, "startingTime", 101);
            check(r.changed("startingTime", kOfxChangeUserEdited), "instanceChanged(startingTime, user edit)");
            checkStartingTime(r, "user set firstFrame 1 and startingTime 101", 1, 10, 101);

            // the user picks the sequence 1..4, as Natron does it: Natron sets originalFrameRange (reason
            // NatronInternalEdited, sent as kOfxChangeUserEdited) before instanceChanged(filename), and the
            // plug-in sets startingTime to the first frame of the new sequence
            setOverrideString(&r.ctx, kOfxImageEffectFileParamName, fileAtB(1));
            r.ctx.params[kOfxImageEffectFileParamName].stringAtTime = fileAtB;
            r.ctx.params["originalFrameRange"].hasOv = true;
            r.ctx.params["originalFrameRange"].ov.i = { 1, 4 };
            check(r.changed("originalFrameRange", kOfxChangeUserEdited), "instanceChanged(originalFrameRange, user edit)");
            check(r.changed(kOfxImageEffectFileParamName, kOfxChangeUserEdited), "instanceChanged(filename, user edit)");
            checkStartingTime(r, "new sequence 1..4 (Natron)", 1, 4, 1);
        } else {
            ++gFailures;
        }
    }
    {
        // Project load: the host restores the saved values before createInstance, then may send
        // instanceChanged(filename) with kOfxChangePluginEdited (Natron). The plug-in must not set any param
        // (this would mark the project as modified), and render must use the saved offset.
        Reader r;
        r.restore = [&](Effect& ctx) {
            auto set = [&](const char* name, std::vector<int> v) {
                ctx.params[name].hasOv = true;
                ctx.params[name].ov.i = v;
            };
            set("originalFrameRange", { 1, 10 });
            set("firstFrame", { 1 });
            set("lastFrame", { 10 });
            set("startingTime", { 101 });
            set("timeOffset", { 100 });
            set(kParamGuessedParamsName, { 1 });
        };
        r.fileChangeReason = kOfxChangePluginEdited;
        if (r.create(pDefault, fileAtA(1), fileAtA)) {
            // (non-persistent params, such as the OCIO colorspace menus, are not saved in the project)
            std::string written;
            for (const auto& kv : r.ctx.params) {
                int persistent = 1;
                pGetInt(PH(kv.second.props), kOfxParamPropPersistant, 0, &persistent);
                if ((kv.second.nSet > 0) && persistent) {
                    written += " " + kv.first;
                }
            }
            check(written.empty(), "project load: no persistent param set by the plug-in (set:%s)", written.c_str());
            checkStartingTime(r, "project load, startingTime 101", 1, 10, 101);
        } else {
            ++gFailures;
        }
    }
    for (const std::string& f : files) {
        std::remove(f.c_str());
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

// 2d. renders on several threads while the main thread switches the file param between two files of different sizes
// (#373): the OFX spec lets the host change a param during a render ("a render thread will only cause a parameter to
// lock out writes only for the duration of the call that reads the parameter, not for the duration of the whole
// render action", ofxThreadSafety.rst). render() must read the file param once: before the fix, it read it again to
// look up the proxy file, and asserted that it got the same file. Each render's output must be one of the two files.
// One instance per thread (the test host's clips hold a single image), all reading the same file param value.
void
testFileSwitch(OfxPlugin* pDefault, const std::string& still, const std::string& big)
{
    std::printf("==== file param switched by the main thread during renders ====\n");
    std::atomic<bool> useBig { false }; // the file param's value: big or still
    const auto fileAtTime = [&](double) { return useBig.load() ? big : still; };
    const int kThreads = 4;
    const int kRenders = 300; // per thread (master aborted within 20)
    std::vector<std::unique_ptr<Reader>> readers;
    for (int i = 0; i < kThreads; ++i) {
        readers.push_back(std::make_unique<Reader>());
        if (!readers.back()->create(pDefault, still, fileAtTime)) {
            ++gFailures;

            return;
        }
    }
    const OfxRectI all = { 0, 0, kBigWidth, kBigHeight }; // the RoD, image bounds and render window of each render
    const float vStill = pixelValue(1.), vBig = vStill + 1.f; // R, G and B of each file (renderWithRoD() renders time 1)
    struct Counts {
        int failed = 0; // renders that failed
        int wrong = 0; // outputs that are neither file
        int still = 0;
        int big = 0;
    };
    std::vector<Counts> counts(kThreads); // one per thread
    std::atomic<int> running { kThreads };
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            Reader& r = *readers[i];
            Counts& n = counts[i];
            for (int k = 0; k < kRenders; ++k) {
                if (!ok(r.renderWithRoD(all, all, all))) {
                    ++n.failed;
                    continue;
                }
                const bool isBig = r.outputPixel(all, 0, 0, 0) == vBig;
                const OfxRectI image = { 0, 0, isBig ? kBigWidth : kWidth, isBig ? kBigHeight : kHeight };
                bool good = true;
                for (int y = all.y1; y < all.y2 && good; ++y) {
                    for (int x = all.x1; x < all.x2 && good; ++x) {
                        const bool in = (x < image.x2) && (y < image.y2);
                        for (int c = 0; c < 4; ++c) {
                            const float expected = !in ? 0.f : ((c == 3) ? 1.f : (isBig ? vBig : vStill));
                            good = good && (r.outputPixel(all, x, y, c) == expected);
                        }
                    }
                }
                if (!good) {
                    ++n.wrong;
                } else if (isBig) {
                    ++n.big;
                } else {
                    ++n.still;
                }
            }
            --running;
        });
    }
    // the main thread switches the file param while the renders run
    while (running > 0) {
        useBig = !useBig;
        std::this_thread::yield();
    }
    for (std::thread& t : threads) {
        t.join();
    }
    int totalStill = 0, totalBig = 0;
    for (int i = 0; i < kThreads; ++i) {
        const Counts& n = counts[i];
        totalStill += n.still;
        totalBig += n.big;
        check(n.failed == 0 && n.wrong == 0, "thread %d: %d renders failed, %d outputs are neither file (%d renders of the small file, %d of the big one)",
              i, n.failed, n.wrong, n.still, n.big);
    }
    check(totalStill > 0 && totalBig > 0, "both files were rendered (%d and %d renders)", totalStill, totalBig);
}

// 2e. a proxy file (#373): the big file (16x8) with the still file (8x4) as its proxy. Setting the proxy param sets
// proxyThreshold to the proxy's scale (1/2); a render at scale 1/2 then reads the proxy, and a render at scale 1 the
// full-resolution file.
void
testProxyFile(OfxPlugin* pDefault, const std::string& still, const std::string& big)
{
    std::printf("==== proxy file, render scale 1/2 and 1 ====\n");
    Reader r;
    if (!r.create(pDefault, big)) {
        ++gFailures;

        return;
    }
    setOverrideString(&r.ctx, kOfxImageEffectProxyParamName, still);
    check(r.changed(kOfxImageEffectProxyParamName, kOfxChangeUserEdited), "instanceChanged(proxy)");
    ParamRec& threshold = r.ctx.params["proxyThreshold"];
    check(paramDouble(&threshold, 0) == 0.5 && paramDouble(&threshold, 1) == 0.5, "proxyThreshold = %g %g (0.5 0.5)",
          paramDouble(&threshold, 0), paramDouble(&threshold, 1));
    const float vStill = pixelValue(1.), vBig = vStill + 1.f;
    for (unsigned int level = 0; level <= 1; ++level) {
        const bool proxy = (level == 1);
        const OfxRectI window = { 0, 0, kBigWidth >> level, kBigHeight >> level }; // the whole image at that scale
        OfxRectD rod;
        OfxRectI bounds;
        const OfxStatus st = r.renderAtScale(1., level, window, -1.f, &rod, &bounds);
        int wrong = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const float expected = (c == 3) ? 1.f : (proxy ? vStill : vBig);
                    wrong += (r.outputPixel(bounds, x, y, c) != expected);
                }
            }
        }
        check(ok(st) && wrong == 0 && r.plugin->frameBoundsCalls.back().first == (proxy ? still : big),
              "scale 1/%d: render = %d, %d wrong values, read %s (the %s file)", 1 << level, st, wrong,
              r.plugin->frameBoundsCalls.back().first.c_str(), proxy ? "proxy" : "full-resolution");
    }
}

// 4. refreshFrameBounds() (#163): only when the part of the render window in the host's RoD is out of the frame
// bounds (the reader's cached frame bounds may be older than the file), not for a render window larger than an
// up-to-date RoD (e.g. the display window of a cropped image), which would read the file again for each render
void
testRefreshFrameBounds(OfxPlugin* pTiles, const std::string& still)
{
    std::printf("==== refreshFrameBounds(): only for a render window in the host's RoD and out of the frame bounds (#163) ====\n");
    const OfxRectI frame = { 3, 1, 32, 18 }; // the frame bounds (a cropped image)
    const OfxRectI display = { 0, 0, 40, 24 }; // a larger window (its display window)
    gMipmapContent.bounds = frame;
    gMipmapContent.value = [](int x, int y, int c) -> float { return (float)(x + 2 * y + c); };
    Reader r;
    if (!r.create(pTiles, still)) {
        ++gFailures;

        return;
    }
    r.plugin->refreshCalls = 0;
    for (int k = 0; k < 3; ++k) {
        // the host's RoD is the frame bounds, the render window the display window (or the frame bounds)
        OfxStatus st = r.renderWithRoD(frame, display, (k == 2) ? frame : display);
        check(ok(st), "render %d with the host's RoD = the frame bounds = %d", k + 1, st);
    }
    check(r.plugin->refreshCalls == 0, "render windows larger than the host's RoD: refreshFrameBounds() not called (%d calls)", r.plugin->refreshCalls);
    // the host's RoD is larger than the frame bounds (the file changed since the host got it)
    OfxStatus st = r.renderWithRoD(display, display, display);
    check(ok(st), "render with the host's RoD larger than the frame bounds = %d", st);
    check(r.plugin->refreshCalls == 1, "the host's RoD is larger than the frame bounds: refreshFrameBounds() called once (%d calls)", r.plugin->refreshCalls);
    // a render window in the host's RoD and in the frame bounds
    r.plugin->refreshCalls = 0;
    st = r.renderWithRoD(display, display, { 4, 2, 30, 17 });
    check(ok(st) && r.plugin->refreshCalls == 0, "render window in the frame bounds: refreshFrameBounds() not called (%d calls)", r.plugin->refreshCalls);
}

// The same, with a PAR of 40/33 and a width of 1920: the RoD that getRegionOfDefinition() returns, x * PAR, converted
// back to pixels (x' / PAR) is not exactly 1920 (1920.0000000000002, so 1921 pixels): an up-to-date RoD, rendered
// whole by the host, must not call refreshFrameBounds() (it read the file again for each render)
void
testRefreshFrameBoundsPar(OfxPlugin* pTiles, const std::string& prefix)
{
    std::printf("==== refreshFrameBounds(): not for an up-to-date RoD at a PAR of 40/33 ====\n");
    const std::string file = prefix + ".par40_33.tst";
    if (std::FILE* fp = std::fopen(file.c_str(), "wb")) {
        std::fclose(fp);
    }
    gMipmapContent.bounds = { 0, 0, 1920, 1080 };
    gMipmapContent.value = [](int x, int y, int c) -> float { return (float)(x + 2 * y + c); };
    {
        Reader r;
        if (!r.create(pTiles, file)) {
            ++gFailures;
            std::remove(file.c_str());

            return;
        }
        // the RoD, as the host gets it (with the PAR of the output clip, that getClipPreferences() set)
        pSetDouble(PH(&r.out->props), kOfxImagePropPixelAspectRatio, 0, 40. / 33.);
        double rs[2] = { 1., 1. };
        PropSet a;
        pSetDouble(PH(&a), kOfxPropTime, 0, 1.);
        pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, 0);
        PropSet o;
        OfxStatus st = r.p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, H(r.inst), PH(&a), PH(&o));
        double rd[4] = { 0., 0., 0., 0. };
        pGetDoubleN(PH(&o), kOfxImageEffectPropRegionOfDefinition, 4, rd);
        check(ok(st) && rd[2] == 1920. * (40. / 33.), "getRegionOfDefinition = %d, x2 = %.17g", st, rd[2]);
        const OfxRectD rod = { rd[0], rd[1], rd[2], rd[3] };
        // the host converts it to pixels (x2 = ceil(2327.2727272727275 / (40 / 33)) = 1921) for the output image
        // and the render window (a reader that supports tiles: the image may be larger than the frame bounds)
        const OfxRectI bounds = { (int)std::floor(rd[0] / (40. / 33.)), (int)std::floor(rd[1]), (int)std::ceil(rd[2] / (40. / 33.)), (int)std::ceil(rd[3]) };
        check(bounds.x2 == 1921, "the host's RoD in pixels: x2 = %d (1921)", bounds.x2);
        r.plugin->refreshCalls = 0;
        for (const OfxRectI& w : { bounds, OfxRectI{ 1900, 0, bounds.x2, 1080 }, OfxRectI{ 0, 1070, bounds.x2, 1080 } }) {
            st = r.renderWithRoD(rod, 40. / 33., bounds, w);
            check(ok(st), "render window %d,%d,%d,%d = %d", w.x1, w.y1, w.x2, w.y2, st);
        }
        check(r.plugin->refreshCalls == 0, "up-to-date RoD at a PAR of 40/33: refreshFrameBounds() not called (%d calls)", r.plugin->refreshCalls);
    }
    std::remove(file.c_str());
}

// 5. clipToFile() (#232): the part of the window out of the file is cleared, only once per pixel, whatever the depth
// and the row bytes (padded, negative), and the rest is untouched
void
testClipToFileHelper()
{
    std::printf("==== clipToFile(): float and 8-bit images, padded and negative row bytes ====\n");
    const OfxRectI bounds = { -3, 2, 14, 11 }; // the image
    const OfxRectI window = { -1, 3, 12, 10 }; // the render window, in bounds
    struct Case {
        OfxRectI file;
        bool inFile;
        OfxRectI expected;
    };
    const Case cases[] = {
        { { 2, 5, 9, 8 }, true, { 2, 5, 9, 8 } }, // in the window
        { { -10, -10, 50, 50 }, true, { -1, 3, 12, 10 } }, // larger than the window: nothing cleared
        { { 0, 0, 5, 6 }, true, { 0, 3, 5, 6 } }, // across a corner
        { { 20, 3, 30, 10 }, false, { 0, 0, 0, 0 } }, // out of the window: all cleared
        { { 3, 4, 3, 9 }, false, { 0, 0, 0, 0 } }, // empty
    };
    for (BitDepthEnum depth : { eBitDepthFloat, eBitDepthUByte }) {
        const int nComps = (depth == eBitDepthFloat) ? 4 : 3;
        const int compBytes = (depth == eBitDepthFloat) ? 4 : 1;
        const int pixelBytes = nComps * compBytes;
        const int w = bounds.x2 - bounds.x1;
        const int h = bounds.y2 - bounds.y1;
        for (int negative = 0; negative <= 1; ++negative) {
            const int stride = w * pixelBytes + 7; // padded
            for (const Case& k : cases) {
                std::vector<unsigned char> buf((size_t)stride * h, 0xab);
                // bottom-up, or top-down with negative row bytes (data points to the bottom row, the last one in memory)
                unsigned char* data = negative ? &buf[(size_t)stride * (h - 1)] : &buf[0];
                const int rowBytes = negative ? -stride : stride;
                OfxRectI got = { 1, 2, 3, 4 };
                const bool r = TestReaderPlugin::clip(window, k.file, data, bounds, nComps, depth, rowBytes, &got);
                int bad = 0;
                for (int y = bounds.y1; y < bounds.y2; ++y) {
                    const unsigned char* row = data + (ptrdiff_t)(y - bounds.y1) * rowBytes;
                    for (int x = bounds.x1; x < bounds.x2 + 1; ++x) { // (and the padding)
                        const bool inWindow = (x >= window.x1) && (x < window.x2) && (y >= window.y1) && (y < window.y2);
                        const bool inGot = (x >= got.x1) && (x < got.x2) && (y >= got.y1) && (y < got.y2);
                        const unsigned char expected = (inWindow && !inGot) ? 0 : 0xab;
                        const int n = (x < bounds.x2) ? pixelBytes : 7;
                        for (int i = 0; i < n; ++i) {
                            bad += (row[(ptrdiff_t)(x - bounds.x1) * pixelBytes + i] != expected) ? 1 : 0;
                        }
                    }
                }
                const bool sameRect = (got.x1 == k.expected.x1) && (got.y1 == k.expected.y1) && (got.x2 == k.expected.x2) && (got.y2 == k.expected.y2);
                check(r == k.inFile && sameRect && bad == 0, "%s, row bytes %d, file %d,%d,%d,%d: %d, window %d,%d,%d,%d, %d wrong bytes",
                      (depth == eBitDepthFloat) ? "float RGBA" : "8-bit RGB", rowBytes, k.file.x1, k.file.y1, k.file.x2, k.file.y2, (int)r, got.x1, got.y1, got.x2, got.y2, bad);
            }
        }
    }
}

// 6. An Opaque file rewritten smaller between getFrameBounds() and decode() (#232, the known issue of #225): alpha
// is 1 only where decode() wrote the image (the window that clipToFile() returned, or the one it reported with
// setDecodedWindow()), the rest of the render window is transparent black. Scale 1 (decode() writes in the output
// image) and 1/2 (in GenericReader's buffer).
void
testOpaqueClip(OfxPlugin* pClip, const std::string& still)
{
    const OfxRectI frame = { 0, 0, 13, 9 }; // the frame bounds, from which the host got the RoD
    const OfxRectI file = { 2, 1, 8, 6 }; // the image in the file that decode() opened
    gMipmapContent.bounds = frame;
    gMipmapContent.value = [](int x, int y, int c) -> float { return 0.1f + 0.01f * (float)(x + 16 * y + 4 * c); };
    for (int reportFrameBounds = 0; reportFrameBounds <= 1; ++reportFrameBounds) {
        std::printf("==== Opaque file rewritten smaller during a render: alpha%s (#232) ====\n",
                    reportFrameBounds ? " = 1 in the window reported by setDecodedWindow()" : " = 1 only in the file");
        gClipContent.fileBounds = file;
        gClipContent.reportFrameBounds = reportFrameBounds != 0;
        Reader r;
        if (!r.create(pClip, still)) {
            ++gFailures;

            return;
        }
        setParamInt(r.ctx, "filePremult", 0); // Opaque
        setParamInt(r.ctx, "outputPremult", 1);
        // where alpha must be 1, in full-resolution pixels
        const OfxRectI opaque = reportFrameBounds ? frame : file;
        for (unsigned int level = 0; level <= 1; ++level) {
            const int s = 1 << level;
            const OfxRectI windows[] = {
                { 0, 0, (13 + s - 1) / s, (9 + s - 1) / s }, // the RoD
                { 1 / s, 2 / s, 10 / s, 5 / s }, // across the file's bounds
                { 9 / s, 0, 13 / s, 9 / s }, // out of the file
            };
            for (const OfxRectI& w : windows) {
                OfxRectD rod;
                OfxRectI bounds;
                const float fill = -7.f;
                OfxStatus st = r.renderAtScale(1., level, w, fill, &rod, &bounds);
                int bad = 0;
                for (int y = bounds.y1; y < bounds.y2; ++y) {
                    for (int x = bounds.x1; x < bounds.x2; ++x) {
                        const bool inWindow = (x >= w.x1) && (x < w.x2) && (y >= w.y1) && (y < w.y2);
                        // the full-resolution pixels of this one
                        const int fx1 = x * s, fy1 = y * s, fx2 = fx1 + s, fy2 = fy1 + s;
                        const bool inFile = (fx1 >= file.x1) && (fy1 >= file.y1) && (fx2 <= file.x2) && (fy2 <= file.y2);
                        const bool inOpaque = (fx1 >= opaque.x1) && (fy1 >= opaque.y1) && (fx2 <= opaque.x2) && (fy2 <= opaque.y2);
                        const bool outOpaque = (fx2 <= opaque.x1) || (fy2 <= opaque.y1) || (fx1 >= opaque.x2) || (fy1 >= opaque.y2);
                        const bool outFile = (fx2 <= file.x1) || (fy2 <= file.y1) || (fx1 >= file.x2) || (fy1 >= file.y2);
                        for (int c = 0; c < 4; ++c) {
                            const float v = r.outputPixel(bounds, x, y, c);
                            bool good = true;
                            if (!inWindow) {
                                good = (v == fill);
                            } else if (c == 3) {
                                good = inOpaque ? (v == 1.f) : outOpaque ? (v == 0.f) : true;
                            } else if (outFile) {
                                good = (v == 0.f);
                            } else if (inFile && (level == 0)) {
                                good = (v == gMipmapContent.value(x, y, c));
                            }
                            if (!good) {
                                if (bad < 3) {
                                    std::printf("  pixel (%d,%d) channel %d: %g\n", x, y, c, v);
                                }
                                ++bad;
                            }
                        }
                    }
                }
                check(ok(st) && bad == 0, "scale 1/%d, window %d,%d,%d,%d: render = %d, %d wrong values", s, w.x1, w.y1, w.x2, w.y2, st, bad);
            }
        }
    }
}

// 3. downscaling by GenericReader for a reader that doesn't support render scale (issue #104)

// floor(a / 2^l) and ceil(a / 2^l)
int
floorDivPot(int a, unsigned int l)
{
    return (int)std::floor((double)a / (1 << l));
}

int
ceilDivPot(int a, unsigned int l)
{
    return (int)std::ceil((double)a / (1 << l));
}

// An RGBA image in double precision over the given bounds
struct RefImage {
    OfxRectI b;
    std::vector<double> v;

    size_t index(int x, int y, int c) const { return ((size_t)(y - b.y1) * (b.x2 - b.x1) + (x - b.x1)) * 4 + c; }
    bool contains(int x, int y) const { return b.x1 <= x && x < b.x2 && b.y1 <= y && y < b.y2; }
};

// The expected mipmap pyramid of the whole frame, computed independently of GenericReader:
// each pixel of level l is the average of those of its 2x2 children at level l-1 that exist
// (a 2x2 box filter whose box is cut by the image edges, which matters for odd sizes and odd offsets).
std::vector<RefImage>
referencePyramid(const MipmapContent& content, unsigned int levels)
{
    std::vector<RefImage> pyr(levels + 1);
    RefImage& l0 = pyr[0];
    l0.b = content.bounds;
    l0.v.assign((size_t)(l0.b.x2 - l0.b.x1) * (l0.b.y2 - l0.b.y1) * 4, 0.);
    for (int y = l0.b.y1; y < l0.b.y2; ++y) {
        for (int x = l0.b.x1; x < l0.b.x2; ++x) {
            for (int c = 0; c < 4; ++c) {
                l0.v[l0.index(x, y, c)] = content.value(x, y, c);
            }
        }
    }
    for (unsigned int l = 1; l <= levels; ++l) {
        const RefImage& prev = pyr[l - 1];
        RefImage& cur = pyr[l];
        cur.b = { floorDivPot(prev.b.x1, 1), floorDivPot(prev.b.y1, 1), ceilDivPot(prev.b.x2, 1), ceilDivPot(prev.b.y2, 1) };
        cur.v.assign((size_t)(cur.b.x2 - cur.b.x1) * (cur.b.y2 - cur.b.y1) * 4, 0.);
        for (int y = cur.b.y1; y < cur.b.y2; ++y) {
            for (int x = cur.b.x1; x < cur.b.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    double sum = 0.;
                    int n = 0;
                    for (int j = 0; j < 2; ++j) {
                        for (int i = 0; i < 2; ++i) {
                            if (prev.contains(2 * x + i, 2 * y + j)) {
                                sum += prev.v[prev.index(2 * x + i, 2 * y + j, c)];
                                ++n;
                            }
                        }
                    }
                    cur.v[cur.index(x, y, c)] = sum / n;
                }
            }
        }
    }

    return pyr;
}

bool
closeEnough(double a, double b)
{
    return std::fabs(a - b) <= 1e-5 * (std::max)(1., std::fabs(b));
}

// Renders at render scale 1/2, 1/4 and 1/8 (mipmap levels 1 to 3) with a reader that doesn't
// support render scale: GenericReader decodes the full-resolution render window, and halves it
// level times (scalePixelData). Levels >= 2 crashed before the fix of #104.
void
testMipmap(OfxPlugin* pMipmap, const std::string& still)
{
    std::printf("==== render scale 1/2, 1/4, 1/8 with a reader that doesn't support render scale (#104) ====\n");
    const unsigned int kMaxLevel = 3;
    // A gradient (R and G are linear, so that the average over a full box is the value at the
    // center of the box), a non-linear pattern (B), and constant alpha
    auto gradient = [](int x, int y, int c) -> float {
        switch (c) {
        case 0:
            return (float)x + 0.25f;
        case 1:
            return 2.f * y;
        case 2:
            return (float)(x % 3) + 0.5f * (y % 5);
        default:
            return 1.f;
        }
    };
    // constant per 8x8 block (aligned on multiples of 8), different for each channel: at levels 1 to 3,
    // every output pixel only comes from a single block, whatever the weights at the edges
    auto blocks = [](int x, int y, int c) -> float {
        return (float)(((x / 8) * 7 + (y / 8) * 13 + c * 3) % 17) + 0.5f;
    };
    struct {
        const char* name;
        std::function<float(int, int, int)> value;
        bool linearRG;
    } const contents[] = { { "gradient", gradient, true }, { "8x8 blocks", blocks, false } };
    // frame bounds in full-resolution pixels (non-negative): odd size with an odd offset,
    // odd size at the origin, and a 3x1 image (smaller than the box at the upper levels)
    const OfxRectI frames[] = { { 3, 1, 32, 18 }, { 0, 0, 13, 7 }, { 5, 3, 8, 4 } };
    const float kFill = -7.f; // the output outside the render window must not be written

    for (unsigned int level = 1; level <= kMaxLevel; ++level) {
        const int pot = 1 << level;
        for (const auto& content : contents) {
            for (const OfxRectI& frame : frames) {
                gMipmapContent.bounds = frame;
                gMipmapContent.value = content.value;
                const std::vector<RefImage> pyr = referencePyramid(gMipmapContent, kMaxLevel);
                const RefImage& ref = pyr[level];
                Reader r;
                if (!r.create(pMipmap, still)) {
                    ++gFailures;
                    continue;
                }
                // the output bounds at this scale: the RoD (the frame bounds, PAR = 1) times 1/2^level, enclosing
                const OfxRectI ob = { floorDivPot(frame.x1, level), floorDivPot(frame.y1, level), ceilDivPot(frame.x2, level), ceilDivPot(frame.y2, level) };
                // render windows: the whole image, inset by one pixel (odd offsets), and the bottom right pixel
                std::vector<OfxRectI> windows = { ob };
                if ((ob.x2 - ob.x1 > 2) && (ob.y2 - ob.y1 > 2)) {
                    windows.push_back({ ob.x1 + 1, ob.y1 + 1, ob.x2 - 1, ob.y2 - 1 });
                }
                windows.push_back({ ob.x2 - 1, ob.y1, ob.x2, ob.y1 + 1 });
                for (const OfxRectI& rw : windows) {
                    char what[256];
                    std::snprintf(what, sizeof(what), "scale 1/%d, %s, frame (%d,%d)-(%d,%d), render window (%d,%d)-(%d,%d)",
                                  pot, content.name, frame.x1, frame.y1, frame.x2, frame.y2, rw.x1, rw.y1, rw.x2, rw.y2);
                    r.plugin->records.clear();
                    OfxRectD rod = { 0., 0., 0., 0. };
                    OfxRectI bounds = { 0, 0, 0, 0 };
                    std::fflush(stdout); // in case it crashes
                    OfxStatus st = r.renderAtScale(1., level, rw, kFill, &rod, &bounds);
                    check(ok(st), "%s: render = %d", what, st);
                    if (!ok(st)) {
                        continue;
                    }
                    check(rod.x1 == frame.x1 && rod.y1 == frame.y1 && rod.x2 == frame.x2 && rod.y2 == frame.y2 && bounds.x1 == ob.x1 && bounds.y1 == ob.y1 && bounds.x2 == ob.x2 && bounds.y2 == ob.y2,
                          "%s: RoD (%g,%g)-(%g,%g) (canonical coordinates), output bounds (%d,%d)-(%d,%d)",
                          what, rod.x1, rod.y1, rod.x2, rod.y2, bounds.x1, bounds.y1, bounds.x2, bounds.y2);
                    // decode() was asked for the full-resolution pixels of the render window, at render scale 1 (its
                    // window is at full resolution, #160)
                    const OfxRectI fullRes = { (std::max)(rw.x1 * pot, frame.x1), (std::max)(rw.y1 * pot, frame.y1),
                                               (std::min)(rw.x2 * pot, frame.x2), (std::min)(rw.y2 * pot, frame.y2) };
                    const bool decodeOk = r.plugin->records.size() == 1 && r.plugin->records[0].renderScale.x == 1. && r.plugin->records[0].renderScale.y == 1.
                        && r.plugin->records[0].renderWindow.x1 == fullRes.x1 && r.plugin->records[0].renderWindow.y1 == fullRes.y1
                        && r.plugin->records[0].renderWindow.x2 == fullRes.x2 && r.plugin->records[0].renderWindow.y2 == fullRes.y2;
                    check(decodeOk, "%s: decode() called once, for the full-resolution window (%d,%d)-(%d,%d), at scale 1", what, fullRes.x1, fullRes.y1, fullRes.x2, fullRes.y2);

                    int badRef = 0, badBox = 0, badOutside = 0, nBoxes = 0;
                    for (int y = ob.y1; y < ob.y2; ++y) {
                        for (int x = ob.x1; x < ob.x2; ++x) {
                            const bool inside = rw.x1 <= x && x < rw.x2 && rw.y1 <= y && y < rw.y2;
                            // the box of full-resolution pixels averaged into this pixel is entirely inside the frame
                            const bool fullBox = frame.x1 <= x * pot && (x + 1) * pot <= frame.x2 && frame.y1 <= y * pot && (y + 1) * pot <= frame.y2;
                            if (inside && fullBox) {
                                ++nBoxes;
                            }
                            for (int c = 0; c < 4; ++c) {
                                const float v = r.outputPixel(bounds, x, y, c);
                                if (!inside) {
                                    if ((v != kFill) && (++badOutside <= 3)) {
                                        std::printf("    (%d,%d) channel %d outside the render window: %g, expected untouched %g\n", x, y, c, v, kFill);
                                    }
                                    continue;
                                }
                                // the reference pyramid
                                const double expected = ref.v[ref.index(x, y, c)];
                                if (!closeEnough(v, expected) && (++badRef <= 3)) {
                                    std::printf("    (%d,%d) channel %d: %g, expected %g\n", x, y, c, v, expected);
                                }
                                // independently of the reference pyramid: for linear R and G, a full box is the value
                                // at its center; for 8x8 blocks, every pixel is the value of its block
                                double box;
                                if (content.linearRG) {
                                    if (!fullBox || (c >= 2)) {
                                        continue;
                                    }
                                    const double center = (c == 0 ? x : y) * pot + (pot - 1) / 2.;
                                    box = (c == 0) ? center + 0.25 : 2. * center;
                                } else {
                                    box = content.value((std::max)(x * pot, frame.x1), (std::max)(y * pot, frame.y1), c);
                                }
                                if (!closeEnough(v, box) && (++badBox <= 3)) {
                                    std::printf("    (%d,%d) channel %d: %g, expected %s %g\n", x, y, c, v, content.linearRG ? "box average" : "block value", box);
                                }
                            }
                        }
                    }
                    check(badRef == 0, "%s: output = 2x2 box filter applied %u time(s) (%d mismatches)", what, level, badRef);
                    if (content.linearRG) {
                        check(badBox == 0, "%s: full boxes = average of the %dx%d box (%d boxes, %d mismatches)", what, pot, pot, nBoxes, badBox);
                    } else {
                        check(badBox == 0, "%s: output = value of the 8x8 block (%d mismatches)", what, badBox);
                    }
                    check(badOutside == 0, "%s: output outside the render window untouched (%d mismatches)", what, badOutside);
                }
            }
        }
    }
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
    const std::string big = prefix + ".big.tst";
    for (const std::string& f : { still, video, big }) {
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
    Effect descMipmap;
    OfxPlugin* pMipmap = loadPlugin(kPluginIdentifierMipmap, descMipmap);
    Effect descTiles;
    OfxPlugin* pTiles = loadPlugin(kPluginIdentifierTiles, descTiles);
    Effect descClip;
    OfxPlugin* pClip = loadPlugin(kPluginIdentifierClip, descClip);
    if (!pDefault || !pFrameVarying || !pMultiPlanar || !pMipmap || !pTiles || !pClip) {
        std::printf("FAIL: cannot load the test readers\n");

        return 1;
    }

    testFrameVarying(pDefault, pFrameVarying, still, video);
    testClipPreferencesSequence(pDefault, prefix);
    testClipPreferencesPar0(pDefault, prefix);
    testIOUtility();
    testStartingTime(pDefault, prefix);
    testRenderTime(pFrameVarying, still, video);
    testThreads(pFrameVarying, still);
    testFileSwitch(pDefault, still, big);
    testProxyFile(pDefault, still, big);
    testDecodePlane(pMultiPlanar, still, video);
    testMipmap(pMipmap, still);
    testRefreshFrameBounds(pTiles, still);
    testRefreshFrameBoundsPar(pTiles, prefix);
    testClipToFileHelper();
    testOpaqueClip(pClip, still);

    pDefault->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    pFrameVarying->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    pMultiPlanar->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    pMipmap->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    pTiles->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    pClip->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::remove(still.c_str());
    std::remove(video.c_str());
    std::remove(big.c_str());

    std::printf("Result: failures=%d\n", gFailures);
    std::printf(gFailures == 0 ? "PASS\n" : "FAIL\n");

    return gFailures == 0 ? 0 : 1;
}
