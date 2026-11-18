/*
 * oiio_edge_test — OIIOResize and OIIOText (OIIO/OIIOResize.cpp, OIIO/OIIOText.cpp) with degenerate inputs, DIRECTLY LINKED with the OFX C++
 * Support library and driven by the in-process host of ofx_test_host.h, in the Filter context:
 *
 *  - render an empty source image (0x0) into a 32x32 output, with the "default" filter: it must not crash (it
 *    segfaulted in ImageBufAlgo::resize()), and the output must be black.
 *  - getRegionOfDefinition, type Size with Preserve PAR, for a source RoD of +-DBL_MAX: the size is
 *    computed in double, and must be converted to int only within the range of int (it converted
 *    NaN or a huge value to int).
 *  - the source clip changes (changedClip) with an infinite RoD (kOfxFlagInfinite*): the Size param is set to the
 *    size of the RoD clamped to the range of int (the subtraction overflowed int).
 *  - OIIOText with a position of +-DBL_MAX: the text position is clamped before its conversion to int (the render may
 *    draw nothing, but must not convert an out-of-range value to int).
 *
 * Build it with -fsanitize=undefined,float-cast-overflow to check the conversions (see tests/README.md).
 *
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <cfloat>
#include <climits>
#include <cstdarg>
#include <cstdio>
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
}

void
testPreserveParHugeRoD(OfxPlugin* p)
{
    std::printf("==== getRegionOfDefinition, Size with Preserve PAR, source RoD +-DBL_MAX ====\n");
    const struct {
        double x1, y1, x2, y2;
        const char* label;
    } cases[] = {
        { -DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX, "+-DBL_MAX" },
        { 0., 0., DBL_MAX, 10., "0,0,DBL_MAX,10" },
        { 0., 0., 10., DBL_MAX, "0,0,10,DBL_MAX" },
        { 0., 0., 200., 100., "0,0,200,100" },
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
        // the RoD is 0,0,w,h with 0 <= w, h <= INT_MAX (or the default RoD)
        const bool valid = (rod[0] == 0. && rod[1] == 0. && rod[2] >= 0. && rod[2] <= (double)INT_MAX && rod[3] >= 0.
                            && rod[3] <= (double)INT_MAX);
        const bool expected = (c.x2 != 200.) || (rod[2] == 100. && rod[3] == 50.);
        check(!ok(st) || (valid && expected), "%s: status %d, RoD %g,%g,%g,%g", c.label, st, rod[0], rod[1], rod[2], rod[3]);
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

// OIIOText, text at +-DBL_MAX: the render succeeds (the text is out of the image)
void
testTextHugePosition()
{
    std::printf("==== OIIOText, position +-DBL_MAX ====\n");
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
    const int w = 16, h = 16;
    const double positions[][2] = { { DBL_MAX, -DBL_MAX }, { -DBL_MAX, DBL_MAX }, { 1e12, 1e12 } };
    for (const auto& pos : positions) {
        Effect inst;
        inst.delegate = &ctx;
        setInstanceProps(inst, w, h, 1, kOfxImageEffectContextFilter);
        if (!ok(p->mainEntry(kOfxActionCreateInstance, H(inst), nullptr, nullptr))) {
            check(false, "CreateInstance");

            return;
        }
        Clip* src = ctx.clips[kOfxImageEffectSimpleSourceClipName];
        Clip* dst = ctx.clips[kOfxImageEffectOutputClipName];
        src->connected = true;
        dst->connected = true;
        setupClipImage(src, w, h, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        setupClipImage(dst, w, h, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        ctx.params["position"].hasOv = true;
        ctx.params["position"].ov.d = { pos[0], pos[1] };
        setOverrideString(&ctx, "text", "Hello");
        gPersistentError.clear();
        PropSet a = makeRenderInArgs(1., w, h);
        OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, H(inst), PH(&a), nullptr);
        check(ok(st), "position %g,%g: render = %d%s%s", pos[0], pos[1], st, gPersistentError.empty() ? "" : ", error: ",
              gPersistentError.c_str());
        p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
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
    testPreserveParHugeRoD(p);
    testChangedClipInfiniteRoD(p);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    testTextHugePosition();

    std::printf("Result: failures=%d\n", gFailures);
    std::printf(gFailures == 0 ? "PASS\n" : "FAIL\n");

    return gFailures == 0 ? 0 : 1;
}
