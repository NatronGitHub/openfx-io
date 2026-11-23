/*
 * oiio_edge_test — OIIOResize and OIIOText (OIIO/OIIOResize.cpp, OIIO/OIIOText.cpp) with degenerate inputs, DIRECTLY LINKED with the OFX C++
 * Support library and driven by the in-process host of ofx_test_host.h, in the Filter context:
 *
 *  - render an empty source image (0x0) into a 32x32 output, with the "default" filter: it must not crash (it
 *    segfaulted in ImageBufAlgo::resize() with OpenImageIO 2.4), and the output must be black.
 *  - getRegionOfDefinition, type Size with Preserve PAR, for a source RoD of +-DBL_MAX or +-inf: the coordinates of
 *    the source are clamped to +-5e289, so that the sizes and ratios are finite (inf / inf gave NaN, converted to int),
 *    and the RoD is the expected one; a size above INT_MAX (an infinite x and y 0..3e9) gives the same RoD as before.
 *  - the source clip changes (changedClip) with an infinite RoD (kOfxFlagInfinite*): the Size param is set to the
 *    size of the RoD clamped to the range of int (the subtraction overflowed int).
 *  - OIIOText with a position of +-DBL_MAX or 1e12: the text position is clamped before its conversion to int, and the
 *    text is out of the image: the output is the source.
 *  - OIIOText with a font that doesn't exist: ImageBufAlgo::render_text() fails, and so must the render
 *    (kOfxStatFailed, with an error message about the font); it succeeded, passing the source through without the
 *    text (#295).
 *
 * OIIOText needs one of OpenImageIO's default fonts (DroidSans, cour, Courier New or FreeMono), or a directory with
 * one of them in $OPENIMAGEIO_FONTS.
 *
 * Build it with -fsanitize=undefined,float-cast-overflow to check the conversions (see tests/README.md).
 *
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <cfloat>
#include <cmath>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int gFailures = 0;

void
check(bool cond,
      const char* fmt,
      ...)
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

OfxImageEffectHandle
H(Effect& e)
{
    return reinterpret_cast<OfxImageEffectHandle>(&e);
}

// the highest version of OIIOResize
OfxPlugin*
findResize()
{
    OfxPlugin* best = nullptr;
    int n = OfxGetNumberOfPlugins();
    for (int i = 0; i < n; ++i) {
        OfxPlugin* p = OfxGetPlugin(i);
        if (p && p->pluginIdentifier && !std::strcmp(p->pluginIdentifier, "fr.inria.openfx.OIIOResize")
            && (!best || p->pluginVersionMajor > best->pluginVersionMajor)) {
            best = p;
        }
    }

    return best;
}

// An OIIOResize instance in the Filter context
struct Resize {
    OfxPlugin* p;
    Effect ctx;
    Effect inst;
    bool created = false;

    explicit Resize(OfxPlugin* plugin)
        : p(plugin)
    {
        PropSet inArgs;
        pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
        if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, H(ctx), PH(&inArgs), nullptr))) {
            return;
        }
        inst.delegate = &ctx;
        setInstanceProps(inst, 64, 64, 1, kOfxImageEffectContextFilter);
        created = ok(p->mainEntry(kOfxActionCreateInstance, H(inst), nullptr, nullptr));
    }

    ~Resize()
    {
        if (created) {
            p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
        }
    }

    Clip* src() { return ctx.clips[kOfxImageEffectSimpleSourceClipName]; }
    Clip* dst() { return ctx.clips[kOfxImageEffectOutputClipName]; }

    void setSrcRoD(double x1, double y1, double x2, double y2)
    {
        double rod[4] = { x1, y1, x2, y2 };
        pSetDoubleN(PH(&src()->props), kOfxImageEffectPropRegionOfDefinition, 4, rod);
    }
};

void
testEmptySource(OfxPlugin* p)
{
    std::printf("==== render an empty source image with the default filter ====\n");
    Resize r(p);
    if (!r.created) {
        check(false, "CreateInstance");

        return;
    }
    const int w = 32, h = 32;
    Clip* src = r.src();
    Clip* dst = r.dst();
    src->connected = true;
    dst->connected = true;
    setupClipImage(src, 0, 0, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    setupClipImage(dst, w, h, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    float* d = reinterpret_cast<float*>(dst->buffer.data());
    for (int i = 0; i < w * h * 4; ++i) {
        d[i] = 0.5f; // not black
    }
    setOverrideInt(&r.ctx, "type", 1); // Size
    r.ctx.params["size"].hasOv = true;
    r.ctx.params["size"].ov.i = { w, h };
    gPersistentError.clear();
    PropSet a = makeRenderInArgs(1., w, h);
    OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, H(r.inst), PH(&a), nullptr);
    int notBlack = 0;
    for (int i = 0; i < w * h * 4; ++i) {
        notBlack += (d[i] != 0.f) ? 1 : 0;
    }
    check(ok(st) && notBlack == 0, "render = %d, the output is black (%d values are not)%s%s", st, notBlack,
          gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());

    // a render window that is the bottom half of the image: only the render window is written
    for (int i = 0; i < w * h * 4; ++i) {
        d[i] = 0.5f;
    }
    PropSet a2 = makeRenderInArgs(1., w, h);
    int win[4] = { 0, 0, w, h / 2 };
    pSetIntN(PH(&a2), kOfxImageEffectPropRenderWindow, 4, win);
    st = p->mainEntry(kOfxImageEffectActionRender, H(r.inst), PH(&a2), nullptr);
    int wrong = 0;
    for (int y = 0; y < h; ++y) {
        for (int i = 0; i < w * 4; ++i) {
            wrong += (d[y * w * 4 + i] != ((y < h / 2) ? 0.f : 0.5f)) ? 1 : 0;
        }
    }
    check(ok(st) && wrong == 0, "render window 0,0,%d,%d: render = %d, black in the window, untouched out of it (%d wrong)", w, h / 2, st, wrong);

    // an empty output image: nothing is rendered
    setupClipImage(dst, 0, 0, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    PropSet a3 = makeRenderInArgs(1., 0, 0);
    st = p->mainEntry(kOfxImageEffectActionRender, H(r.inst), PH(&a3), nullptr);
    check(ok(st), "empty output image: render = %d", st);
}

// a non-empty source and an empty output image (and render window): nothing is rendered (master crashed: a stack
// overflow in ImageBufAlgo::resize(), with roi.xbegin = INT_MIN)
void
testEmptyOutput(OfxPlugin* p)
{
    const struct {
        int filter; // -1: the default
        const char* label;
    } cases[] = { { -1, "default filter" }, { 0, "filter 0 (impulse)" } };
    for (const auto& c : cases) {
        std::printf("==== render a 16x16 source into an empty output image, %s ====\n", c.label);
        Resize r(p);
        if (!r.created) {
            check(false, "CreateInstance");

            return;
        }
        Clip* src = r.src();
        Clip* dst = r.dst();
        src->connected = true;
        dst->connected = true;
        setupClipImage(src, 16, 16, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        float* s = reinterpret_cast<float*>(src->buffer.data());
        for (int i = 0; i < 16 * 16 * 4; ++i) {
            s[i] = 0.5f;
        }
        setupClipImage(dst, 0, 0, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        if (c.filter >= 0) {
            setOverrideInt(&r.ctx, "filter", c.filter);
        }
        gPersistentError.clear();
        PropSet a = makeRenderInArgs(1., 0, 0);
        OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, H(r.inst), PH(&a), nullptr);
        check(ok(st) && gPersistentError.empty(), "render = %d%s%s", st, gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
    }
}

void
testPreserveParHugeRoD(OfxPlugin* p)
{
    std::printf("==== getRegionOfDefinition, Size with Preserve PAR, source RoD +-DBL_MAX ====\n");
    const struct {
        double x1, y1, x2, y2;
        double w, h; // the expected size of the RoD
        const char* label;
    } cases[] = {
        // the coordinates are clamped to +-5e289: a square, 100 x 100
        { -DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX, 100., 100., "+-DBL_MAX" },
        { -HUGE_VAL, -HUGE_VAL, HUGE_VAL, HUGE_VAL, 100., 100., "+-inf" }, // inf - inf would be NaN
        // 5e289 x 10 into 100 x 100: 100 x (10 * 100 / 5e289), 0 once converted to int
        { 0., 0., DBL_MAX, 10., 100., 0., "0,0,DBL_MAX,10" },
        // an infinite x (kOfxFlagInfinite*) and a finite y above INT_MAX: 100 x (3e9 * 100 / 2^32) = 100 x 69, as on master
        { (double)kOfxFlagInfiniteMin, 0., (double)kOfxFlagInfiniteMax, 3e9, 100., 69., "infinite x, y 0..3e9" },
        { 0., 0., 10., DBL_MAX, 0., 100., "0,0,10,DBL_MAX" },
        { 0., 0., 200., 100., 100., 50., "0,0,200,100" },
    };
    for (const auto& c : cases) {
        Resize r(p);
        if (!r.created) {
            check(false, "CreateInstance");

            return;
        }
        r.src()->connected = true;
        setupClipImage(r.src(), 8, 8, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        r.setSrcRoD(c.x1, c.y1, c.x2, c.y2);
        setOverrideInt(&r.ctx, "type", 1); // Size
        setOverrideInt(&r.ctx, "preservePAR", 1);
        r.ctx.params["size"].hasOv = true;
        r.ctx.params["size"].ov.i = { 100, 100 };
        PropSet a;
        pSetDouble(PH(&a), kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. };
        pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
        PropSet o;
        OfxStatus st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, H(r.inst), PH(&a), PH(&o));
        double rod[4] = { -1., -1., -1., -1. };
        pGetDoubleN(PH(&o), kOfxImageEffectPropRegionOfDefinition, 4, rod);
        check(st == kOfxStatOK && rod[0] == 0. && rod[1] == 0. && rod[2] == c.w && rod[3] == c.h,
              "%s: status %d, RoD %g,%g,%g,%g (expected 0,0,%g,%g)", c.label, st, rod[0], rod[1], rod[2], rod[3], c.w, c.h);
    }
}

void
testChangedClipInfiniteRoD(OfxPlugin* p)
{
    std::printf("==== changedClip, infinite source RoD: the Size param is clamped ====\n");
    Resize r(p);
    if (!r.created) {
        check(false, "CreateInstance");

        return;
    }
    r.src()->connected = true;
    setupClipImage(r.src(), 8, 8, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    r.setSrcRoD(kOfxFlagInfiniteMin, kOfxFlagInfiniteMin, kOfxFlagInfiniteMax, kOfxFlagInfiniteMax);
    PropSet a;
    pSetString(PH(&a), kOfxPropType, 0, kOfxTypeClip);
    pSetString(PH(&a), kOfxPropName, 0, kOfxImageEffectSimpleSourceClipName);
    pSetString(PH(&a), kOfxPropChangeReason, 0, kOfxChangeUserEdited);
    pSetDouble(PH(&a), kOfxPropTime, 0, 1.);
    double rs[2] = { 1., 1. };
    pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
    OfxStatus st = p->mainEntry(kOfxActionInstanceChanged, H(r.inst), PH(&a), nullptr);
    const ParamRec& size = r.ctx.params["size"];
    const int w = size.ov.i.size() > 0 ? size.ov.i[0] : -1;
    const int h = size.ov.i.size() > 1 ? size.ov.i[1] : -1;
    check(ok(st) && w == INT_MAX && h == INT_MAX, "status %d, size %d x %d (expected %d x %d)", st, w, h, INT_MAX, INT_MAX);
}

struct TextRender {
    OfxStatus st;
    int notSource; // the number of output values that are not the source (-1: not rendered)
};

// Render "Hello" with OIIOText at (x, y) with the font fontName ("": the default font), in a 16x16 RGBA float black
// source, into an output filled with 0.5.
TextRender
renderText(OfxPlugin* p,
           Effect& ctx,
           double x,
           double y,
           const char* fontName)
{
    const int w = 16, h = 16;
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, w, h, 1, kOfxImageEffectContextFilter);
    OfxStatus st = p->mainEntry(kOfxActionCreateInstance, H(inst), nullptr, nullptr);
    if (!ok(st)) {
        return { st, -1 };
    }
    Clip* src = ctx.clips[kOfxImageEffectSimpleSourceClipName];
    Clip* dst = ctx.clips[kOfxImageEffectOutputClipName];
    src->connected = true;
    dst->connected = true;
    setupClipImage(src, w, h, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    setupClipImage(dst, w, h, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    ctx.params["position"].hasOv = true;
    ctx.params["position"].ov.d = { x, y };
    setOverrideString(&ctx, "text", "Hello");
    setOverrideString(&ctx, "fontName", fontName);
    gPersistentError.clear();
    PropSet a = makeRenderInArgs(1., w, h);
    float* d = reinterpret_cast<float*>(dst->buffer.data());
    for (int i = 0; i < w * h * 4; ++i) {
        d[i] = 0.5f; // not the source
    }
    st = p->mainEntry(kOfxImageEffectActionRender, H(inst), PH(&a), nullptr);
    int notSource = 0;
    for (int i = 0; i < w * h * 4; ++i) {
        notSource += (d[i] != 0.f) ? 1 : 0;
    }
    p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);

    return { st, notSource };
}

// OIIOText, text at +-DBL_MAX: the render succeeds (the text is out of the image); a font that doesn't exist: the
// render fails, with an error message (it succeeded, and passed the source through without the text, #295)
void
testText()
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.OIIOText");
    if (!p) {
        check(false, "OIIOText not found");

        return;
    }
    Effect describeEffect, ctx;
    if (loadAndDescribe(p, describeEffect, ctx, kOfxImageEffectContextFilter) != 0) {
        check(false, "load/describe");

        return;
    }
    std::printf("==== OIIOText, position +-DBL_MAX ====\n");
    const double positions[][2] = { { DBL_MAX, -DBL_MAX }, { -DBL_MAX, DBL_MAX }, { 1e12, 1e12 } };
    for (const auto& pos : positions) {
        const TextRender r = renderText(p, ctx, pos[0], pos[1], "");
        // the text is out of the image: the output is the source (black)
        check(ok(r.st) && r.notSource == 0, "position %g,%g: render = %d, the output is the source (%d values are not)%s%s",
              pos[0], pos[1], r.st, r.notSource, gPersistentError.empty() ? "" : ", error: ", gPersistentError.c_str());
    }

    std::printf("==== OIIOText, a font that doesn't exist ====\n");
    {
        const TextRender r = renderText(p, ctx, 4., 4., "NoSuchFont-openfx-io-295");
        check(r.st == kOfxStatFailed && gPersistentError.find("font") != std::string::npos,
              "render = %d (expected %d, kOfxStatFailed), error about the font: \"%s\"", r.st, kOfxStatFailed,
              gPersistentError.c_str());
    }
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
}

} // namespace

int
main()
{
    initHost();
    OfxPlugin* p = findResize();
    if (!p) {
        std::printf("FAIL: OIIOResize not found\n");

        return 1;
    }
    Effect describeEffect;
    p->setHost(&gOfxHost);
    if (!ok(p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr)) || !ok(p->mainEntry(kOfxActionDescribe, H(describeEffect), nullptr, nullptr))) {
        std::printf("FAIL: load/describe\n");

        return 1;
    }
    testEmptySource(p);
    testEmptyOutput(p);
    testPreserveParHugeRoD(p);
    testChangedClipInfiniteRoD(p);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    testText();

    std::printf("Result: failures=%d\n", gFailures);
    std::printf(gFailures == 0 ? "PASS\n" : "FAIL\n");

    return gFailures == 0 ? 0 : 1;
}
