/*
 * seexpr_test — unit test for the SeExpr plug-in (SeExpr/SeExpr.cpp), DIRECTLY LINKED with the OFX C++
 * Support library and driven by the in-process host of ofx_test_host.h.
 *
 *  - getFramesNeeded(): the frames fetched by cpixel() are found by an analysis of the parse tree: a frame argument
 *    made only of frame, numbers, operators, ?: and deterministic builtin functions is evaluated (whether or not the
 *    call would be evaluated: a superset); anything else (another variable, a local variable, a pixel value, an
 *    assignment to frame, an input index that is not computable) gives the input frame range param. The frames of
 *    the evaluation of the expression with NaN for the other variables, as before #176, are added (a NaN frame gives the
 *    default range, tested with OFX::IsNaN()): the frames are at least those before #176 (checked on the corpus of the
 *    review, seexpr_test_pre176.inc).
 *  - render: the coordinates of cpixel() may be out of the range of int (or infinite): they are clamped to +-1e9
 *    before their conversion to int, which gives the edge pixel, as before; the frame is clamped to the range of int,
 *    in the render and in getFramesNeeded(), since hosts convert frames to int (#184); a NaN coordinate or frame gives
 *    black.
 *  - render: the parameter variables (x1, pos1, color1) and mix have their value at the render time
 *    (paramGetValueAtTime), not at the host's current time (paramGetValue), from the first evaluation of the
 *    expression on (it returned an uninitialized value when the variable was first read at a pixel, in a branch that
 *    the evaluation before the render did not take); the RoD (Size) has bottomLeft and size at the requested time.
 *  - SeNoise with a noise size of 0 (the range of the param starts at 0) or a render scale of 0: the scale was
 *    infinite, and the noise NaN. A size of 0 or less is replaced by 0.001; a positive size (e.g. 0.0005) is used
 *    as is.
 *  - SeNoise's Reset Center sets the center and the translation between paramEditBegin and paramEditEnd (one undo
 *    step): the EditBlock was an unnamed temporary, which ended the block before the params were set. After the user
 *    moved the center, Reset Center also clears the hidden "center changed" flag in that block (#351): it was cleared
 *    after the block, so undoing a Reset Center left it false, and the next source reconnection re-centred again.
 *
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <algorithm>
#include <cstdarg>
#include <cfenv> // feclearexcept, fetestexcept
#include <pthread.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <chrono>
#include <climits> // INT_MIN, INT_MAX
#include <string>
#include <utility>
#include <vector>

namespace {

const char* const kPluginIdentifier = "fr.inria.openfx.SeExpr";
const char* const kPluginIdentifierSeNoise = "net.sf.openfx.SeNoise";
const char* const kPluginIdentifierSeGrain = "net.sf.openfx.SeGrain";
// the transform params of SeNoise (SupportExt/ofxsTransformInteract.h)
const char* const kParamTransformResetCenter = "transformResetCenter";
const char* const kParamTransformCenter = "transformCenter";
const char* const kParamTransformTranslate = "transformTranslate";
const char* const kParamTransformCenterChanged = "transformCenterChanged";

// Is *p finite (not NaN or infinite)? Tested on its bits in memory, never passed as a float value (see isUnwritten()
// in ofx_test_host.h), which works even if the compiler assumes that there are no NaNs or infinities (-ffast-math)
bool
isFiniteBits(const float* p)
{
    uint32_t bits;
    std::memcpy(&bits, p, sizeof(bits));

    return (bits & 0x7f800000u) != 0x7f800000u;
}

std::string
rangesString(const std::vector<double>& r)
{
    std::string s;
    for (size_t i = 0; i + 1 < r.size(); i += 2) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s[%g,%g]", s.empty() ? "" : " ", r[i], r[i + 1]);
        s += buf;
    }

    return s.empty() ? "none" : s;
}

// The frame ranges needed on the Source clip (input 1) at time t, as min, max pairs, sorted, without duplicates
// (the current frame may be given twice, by the RGB and the alpha scripts)
std::vector<double>
framesNeeded(OfxPlugin* p,
             Effect& ctx,
             Effect& inst,
             const char* script,
             double t)
{
    setOverrideString(&ctx, "script", script);
    PropSet a;
    pSetDouble(PH(&a), kOfxPropTime, 0, t);
    PropSet o;
    gPersistentError.clear();
    OfxStatus st = p->mainEntry(kOfxImageEffectActionGetFramesNeeded, EH(inst), PH(&a), PH(&o));
    if (!ok(st)) {
        std::printf("  getFramesNeeded(\"%s\") = %d\n", script, st);

        return std::vector<double>();
    }
    auto it = o.props.find("OfxImageClipPropFrameRange_" kOfxImageEffectSimpleSourceClipName);
    if (it == o.props.end()) {
        return std::vector<double>();
    }
    std::vector<std::pair<double, double>> ranges;
    for (size_t i = 0; i + 1 < it->second.d.size(); i += 2) {
        ranges.push_back(std::make_pair(it->second.d[i], it->second.d[i + 1]));
    }
    std::sort(ranges.begin(), ranges.end());
    ranges.erase(std::unique(ranges.begin(), ranges.end()), ranges.end());
    std::vector<double> r;
    for (const auto& range : ranges) {
        r.push_back(range.first);
        r.push_back(range.second);
    }

    return r;
}

// Does every range of expected (min, max pairs) lie in a range of r?
bool
covers(const std::vector<double>& r,
       const std::vector<double>& expected)
{
    for (size_t i = 0; i + 1 < expected.size(); i += 2) {
        bool found = false;
        for (size_t j = 0; j + 1 < r.size(); j += 2) {
            if ((r[j] <= expected[i]) && (expected[i + 1] <= r[j + 1])) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    return true;
}

// The frames needed before #176 (openfx-io bc4c45c) for the expressions where an earlier version of this change gave
// fewer frames than bc4c45c, at times 0, 10 and 3.5, with the default frame range 0,0 (frameRange 0) and -5..5
// (frameRange 1), from the corpus of the review of #176 (infinite and NaN frames left out: they are clamped, or give
// the default range). The frames needed must contain them.
struct Pre176Frames {
    const char* script;
    int frameRange;
    double time;
    std::vector<double> ranges;
};
const Pre176Frames kPre176Frames[] = {
#include "seexpr_test_pre176.inc"
};

// The cases of kPre176Frames where bc4c45c found frames only through NaN semantics (it evaluated the expression with NaN
// for the variables other than frame): the frame depends on another variable, and is unknown here (the default frame
// range is used). The frames may be fewer than bc4c45c's for these.
const struct {
    const char* script;
    const char* reason;
} kNaNArtifacts[] = {
    // fmax/fmin(NaN, v) is v in C: bc4c45c's frame was the frame alone; here it depends on x
    { "cpixel(1, frame + max(x, 0), x, y)", "max(NaN, 0) is 0; it depends on x" },
    { "cpixel(1, frame + min(x, 0), x, y)", "min(NaN, 0) is 0; it depends on x" },
    { "cpixel(1, max(x, frame), x, y)", "max(NaN, frame) is frame; it depends on x" },
    { "cpixel(1, min(x, frame), x, y)", "min(NaN, frame) is frame; it depends on x" },
    { "cpixel(1, pow(x, frame), x, y)", "pow(NaN, 0) is 1; it depends on x" },
    { "cpixel(1, clamp(frame, 5, x), x, y)", "clamp(frame, 5, NaN) is 5 at frames 0 and 3.5; it depends on x" },
};

bool
isNaNArtifact(const char* script)
{
    for (const auto& a : kNaNArtifacts) {
        if (std::strcmp(a.script, script) == 0) {
            return true;
        }
    }

    return false;
}

// Is the script nested deeper than the cap of the analysis (kStubMaxDepth, 256, in SeExpr.cpp)? Counted on its
// parentheses (a lower bound of the depth of the parse tree). Its frames are not analyzed: the default range is used
// for all the inputs, which may be fewer frames than bc4c45c's (e.g. with the frame range 0,0).
bool
isDeeperThanCap(const char* script)
{
    int depth = 0, maxDepth = 0;
    for (const char* c = script; *c; ++c) {
        depth += (*c == '(') ? 1 : ((*c == ')') ? -1 : 0);
        maxDepth = (std::max)(maxDepth, depth);
    }

    return maxDepth > 256;
}

// Render the script at time t and copy the RGBA output pixel (x, y) to px; false if the render failed
bool
renderPixel(OfxPlugin* p,
            Effect& ctx,
            Effect& inst,
            Clip* out,
            const char* script,
            double t,
            int w,
            int h,
            int x,
            int y,
            float px[4])
{
    setOverrideString(&ctx, "script", script);
    PropSet a = makeRenderInArgs(t, w, h);
    OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, EH(inst), PH(&a), nullptr);
    if (!ok(st)) {
        std::printf("  render(\"%s\") = %d\n", script, st);

        return false;
    }
    std::memcpy(px, reinterpret_cast<const float*>(out->buffer.data()) + ((size_t)y * w + x) * 4, 4 * sizeof(float));

    return true;
}

// The value of the red channel of the output pixel (x, y) after rendering the script at time t, or -1 if the
// render failed
float
renderRed(OfxPlugin* p,
          Effect& ctx,
          Effect& inst,
          Clip* out,
          const char* script,
          double t,
          int w,
          int h,
          int x,
          int y)
{
    float px[4];

    return renderPixel(p, ctx, inst, out, script, t, w, h, x, y, px) ? px[0] : -1.f;
}

// testParamsAtTime(): the params x1, pos1, color1, bottomLeft, size and mix are animated. paramGetValueAtTime(t) gives their value at t,
// paramGetValue() gives their value at the host's current time (kHostCurrentTime), which is not the render time.
const double kHostCurrentTime = 70.;
bool gAnimateMix = false; // mix is animated only for its own check, so that the other checks do not depend on it

// The value of component i of the animated param named name at time t, or false if it is not animated
bool
animatedValue(const std::string& name,
              double t,
              int i,
              double* v)
{
    if (name == "x1") {
        *v = 0.01 * t;
    } else if (name == "pos1") {
        *v = (i + 2) * 0.01 * t;
    } else if (name == "color1") {
        *v = (i == 0 ? 4 : i + 5) * 0.01 * t; // no component is the source value, 0.5
    } else if (name == "bottomLeft") {
        *v = (i + 1) * t; // (t, 2t)
    } else if (name == "size") {
        *v = (i + 3) * t; // (3t, 4t)
    } else if (gAnimateMix && name == "mix") {
        *v = (t < 40.) ? 1. : 0.; // 0 at the host's current time: the output would be the source
    } else {
        return false;
    }

    return true;
}

OfxStatus
animGetV(OfxParamHandle ph,
         double t,
         va_list ap)
{
    ParamRec* r = reinterpret_cast<ParamRec*>(ph);
    const int n = (r->type == kOfxParamTypeDouble) ? 1 : (r->type == kOfxParamTypeDouble2D) ? 2 : (r->type == kOfxParamTypeRGB) ? 3 : 0;
    double v = 0.;
    if (n == 0 || !animatedValue(r->name, t, 0, &v)) {
        return prmGetV(ph, ap); // not animated (no param here has a stringAtTime)
    }
    for (int i = 0; i < n; ++i) {
        animatedValue(r->name, t, i, &v);
        *va_arg(ap, double*) = v;
    }

    return kOfxStatOK;
}

OfxStatus
animGetValue(OfxParamHandle ph,
             ...)
{
    va_list ap;
    va_start(ap, ph);
    OfxStatus s = animGetV(ph, kHostCurrentTime, ap);
    va_end(ap);

    return s;
}

OfxStatus
animGetValueAtTime(OfxParamHandle ph,
                   OfxTime t,
                   ...)
{
    va_list ap;
    va_start(ap, t);
    OfxStatus s = animGetV(ph, t, ap);
    va_end(ap);

    return s;
}

// The parameter variables (x1, pos1, color1) and mix have their value at the render time, from the first evaluation
// of the expression on: a variable that is only read at one pixel, in a branch that the evaluation before the render
// does not take, was not initialized; and the values came from paramGetValue(), which ofxParam.h forbids in render
void
testParamsAtTime(OfxPlugin* p,
                 Effect& ctx,
                 Effect& inst,
                 Clip* out,
                 int w,
                 int h)
{
    std::printf("==== render: parameter variables and mix have their value at the render time, from the first evaluation ====\n");
    const OfxParameterSuiteV1 saved = gParamSuite;
    gParamSuite.paramGetValue = animGetValue;
    gParamSuite.paramGetValueAtTime = animGetValueAtTime;
    const struct {
        const char* script;
        int component;
        const char* param;
    } scripts[] = {
        { "x1", 0, "x1" },
        { "x == 3 && y == 3 ? x1 : 0", 0, "x1" }, // x1 is only read at (3, 3)
        { "x == 3 && y == 3 ? pos1 : 0", 0, "pos1" },
        { "x == 3 && y == 3 ? pos1 : 0", 1, "pos1" },
        { "x == 3 && y == 3 ? color1 : 0", 0, "color1" },
        { "x == 3 && y == 3 ? color1 : 0", 1, "color1" },
        { "x == 3 && y == 3 ? color1 : 0", 2, "color1" },
    };
    for (double t : { 10., 20. }) {
        for (const auto& s : scripts) {
            float px[4];
            if (!renderPixel(p, ctx, inst, out, s.script, t, w, h, 3, 3, px)) {
                check(false, "%s at %g: render failed", s.script, t);
                continue;
            }
            double expected = 0.;
            animatedValue(s.param, t, s.component, &expected);
            const float v = px[s.component];
            check(std::fabs(v - expected) < 1e-6, "%s at %g: component %d of (3, 3) = %g (expected %g, the value at %g)",
                  s.script, t, s.component, v, expected, t);
        }
    }
    gAnimateMix = true; // 1 at the render time, 0 at the host's current time (the output would be the source, 0.5)
    const float v = renderRed(p, ctx, inst, out, "0.25", 10., w, h, 3, 3);
    check(v == 0.25f, "0.25 with mix animated, at 10: %g (expected 0.25, mix = 1 at 10)", v);
    gAnimateMix = false;

    // getRegionOfDefinition() with the Region of Definition "Size": bottomLeft and size at the requested time
    setOverrideInt(&ctx, "rod", 2); // eRegionOfDefinitionOptionSize
    for (double t : { 10., 20. }) {
        PropSet a;
        pSetDouble(PH(&a), kOfxPropTime, 0, t);
        double rs[2] = { 1., 1. };
        pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
        PropSet o;
        const OfxStatus st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, EH(inst), PH(&a), PH(&o));
        double rod[4] = { 0., 0., 0., 0. };
        for (int i = 0; i < 4; ++i) {
            pGetDouble(PH(&o), kOfxImageEffectPropRegionOfDefinition, i, &rod[i]);
        }
        const double expected[4] = { t, 2 * t, t + 3 * t, 2 * t + 4 * t };
        check(ok(st) && std::equal(rod, rod + 4, expected), "RoD (Size) at %g = %d, (%g, %g, %g, %g) (expected (%g, %g, %g, %g), bottomLeft and size at %g)",
              t, st, rod[0], rod[1], rod[2], rod[3], expected[0], expected[1], expected[2], expected[3], t);
    }
    ctx.params["rod"].hasOv = false;
    gParamSuite = saved;
}

} // namespace

// Render SeNoise (or, if size < 0, SeGrain with its default params) with the given noise size and render scale, into
// *pixels. Returns the render status.
OfxStatus
renderSeNoise(OfxPlugin* p,
              double size,
              double renderScale,
              std::vector<float>* pixels)
{
    Effect describeEffect, ctx;
    PropSet inArgs;
    pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
    if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, EH(ctx), PH(&inArgs), nullptr))) {
        return kOfxStatFailed;
    }
    const int W = 8, H8 = 8;
    if (size >= 0.) {
        if (!ctx.params.count("noiseSize")) {
            std::printf("  no noiseSize param\n");

            return kOfxStatFailed;
        }
        ctx.params["noiseSize"].hasOv = true;
        ctx.params["noiseSize"].ov.d = { size, size };
    }
    Clip* src = ctx.clips[kOfxImageEffectSimpleSourceClipName];
    Clip* out = ctx.clips[kOfxImageEffectOutputClipName];
    src->connected = true;
    out->connected = true;
    setupClipImage(src, W, H8, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    setupClipImage(out, W, H8, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H8, 100, kOfxImageEffectContextFilter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, EH(inst), nullptr, nullptr))) {
        return kOfxStatFailed;
    }
    PropSet a = makeRenderInArgs(1., W, H8);
    double rs[2] = { renderScale, renderScale };
    pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
    pSetDoubleN(PH(&out->image), kOfxImageEffectPropRenderScale, 2, rs);
    pSetDoubleN(PH(&src->image), kOfxImageEffectPropRenderScale, 2, rs);
    OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, EH(inst), PH(&a), nullptr);
    const float* px = reinterpret_cast<const float*>(out->buffer.data());
    pixels->assign(px, px + (size_t)W * H8 * 4);
    p->mainEntry(kOfxActionDestroyInstance, EH(inst), nullptr, nullptr);

    return st;
}

int
countNotFinite(const std::vector<float>& pixels)
{
    int notFinite = 0;
    for (const float& v : pixels) {
        notFinite += isFiniteBits(&v) ? 0 : 1;
    }

    return notFinite;
}

// The param suite of testSeNoiseResetCenter(): it records which params the plug-in sets inside and outside an edit
// block (paramEditBegin/paramEditEnd)
int gEditDepth = 0;
std::vector<std::string> gSetInsideEdit;
std::vector<std::string> gSetOutsideEdit;

OfxStatus
recEditBegin(OfxParamSetHandle,
             const char*)
{
    ++gEditDepth;

    return kOfxStatOK;
}

OfxStatus
recEditEnd(OfxParamSetHandle)
{
    --gEditDepth;

    return kOfxStatOK;
}

OfxStatus
recSetValue(OfxParamHandle ph,
            ...)
{
    (gEditDepth > 0 ? gSetInsideEdit : gSetOutsideEdit).push_back(reinterpret_cast<ParamRec*>(ph)->name);
    va_list ap;
    va_start(ap, ph);
    OfxStatus s = prmSetV(ph, ap);
    va_end(ap);

    return s;
}

// Send kOfxActionInstanceChanged for a param changed by the user, as a host does
OfxStatus
userChangedParam(OfxPlugin* p,
                 Effect& inst,
                 const char* paramName)
{
    return instanceChanged(p, inst, paramName);
}

// SeNoise's Reset Center, after the user moved the center, sets the center and the translation, and clears the
// "center changed" flag, in one edit block, so that it is one undo step
void
testSeNoiseResetCenter(OfxPlugin* p)
{
    std::printf("==== SeNoise, Reset Center: the center, translation and center-changed flag are set in one edit block ====\n");
    Effect ctx;
    PropSet inArgs;
    pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
    if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, EH(ctx), PH(&inArgs), nullptr))) {
        check(false, "describe in context");

        return;
    }
    const int W = 8, H8 = 8;
    Clip* src = ctx.clips[kOfxImageEffectSimpleSourceClipName];
    Clip* out = ctx.clips[kOfxImageEffectOutputClipName];
    src->connected = true;
    out->connected = true;
    setupClipImage(src, W, H8, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    setupClipImage(out, W, H8, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H8, 100, kOfxImageEffectContextFilter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, EH(inst), nullptr, nullptr))) {
        check(false, "create instance");

        return;
    }
    // the user moves the center: the host sets it, then sends instanceChanged
    ParamRec* center = &ctx.params[kParamTransformCenter];
    ParamRec* centerChanged = &ctx.params[kParamTransformCenterChanged];
    prmSetValue(reinterpret_cast<OfxParamHandle>(center), 1., 2.);
    check(ok(userChangedParam(p, inst, kParamTransformCenter)), "instance changed (%s)", kParamTransformCenter);
    check(paramInt(centerChanged, 0) == 1, "moving the center sets %s", kParamTransformCenterChanged);
    const OfxParameterSuiteV1 saved = gParamSuite;
    gParamSuite.paramEditBegin = recEditBegin;
    gParamSuite.paramEditEnd = recEditEnd;
    gParamSuite.paramSetValue = recSetValue;
    gSetInsideEdit.clear();
    gSetOutsideEdit.clear();
    OfxStatus st = userChangedParam(p, inst, kParamTransformResetCenter);
    gParamSuite = saved;
    const auto has = [](const std::vector<std::string>& v, const char* name) {
        return std::find(v.begin(), v.end(), name) != v.end();
    };
    check(ok(st), "instance changed = %d", st);
    check(gEditDepth == 0, "paramEditBegin and paramEditEnd are balanced (depth %d)", gEditDepth);
    check(has(gSetInsideEdit, kParamTransformCenter) && has(gSetInsideEdit, kParamTransformTranslate),
          "%s and %s are set inside the edit block", kParamTransformCenter, kParamTransformTranslate);
    check(!has(gSetOutsideEdit, kParamTransformCenter) && !has(gSetOutsideEdit, kParamTransformTranslate),
          "%s and %s are not set outside it", kParamTransformCenter, kParamTransformTranslate);
    check(paramInt(centerChanged, 0) == 0, "Reset Center clears %s", kParamTransformCenterChanged);
    check(has(gSetInsideEdit, kParamTransformCenterChanged) && !has(gSetOutsideEdit, kParamTransformCenterChanged),
          "%s is cleared inside the edit block, so that undo restores it (#351)", kParamTransformCenterChanged);
    p->mainEntry(kOfxActionDestroyInstance, EH(inst), nullptr, nullptr);
}

// SeNoise: a noise size of 0 or a render scale of 0 gives finite values; any positive size is used as is
void
testSeNoise()
{
    OfxPlugin* p = findPlugin(kPluginIdentifierSeNoise);
    if (!p) {
        check(false, "%s not found", kPluginIdentifierSeNoise);

        return;
    }
    Effect describeEffect;
    p->setHost(&gOfxHost);
    if (!ok(p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr)) || !ok(p->mainEntry(kOfxActionDescribe, EH(describeEffect), nullptr, nullptr))) {
        check(false, "load/describe");

        return;
    }
    std::vector<float> pixels;
    std::printf("==== SeNoise, noise size 0: no NaN ====\n");
    OfxStatus st = renderSeNoise(p, 0., 1., &pixels);
    int notFinite = countNotFinite(pixels);
    check(ok(st) && notFinite == 0, "render = %d, %d values that are not finite (expected 0)", st, notFinite);
    std::printf("==== SeNoise, render scale 0: no NaN ====\n");
    st = renderSeNoise(p, 350., 0., &pixels);
    notFinite = countNotFinite(pixels);
    check(ok(st) && notFinite == 0, "render = %d, %d values that are not finite (expected 0)", st, notFinite);
    std::printf("==== SeNoise, a denormal noise size (1e-310): no NaN ====\n");
    st = renderSeNoise(p, 1e-310, 1., &pixels);
    notFinite = countNotFinite(pixels);
    check(ok(st) && notFinite == 0, "render = %d, %d values that are not finite (expected 0)", st, notFinite);
    std::printf("==== SeNoise, a positive size below 0.001 is used as is ====\n");
    std::vector<float> small, min;
    OfxStatus st1 = renderSeNoise(p, 0.0005, 1., &small);
    OfxStatus st2 = renderSeNoise(p, 0.001, 1., &min);
    check(ok(st1) && ok(st2) && small != min, "size 0.0005 and size 0.001 give different images");
    testSeNoiseResetCenter(p);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

    std::printf("==== SeGrain, render scale 0: no NaN ====\n");
    p = findPlugin(kPluginIdentifierSeGrain);
    Effect describeGrain;
    if (!p) {
        check(false, "%s not found", kPluginIdentifierSeGrain);

        return;
    }
    p->setHost(&gOfxHost);
    if (!ok(p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr)) || !ok(p->mainEntry(kOfxActionDescribe, EH(describeGrain), nullptr, nullptr))) {
        check(false, "load/describe");

        return;
    }
    st = renderSeNoise(p, -1., 0., &pixels);
    notFinite = countNotFinite(pixels);
    check(ok(st) && notFinite == 0, "render = %d, %d values that are not finite (expected 0)", st, notFinite);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
}

int
main(int /*argc*/,
     char** /*argv*/)
{
    initHost();
    OfxPlugin* p = findPlugin(kPluginIdentifier);
    if (!p) {
        std::printf("FAIL: %s not found\n", kPluginIdentifier);

        return 1;
    }
    Effect describeEffect, ctx;
    if (loadAndDescribe(p, describeEffect, ctx, kOfxImageEffectContextFilter) != 0) {
        std::printf("FAIL: load/describe\n");

        return 1;
    }
    const int W = 8, H8 = 8;
    // the default input frame range, relative to the current frame: -5..5
    ctx.params["frameRange"].hasOv = true;
    ctx.params["frameRange"].ov.i = { -5, 5 };
    setOverrideInt(&ctx, "validate", 1); // the script is only run once validated
    Clip* src = ctx.clips[kOfxImageEffectSimpleSourceClipName];
    Clip* out = ctx.clips[kOfxImageEffectOutputClipName];
    if (!src || !out) {
        std::printf("FAIL: no Source or Output clip\n");

        return 1;
    }
    src->connected = true;
    out->connected = true;
    setupClipImage(src, W, H8, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    setupClipImage(out, W, H8, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
    for (size_t i = 0; i < (size_t)W * H8 * 4; ++i) {
        reinterpret_cast<float*>(src->buffer.data())[i] = 0.5f;
    }
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H8, 100, kOfxImageEffectContextFilter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, EH(inst), nullptr, nullptr))) {
        std::printf("FAIL: CreateInstance\n");

        return 1;
    }

    std::printf("==== getFramesNeeded(): frames that depend only on the frame ====\n");
    const struct {
        const char* script;
        std::vector<double> ranges;
    } knownScripts[] = {
        { "cpixel(1, frame - 2, x, y)", { 8., 8., 10., 10. } },
        { "cpixel(1, frame, x, y) + cpixel(1, frame + 3, x, y)", { 10., 10., 13., 13. } },
        { "cpixel(1, floor(frame / 2) * 2 + 1, x, y)", { 10., 10., 11., 11. } },
        { "cpixel(1, frame + floor(abs(sin(frame)) * 2), x, y)", { 10., 10., 11., 11. } }, // a function of the frame (sin(10) = -0.544)
        { "cpixel(1, frame > 5 ? frame - 1 : frame + 1, x, y)", { 9., 9., 10., 10. } }, // ?: of the frame
        { "cpixel(1, x > 0 ? frame : frame + 1, x, y)", { 10., 10., 11., 11. } }, // ?: on x of two computable frames: both
        // local variables, with the values of their assignments (and of both branches of an if)
        { "f = frame - 2; cpixel(1, f, x, y)", { 8., 8., 10., 10. } },
        { "f = frame; f = f + 1; cpixel(1, f, x, y)", { 10., 10., 11., 11. } },
        { "frame = 3; cpixel(1, frame + 1, x, y)", { 4., 4., 10., 10. } },
        { "frame -= 2; cpixel(1, frame, x, y)", { 8., 8., 10., 10. } },
        { "o = 3; cpixel(1, frame - o, x, y)", { 7., 7., 10., 10. } },
        { "y = 2; cpixel(1, frame - y, x, y)", { 8., 8., 10., 10. } },
        { "f = frame; if (x > 0) { f = frame + 1; } cpixel(1, f, x, y)", { 10., 10., 11., 11. } },
        { "f = frame; if (x > 0) { f = frame + 9; } else { f = frame - 9; } cpixel(1, f, x, y)", { 1., 1., 10., 10., 19., 19. } },
        { "cpixel(2 - 1, frame - 3, x, y)", { 7., 7., 10., 10. } }, // an input index computed from constants
        { "cpixel(1, frame + smoothstep(frame, 0, 20), x, y)", { 10., 10., 11., 11. } }, // a builtin of the allowlist
        { "cpixel(1, frame + choose(0.9, 1, 2), x, y)", { 10., 10., 12., 12. } },
        // an input index that is one of two values (both branches of a ?: on x): the frame is added to both inputs
        { "cpixel(x > 3 ? 1 : 2, frame - 3, x, y)", { 7., 7., 10., 10. } },
        // short-circuit operators: the operand that is not evaluated does not make the frame unknown
        { "cpixel(1, frame + (1 || x) * 5, x, y)", { 10., 10., 15., 15. } },
        { "cpixel(1, frame + (0 && x) * 5, x, y)", { 10., 10. } },
        { "cpixel(1, frame + (frame > 0 || x > 0) * 3, x, y)", { 10., 10., 13., 13. } },
        { "cpixel(1, frame + (x > 0 || frame > 0), x, y)", { 10., 10., 11., 11. } }, // always true at 10
        // a comparison or a logical operator on x is 0 or 1: both frames
        { "cpixel(1, frame + (x > 0), x, y)", { 10., 10., 11., 11. } },
        { "cpixel(1, frame + (x > 0 && frame > 0), x, y)", { 10., 10., 11., 11. } },
        { "cpixel(1, frame + (x != x) * 5, x, y)", { 10., 10., 15., 15. } },
        { "cpixel(1, frame + (u < 0.5 ? -1 : 1), x, y)", { 9., 9., 10., 10., 11., 11. } },
        { "cpixel(1, frame + printf(\"%f\", x), x, y)", { 10., 10. } }, // printf() is 0 (not evaluated: no output)
        // a local variable keeps its value between pixels (0 before its first assignment)
        { "if (x > 0) { frame = 2; } cpixel(1, frame, x, y)", { 0., 0., 2., 2., 10., 10. } },
        // frame = frame + 2 at each pixel on a branch: the values do not converge; those found, and the default range
        // the builtins that convert an argument to int, evaluated with valid arguments
        { "cpixel(1, frame + pick(frame, 0, 3), x, y)", { 10., 10., 13., 13. } },
        { "cpixel(1, frame + cycle(frame, 0, 3), x, y)", { 10., 10., 12., 12. } },
        { "cpixel(1, frame + spline(0.5, 0, 1, 2, 3), x, y)", { 10., 10., 12., 12. } },
        { "cpixel(1, frame + [1, 2, 3][1], x, y)", { 10., 10., 12., 12. } }, // a vector literal, indexed
        { "cpixel(1, frame + ~0.25, x, y)", { 10., 10., 11., 11. } }, // ~0.25 = 0.75
        // a call in a branch that is not taken keeps its frames (a superset)
        { "x > 1e6 ? cpixel(1, frame + 3, x, y) : cpixel(1, frame, x, y)", { 10., 10., 13., 13. } },
        { "if (x > 1e6) { c = cpixel(1, frame + 4, x, y); } else { c = cpixel(1, frame, x, y); } c", { 10., 10., 14., 14. } },
        { "cpixel(1, sqrt(frame - 20), x, y)", { 5., 15., 10., 10. } }, // NaN: the default range, as before #176
        { "cpixel(1, frame + 2e9, x, y)", { 10., 10., 2000000010., 2000000010. } }, // an int frame is used as is
        // getVar() of a local variable (assigned before the call), and of the external frame (in the assignment of f)
        { "f = frame - 1; g = getVar(\"f\", 0) - 1; cpixel(1, g, x, y)", { 8., 8., 10., 10. } },
        { "frame = getVar(\"frame\", 0) - 4; cpixel(1, frame, x, y)", { 6., 6., 10., 10. } },
        // strings as the lexer reads them: \" ends the string if no other quote follows on the line
        { "a = printf(\"%v\\\", frame); b = frame - 2; cpixel(1, b, x, y)", { 8., 8., 10., 10. } },
        { "c = printf(\"\\\\\"); f = frame - 3; cpixel(1, f, x, y)", { 7., 7., 10., 10. } },
        { "c = printf(\"a\\\" = 1\"); f = frame - 3; cpixel(1, f, x, y)", { 7., 7., 10., 10. } }, // an escaped quote
        { "cpixel(1, frame + pvoronoi([5, 5, 5], 0.5, 0)[0] * 0, x, y)", { 10., 10. } }, // pvoronoi() without fbm
    };
    for (const auto& k : knownScripts) {
        const std::vector<double> r = framesNeeded(p, ctx, inst, k.script, 10.);
        check(r == k.ranges, "%s at 10: %s (expected %s)", k.script, rangesString(r).c_str(), rangesString(k.ranges).c_str());
    }
    std::printf("==== getFramesNeeded(): frames that depend on other values: the input frame range (-5..5) ====\n");
    const char* const unknownScripts[] = {
        "cpixel(1, frame + x, x, y)",
        "cpixel(1, frame + x - y, x, y)",
        "cpixel(1, frame + floor(u - v), x, y)",
        "cpixel(1, frame + floor(x / 1920), x, y)",
        "cpixel(1, frame + floor(log(x)), x, y)",
        "cpixel(1, frame + floor(u), x, y)",
        "cpixel(1, frame + 1 / x, x, y)",
        "cpixel(1, frame + 10 * cpixel(1, frame, x, y)[0], x, y)", // depends on a pixel value
        "cpixel(1, frame + round(clamp(u, 0, 1) * 10), x, y)", // slit-scan
        "cpixel(1, frame + round(clamp(r, 0, 1) * 4), x, y)", // time displacement by a pixel value
        "cpixel(1, frame + Cs[0] - Cs[1], x, y)", // components of a vector variable
        "cpixel(1, frame + clamp(floor(u * 3), 0, 2), x, y)",
        "cpixel(1, frame + floor(1 / x), x, y)",
        "cpixel(1, frame + floor(exp(-x)), x, y)",
        "cpixel(1, frame + floor(atan2(y, x)), x, y)",
        "cpixel(1, frame + abs(x) / x, x, y)",
        "f = frame + x; cpixel(1, f, x, y)", // a local variable that depends on x
        // local variables that depend on another variable
        // unsound with the analysis of round 3: assignments to frame, locals shadowing external variables
        "frame = frame + x; cpixel(1, frame, x, y)",
        "x = x + frame; cpixel(1, x, x, y)",
        "u = u * 10; cpixel(1, frame + u, x, y)",
        "Cs = Cs * 4; cpixel(1, frame + Cs[0], x, y)",
        "f = frame # comment = 1\n; cpixel(1, f + x, x, y)",
        "cpixel(1, frame + noise(x), x, y)", // a function that is not in the allowlist
        "cpixel(1, frame + cpixel(1, frame, x, y)[0], x, y)",
        // builtins that are not in the allowlist because they convert an argument to int or allocate a size from an
        // argument on the stack: a call in a branch that is not taken must not be evaluated (pick(1, 0, 1e9) did
        // alloca(1e9 * 8) twice: a stack overflow)
        "frame < 1e6 ? 0 : cpixel(1, frame + pick(1, 0, 1e9), x, y)",
        "frame < 1e6 ? 0 : cpixel(1, frame + pick(1, 0, 3e6), x, y)",
        "frame < 1e6 ? 0 : cpixel(1, frame + cycle(1, 0, 1e20), x, y)",
        "frame < 1e6 ? 0 : cpixel(1, frame + spline(sqrt(frame - 20), 0, 1, 2, 3), x, y)",
        "frame < 1e6 ? 0 : cpixel(1, frame + noise(frame * 1e300), x, y)",
        "frame < 1e6 ? 0 : cpixel(1, frame + hash(sqrt(frame - 20)), x, y)",
        // pick() and cycle() are evaluated for each component of a vector result (#286: pick() of [3, 3e6, 3] did
        // alloca(3e6 * 8) twice, a stack overflow)
        "frame < 1e6 ? 0 : cpixel(1, frame + pick(frame, [0, 0, 0], [3, 3e6, 3])[0], x, y)",
        // a number that is too large for a double (1e400 is infinite): not a frame (#286)
        "cpixel(1, frame + 1e400, x, y)",
    };
    for (const char* script : unknownScripts) {
        // the default range and the current frame (always fetched), and possibly the frames of the evaluation with NaN
        // for the other variables (e.g. through local variables), as before #176
        const std::vector<double> r = framesNeeded(p, ctx, inst, script, 10.);
        check(covers(r, { 5., 15., 10., 10. }), "%s at 10: %s (expected at least [5,15] [10,10])", script, rangesString(r).c_str());
    }
    std::printf("==== getFramesNeeded(): no right-hand side is evaluated to find the variable of an assignment (crashes) ====\n");
    {
        // each must not crash (pick() did alloca(16 GB), pnoise() a % 0...), print nothing, and give at least these
        const struct {
            const char* script;
            std::vector<double> ranges;
        } scripts[] = {
            { "h = 10; c = pick(u, 2, h); cpixel(1, frame - c, x, y)", { 0., 0., 4., 4., 8., 8., 10., 10. } }, // pick() is 2..10
            { "a = 0; c = pick(0.5, a, -2000000); cpixel(1, frame - 1, x, y) + c", { 9., 9., 10., 10. } },
            { "if (frame > 1e6) { a = pick(1, 0, 3e6); } cpixel(1, frame, x, y)", { 10., 10. } },
            { "b = 1; a = pick(1, 0, b * -1e-243); cpixel(1, frame + a, x, y)", { 10., 10. } },
            { "p = 0; cpixel(1, frame + pnoise([frame, 0, 0], [p, p, p]), x, y)", { 5., 15., 10., 10. } },
            { "l = -1073741824; h = 1073741824; cpixel(1, frame + cycle(frame, l, h), x, y)", { 5., 15., 10., 10. } },
            { "s = 1e300; cpixel(1, frame + noise(frame * s), x, y)", { 5., 15., 10., 10. } },
            { "if (frame > 1e6) { a = printf(\"HELLO %f\", frame); } cpixel(1, frame, x, y)", { 10., 10. } },
            { "f = frame # a comment with = and \"a = b\"\n; g = printf(\"x = 1\"); cpixel(1, f - 1 + g, x, y)", { 9., 9., 10., 10. } },
            // pvoronoi(p, jitter, fbmScale): fbm distortion of a point near 5e8 (int(floor(p)) out of range in vfbm())
            { "x > 1e6 ? cpixel(1, frame + pvoronoi([5e8, 5e8, 5e8], 0.5, 1)[0], x, y) : 0", { 5., 15., 10., 10. } },
        };
        for (const auto& c : scripts) {
            const std::vector<double> r = framesNeeded(p, ctx, inst, c.script, 10.);
            check(covers(r, c.ranges), "%s at 10: %s (expected at least %s)", c.script, rangesString(r).c_str(), rangesString(c.ranges).c_str());
        }
    }

    std::printf("==== getFramesNeeded(): a local variable whose values do not converge between pixels ====\n");
    {
        const char* script = "if (x > 3) { frame = frame - 2; } else { frame = frame + 2; } cpixel(1, frame, x, y)";
        const std::vector<double> r = framesNeeded(p, ctx, inst, script, 10.);
        check(covers(r, { 5., 15., 2., 2., 8., 8., 10., 10. }), "%s at 10: %s (expected at least [5,15] [2,2] [8,8] [10,10])", script, rangesString(r).c_str());
    }

    std::printf("==== getFramesNeeded(): NaN intermediate values ====\n");
    {
        // sqrt(-10) is NaN (true for ?: in the render): the analysis does not compute it (#286), the condition is
        // unknown, both branches: frame + 3, and the default range for frame + x
        const char* script = "cpixel(1, sqrt(frame - 20) ? frame + 3 : frame + x, x, y)";
        const std::vector<double> r = framesNeeded(p, ctx, inst, script, 10.);
        check(r == std::vector<double>({ 5., 15., 10., 10., 13., 13. }), "%s at 10: %s (expected [5,15] [10,10] [13,13])", script, rangesString(r).c_str());
        // the same through the partial evaluation
        const char* script2 = "cpixel(1, (sqrt(frame - 20) + 0 * x) ? frame + 3 : frame + x, x, y)";
        const std::vector<double> r2 = framesNeeded(p, ctx, inst, script2, 10.);
        check(covers(r2, { 5., 15., 10., 10. }) && covers(r2, { 13., 13. }), "%s at 10: %s (expected at least [5,15] [10,10], which has 13)", script2, rangesString(r2).c_str());
    }

    std::printf("==== getFramesNeeded(): no NaN or infinite value is computed, the same with -ffast-math (#286) ====\n");
    {
        // The arguments of each operator and function are checked against its domain before it is evaluated: out of it
        // (a NaN or an infinite value in SeExpr), or above 1e100, the value is unknown, which gives the default range
        // (5..15) and frame 10. No floating-point exception is raised (invalid: NaN; overflow and division by zero:
        // infinite values). The flags are evidence at -O2 only: without FENV_ACCESS (and with -fno-trapping-math, which
        // -Ofast implies) the compiler may move or drop floating-point operations; at -Ofast, the evidence is the exact
        // frames.
        const struct {
            const char* script;
            std::vector<double> ranges;
        } scripts[] = {
            // domains: in them, the value; out of them (NaN in SeExpr), unknown
            { "cpixel(1, frame + sqrt(frame - 1), x, y)", { 10., 10., 13., 13. } },
            { "cpixel(1, frame + sqrt(frame - 20), x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + acos(frame / 20), x, y)", { 10., 10., 11., 11. } }, // 11.05
            { "cpixel(1, frame + acos(frame), x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + fmod(frame, 4), x, y)", { 10., 10., 12., 12. } },
            { "cpixel(1, frame + fmod(frame, 0), x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + pow(-frame, 0.5), x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + (-frame) ^ 0.5, x, y)", { 10., 10. } }, // ^ is 0 for a negative base (SeExpr)
            { "cpixel(1, frame + noise(sqrt(frame - 20)), x, y)", { 5., 15., 10., 10. } }, // a guarded function
            // hash() converts a negative fraction to uint32_t (undefined), also for the index of pick(): unknown, and
            // pick() is one of the integers from lo to hi
            { "cpixel(1, frame + hash(-frame) * 0, x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + pick(-frame, 0, 2), x, y)", { 10., 10., 11., 11., 12., 12. } },
            { "cpixel(1, frame + ([1, 2, 3] / [1, 0, 1])[0], x, y)", { 5., 15., 10., 10. } }, // each component
            // infinite values: log(0), 0 ^ -1, overflows, a division by 0 (partial evaluation), inf - inf
            { "cpixel(1, frame + log(frame - 10), x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + 0 ^ -1, x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + exp(frame * 100), x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + exp(frame * 100) - exp(frame * 100), x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + (x > 3 ? frame : -frame) / 0, x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + (x > 3 ? exp(frame * 70) : 1) * exp(frame * 70) * 0, x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + (x > 3 ? exp(frame * 100) : 0) - exp(frame * 100), x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + gamma(frame, 0), x, y)", { 5., 15., 10., 10. } }, // pow(frame, 1 / 0)
            // ortho() = norm(cross()): the length of a cross product of large operands overflows
            { "cpixel(1, frame + ortho([1, 0, 0], [0, frame, 0])[2], x, y)", { 10., 10., 11., 11. } },
            { "cpixel(1, frame + ortho([frame * 1e98, 0, 0], [0, 1e99, 0])[2], x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + norm(cross([frame * 1e98, 0, 0], [0, 1e99, 0]))[2], x, y)", { 5., 15., 10., 10. } },
            // curve() and ccurve() with constant control points at distinct integer positions; otherwise unknown
            { "cpixel(1, curve(frame, 0, 0, 1, 100, 50, 1), x, y)", { 5., 5., 10., 10. } },
            { "cpixel(1, frame + ccurve(frame, 0, [0, 0, 0], 1, 100, [50, 50, 50], 1)[0], x, y)", { 10., 10., 15., 15. } },
            { "cpixel(1, curve(frame, 5, 1, 4, 5, 2, 4), x, y)", { 5., 15., 10., 10. } }, // coincident positions
            { "cpixel(1, curve(frame, 0, 0, 1, 99.5, 50, 1), x, y)", { 5., 15., 10., 10. } }, // not an integer
            { "f = 100; cpixel(1, curve(frame, 0, 0, 1, f, 50, 1), x, y)", { 5., 15., 10., 10. } }, // read at the prep
            { "cpixel(1, curve(frame * 1e40, 0, 0, 1, 100, 50, 1), x, y)", { 5., 15., 10., 10. } }, // a large parameter
            // functions whose domain can not be checked from their arguments: not analyzed
            { "cpixel(1, frame + angle([1, 0, 0], [0, 1, 0]), x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + up([1, 0, 0], [0, 1, 0])[0], x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + hsi([1, 0.5, 0.2], 0, 1, 1)[0], x, y)", { 5., 15., 10., 10. } },
            { "cpixel(1, frame + midhsi([1, 0.5, 0.2], 0, 1, 1, 0.5)[0], x, y)", { 5., 15., 10., 10. } },
            // a ?: evaluates only the selected branch (as SeExpr), && and || their second operand only if needed
            { "cpixel(1, frame > 20 ? sqrt(frame - 20) : sin(frame) + frame, x, y)", { 9., 9., 10., 10. } }, // 9.46
            { "cpixel(1, frame + (frame > 20 && sqrt(frame - 20) > 1) * 5, x, y)", { 10., 10. } },
            { "cpixel(1, frame + (frame < 20 || log(frame - 10) > 1) * 5, x, y)", { 10., 10., 15., 15. } },
        };
        for (const auto& c : scripts) {
            std::feclearexcept(FE_ALL_EXCEPT);
            const std::vector<double> r = framesNeeded(p, ctx, inst, c.script, 10.);
            const int raised = std::fetestexcept(FE_INVALID | FE_OVERFLOW | FE_DIVBYZERO);
            check((r == c.ranges) && (raised == 0), "%s at 10: %s (expected %s)%s%s%s", c.script, rangesString(r).c_str(), rangesString(c.ranges).c_str(),
                  (raised & FE_INVALID) ? ", raised FE_INVALID (a NaN)" : "", (raised & FE_OVERFLOW) ? ", raised FE_OVERFLOW" : "",
                  (raised & FE_DIVBYZERO) ? ", raised FE_DIVBYZERO" : "");
        }
        // a curve retime (monotone spline) at its last control point, frame 100: frame 50 (as Natron 2.5.0)
        const char* script = "cpixel(1, curve(frame, 1, 1, 4, 100, 50, 4), x, y)";
        const std::vector<double> r = framesNeeded(p, ctx, inst, script, 100.);
        check(r == std::vector<double>({ 50., 50., 100., 100. }), "%s at 100: %s (expected [50,50] [100,100])", script, rangesString(r).c_str());
    }

    std::printf("==== getFramesNeeded(): frames that depend only on the frame, through partial evaluation ====\n");
    {
        const struct {
            const char* script;
            std::vector<double> ranges;
        } scripts[] = {
            { "cpixel(1, frame + fbm(frame), x, y)", { 10., 10., 11., 11. } },
            { "cpixel(1, [frame, x, y][0], x, y)", { 10., 10. } },
            { "cpixel(1, frame + pow(x, 0), x, y)", { 10., 10., 11., 11. } },
            { "cpixel(1, frame + contrast(x, 0), x, y)", { 10., 10., 11., 11. } }, // 10.5, rounded
            { "cpixel(1, getVar(\"frame\", 0) - 1, x, y)", { 9., 9., 10., 10. } },
            { "cpixel(1, floor(x > 3 ? frame - 3 : frame + 3), x, y)", { 7., 7., 10., 10., 13., 13. } },
            { "cpixel(1, frame + (x > 3 ? [1, 2, 3] : [4, 5, 6])[1], x, y)", { 10., 10., 12., 12., 15., 15. } },
            // values that are not all known: the known ones, and the default range
            { "cpixel(1, x > 3 ? log(0) : frame - 1, x, y)", { 5., 15., 9., 9., 10., 10. } },
            { "cpixel(1, frame + boxstep(x, 0), x, y)", { 10., 10., 11., 11. } }, // boxstep() is 0 or 1
            { "cpixel(1, [frame - 1, frame - 2, frame - 3][x], x, y)", { 0., 0., 7., 7., 8., 8., 9., 9., 10., 10. } }, // or 0
            { "a = [1, 2, 3]; b = a * (x > 3); cpixel(1, frame - b[2], x, y)", { 7., 7., 10., 10. } },
            { "cpixel(1, frame + ((x > 3 ? [0, 1, 0] : [1, 0, 0]) * 2)[1], x, y)", { 10., 10., 12., 12. } },
        };
        for (const auto& c : scripts) {
            const std::vector<double> r = framesNeeded(p, ctx, inst, c.script, 10.);
            check(r == c.ranges, "%s at 10: %s (expected %s)", c.script, rangesString(r).c_str(), rangesString(c.ranges).c_str());
        }
    }

    std::printf("==== getFramesNeeded(): a large expression (1000 if blocks, 1000 locals) is not analyzed ====\n");
    {
        std::string script;
        for (int i = 0; i < 1000; ++i) {
            script += "a" + std::to_string(i) + " = frame; if (x > " + std::to_string(i) + ") { a" + std::to_string(i) + " = frame + 1; }\n";
        }
        script += "cpixel(1, a999, x, y)";
        const auto start = std::chrono::steady_clock::now();
        const std::vector<double> r = framesNeeded(p, ctx, inst, script.c_str(), 10.);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        check(covers(r, { 5., 15., 10., 10. }) && (seconds < 2.), "%s... at 10: %s in %.2f s (expected at least [5,15] [10,10], in less than 2 s)", script.substr(0, 40).c_str(), rangesString(r).c_str(), seconds);
    }

    std::printf("==== getFramesNeeded(): a deep expression (20000 terms) is not analyzed (the stack) ====\n");
    {
        std::string script = "cpixel(1, x";
        for (int i = 0; i < 20000; ++i) {
            script += " + 1";
        }
        script += ", x, y)";
        const auto start = std::chrono::steady_clock::now();
        const std::vector<double> r = framesNeeded(p, ctx, inst, script.c_str(), 10.);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        check(covers(r, { 5., 15., 10., 10. }) && (seconds < 2.), "cpixel(1, x + 1 + 1... (20000 terms) at 10: %s in %.2f s (expected at least [5,15] [10,10], in less than 2 s)", rangesString(r).c_str(), seconds);
    }
    std::printf("==== getFramesNeeded(): the depth of the parse tree is capped (256), on a thread with a 512 KB stack ====\n");
    {
        // frame + 0 + 0...: a depth of terms + 2 or so; below the cap, the frame is found
        struct DeepCase {
            OfxPlugin* p;
            Effect* ctx;
            Effect* inst;
            int terms;
            std::vector<double> r;
        };
        DeepCase cases[2] = { { p, &ctx, &inst, 240, {} }, { p, &ctx, &inst, 300, {} } };
        for (DeepCase& c : cases) {
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 512 * 1024);
            pthread_t thread;
            const int err = pthread_create(&thread, &attr, [](void* arg) -> void* {
                DeepCase* d = static_cast<DeepCase*>(arg);
                std::string script = "cpixel(1, frame";
                for (int i = 0; i < d->terms; ++i) {
                    script += " + 0";
                }
                script += " - 2, x, y)";
                d->r = framesNeeded(d->p, *d->ctx, *d->inst, script.c_str(), 10.);

                return nullptr;
            }, &c);
            pthread_attr_destroy(&attr);
            check(err == 0, "pthread_create = %d", err);
            if (err == 0) {
                pthread_join(thread, nullptr);
            }
        }
        check(cases[0].r == std::vector<double>({ 8., 8., 10., 10. }), "cpixel(1, frame + 0 + 0... - 2 (240 terms) at 10: %s (expected [8,8] [10,10])", rangesString(cases[0].r).c_str());
        check(covers(cases[1].r, { 5., 15., 10., 10. }), "cpixel(1, frame + 0 + 0... - 2 (300 terms) at 10: %s (expected at least [5,15] [10,10]: deeper than the cap)", rangesString(cases[1].r).c_str());
    }

    std::printf("==== getFramesNeeded(): the work of the whole evaluations is capped (kStubMaxEvalWork, 5e6 nodes) (#286) ====\n");
    {
        // floor(sin(sin(...(a + b)))) with a and b of 32 values each (1024 combinations): each node is evaluated by
        // SeExpr after its children are checked, so the work is the sum of the subtree sizes, times 1024. With 90
        // nested functions (4.6e6 nodes) the frames are exact; with 100 (5.6e6), the analysis stops: the default range
        // and the current frame (the budget is spent before the call is recorded). Without the cap, 100 is exact too.
        struct ChainCase {
            int depth;
            bool overBudget;
        } cases[] = { { 90, false }, { 100, true } };
        for (const ChainCase& c : cases) {
            std::string e = "a + b";
            for (int i = 0; i < c.depth; ++i) {
                e = "sin(" + e + ")";
            }
            const std::string script = "a = pick(u, 0, 31); b = pick(v, 0, 31); cpixel(1, floor(" + e + "), x, y)";
            const auto start = std::chrono::steady_clock::now();
            const std::vector<double> r = framesNeeded(p, ctx, inst, script.c_str(), 10.);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const std::vector<double> exact({ -1., -1., 0., 0., 10., 10. }); // floor(sin(...)) is -1 or 0
            const bool ok = (r == (c.overBudget ? std::vector<double>({ 5., 15., 10., 10. }) : exact));
            check(ok && (seconds < 2.), "floor(sin(sin(... (%d nested) ...(a + b)))) at 10: %s in %.2f s (expected %s, in less than 2 s)", c.depth,
                  rangesString(r).c_str(), seconds, c.overBudget ? "[5,15] [10,10]: over the budget" : "[-1,-1] [0,0] [10,10]: under the budget");
        }
    }

    std::printf("==== getFramesNeeded(): unknown and known frames of the same input: the default range and the known frames ====\n");
    {
        const char* script = "cpixel(1, frame + x, x, y) + cpixel(1, frame + 7, x, y)";
        const std::vector<double> r = framesNeeded(p, ctx, inst, script, 10.);
        check(r == std::vector<double>({ 5., 15., 10., 10., 17., 17. }), "%s at 10: %s (expected [5,15] [10,10] [17,17])", script, rangesString(r).c_str());
    }

    std::printf("==== getFramesNeeded(): at least the frames before #176 ====\n");
    {
        int notCovered = 0;
        int skipped = 0;
        int artifacts = 0;
        int deep = 0;
        for (const Pre176Frames& m : kPre176Frames) {
            ctx.params["frameRange"].ov.i = (m.frameRange == 0) ? std::vector<int>({ 0, 0 }) : std::vector<int>({ -5, 5 });
            const std::vector<double> r = framesNeeded(p, ctx, inst, m.script, m.time);
            if (r.empty() && ((gPersistentError.find("Prep errors") != std::string::npos) || (gPersistentError.find("Syntax error") != std::string::npos))) {
                // the expression does not parse or prep with this build of SeExpr (e.g. no cbrt() in MSYS2's, as
                // before #176): only these are skipped
                ++skipped;
                continue;
            }
            if (!covers(r, m.ranges) && isNaNArtifact(m.script)) {
                ++artifacts;
                continue;
            }
            if (!covers(r, m.ranges) && isDeeperThanCap(m.script) && covers(r, { m.time, m.time })) {
                ++deep;
                continue;
            }
            if (!covers(r, m.ranges)) {
                ++notCovered;
                std::printf("  %s (frame range %s) at %g: %s, before #176 %s\n", m.script, (m.frameRange == 0) ? "0,0" : "-5,5", m.time,
                            rangesString(r).c_str(), rangesString(m.ranges).c_str());
            }
        }
        ctx.params["frameRange"].ov.i = { -5, 5 };
        const int n = (int)(sizeof(kPre176Frames) / sizeof(kPre176Frames[0]));
        check(notCovered == 0, "%d of %d cases have at least the frames before #176, %d are documented NaN artifacts, %d are deeper than the cap of the analysis (the default range) (%d skipped: not supported by this SeExpr)",
              n - skipped - notCovered - artifacts - deep, n - skipped, artifacts, deep, skipped);
    }

    std::printf("==== getFramesNeeded(): an input index that is not computable: all the inputs use the default range and the computed frames ====\n");
    {
        const char* script = "cpixel(1 + 0 * x, frame - 3, x, y)";
        const std::vector<double> r = framesNeeded(p, ctx, inst, script, 10.);
        check(r == std::vector<double>({ 5., 15., 7., 7., 10., 10. }), "%s at 10: %s (expected [5,15] [7,7] [10,10])", script, rangesString(r).c_str());
    }

    std::printf("==== getFramesNeeded(): frames out of the range of int are clamped to it, as hosts convert frames to int (#184) ====\n");
    {
        const struct {
            const char* script;
            double frame; // the clamped frame
        } cases[] = {
            { "cpixel(1, frame + 1e12, x, y)", (double)INT_MAX },
            { "cpixel(1, frame - 1e12, x, y)", (double)INT_MIN },
        };
        for (const auto& c : cases) {
            const std::vector<double> r = framesNeeded(p, ctx, inst, c.script, 10.);
            const bool inRange = std::all_of(r.begin(), r.end(), [](double f) { return ((double)INT_MIN <= f) && (f <= (double)INT_MAX); });
            check(inRange && covers(r, { c.frame, c.frame }), "%s at 10: %s (expected [%.17g,%.17g], and no frame out of the range of int)", c.script, rangesString(r).c_str(), c.frame, c.frame);
        }
    }

    std::printf("==== render: cpixel() coordinates far out of the image give the edge pixel, NaN gives black ====\n");
    {
        const char* const nanScripts[] = { "cpixel(1, frame, sqrt(x - 100), y)", "cpixel(1, sqrt(frame - 20), x, y)" };
        for (const char* script : nanScripts) {
            const float v = renderRed(p, ctx, inst, out, script, 10., W, H8, 3, 3);
            check(v == 0.f, "%s: %g (expected 0, a NaN argument)", script, v);
        }
    }
    const char* const renderScripts[] = {
        "cpixel(1, frame, x, y)",
        "cpixel(1, frame, x + 1e12, y)",
        "cpixel(1, frame, x, y + 1e9)",
        "cpixel(1, frame, x - 1e300, y)",
        "cpixel(1, frame + 1e12, x, y)", // the frame is clamped to INT_MAX (the test host has every frame)
        "cpixel(1, frame + 1e400, x, y)", // an infinite frame too
    };
    for (const char* script : renderScripts) {
        const float v = renderRed(p, ctx, inst, out, script, 10., W, H8, 3, 3);
        check(v == 0.5f, "%s: %g (expected 0.5, the source pixel or the edge pixel)", script, v);
    }

    testParamsAtTime(p, ctx, inst, out, W, H8);

    p->mainEntry(kOfxActionDestroyInstance, EH(inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

    testSeNoise();

    std::printf("Result: failures=%d\n", gFailures);
    std::printf(gFailures == 0 ? "PASS\n" : "FAIL\n");

    return gFailures == 0 ? 0 : 1;
}
