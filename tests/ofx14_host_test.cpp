/*
 * ofx14_host_test — every plug-in of openfx-io (the sources of IO.ofx), DIRECTLY LINKED with the OFX C++ Support
 * library, in the in-process host of ofx_test_host.h, by default in its OFX 1.4-only mode (gOfx14Only): a host that
 * supports OFX 1.4 and nothing newer, and no extension. In that host, fetchSuite returns NULL for every suite newer
 * than OFX 1.4 or not mandatory (OpenGL render, DrawSuite, MultiThread V2, the Nuke multi-plane suite...), any
 * property that is not defined in the OFX 1.4 headers is unknown (kOfxStatErrUnknown), and the plug-ins get the
 * host from OfxPlugin::setHost (OfxSetHost, OFX 1.5, is never called).
 *
 * Every plug-in must describe, instantiate and render there, with correct output (the newer features of OFX and
 * the extensions are optional extras). For each plug-in, in this order: Load, Describe (and the overlay interact's
 * Describe, if it has one: it must be an OverlayInteractV1, the only one in OFX 1.4), DescribeInContext in each of
 * its contexts, then in the filter context (filters) or the general context (readers and writers): CreateInstance
 * (and the overlay's), GetClipPreferences, GetTimeDomain, GetRegionOfDefinition, GetRegionsOfInterest,
 * GetFramesNeeded, IsIdentity, BeginSequenceRender, Render of one frame, EndSequenceRender, DestroyInstance, Unload.
 * Each action must succeed, the plug-in must post no error message, and:
 *   - a filter or reader must write its whole output image;
 *   - a writer writes a file (in the work directory), which the readers then read: WritePNG -> ReadPNG and ReadOIIO,
 *     WriteEXR -> ReadEXR, WritePFM -> ReadPFM, WriteOIIO (TIFF) -> ReadOIIO, WriteFFmpeg (MOV) -> ReadFFmpeg;
 *   - a reader's image must be the writer's input (a constant colour), within a tolerance;
 *   - the filters that are an identity with their default parameters (OCIOColorSpace, OCIOCDLTransform,
 *     OCIOFileTransform with an identity LUT, with the identity colour spaces of test.ocio) must output their input.
 * Every plug-in of the binary must have a test below: a new plug-in without one fails.
 *
 * Usage: ofx14_host_test [--full] [--only <plug-in identifier>] <work directory>
 *   --full: the same tests in the full host (the default mode of ofx_test_host.h), for comparison. In that mode, a
 *           plug-in that registers an OverlayInteractV2 (DrawSuite) must also register an OverlayInteractV1.
 *   --only: test only this plug-in (its input file, for a reader, must have been written by an earlier run).
 * Needs $OCIO (tests/test.ocio).
 *
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h> // _mkdir
#endif
#include <utility>
#include <vector>

namespace {

const int kW = 32;
const int kH = 24;
// the colour of the source images (opaque)
const float kColour[4] = { 0.25f, 0.5f, 0.75f, 1.f };

enum Kind { eWriter, eReader, eFilter };

struct Spec {
    const char* id;
    Kind kind;
    std::string file;                                         // writer: written, reader: read (in the work dir)
    double tol;                                               // reader: tolerance of the image's mean; filter: if
                                                              // >= 0, the output must be the input (identity)
    std::vector<std::pair<std::string, std::string>> strings; // param values (a "@" is the work directory)
    std::vector<std::pair<std::string, int>> ints;
};

int gFailures = 0;
std::string gWork;

void
fail(const char* fmt,
     ...)
{
    va_list a;
    va_start(a, fmt);
    std::printf("  FAIL: ");
    std::vprintf(fmt, a);
    std::printf("\n");
    va_end(a);
    ++gFailures;
}

OfxImageEffectHandle
EH(Effect& e)
{
    return reinterpret_cast<OfxImageEffectHandle>(&e);
}

long
fileSize(const std::string& f)
{
    struct stat st;
    return (stat(f.c_str(), &st) == 0) ? (long)st.st_size : -1;
}

std::string
expand(const std::string& v)
{
    return (!v.empty() && v[0] == '@') ? gWork + v.substr(1) : v;
}

int
nComps(const std::string& comps)
{
    return comps == kOfxImageComponentRGBA ? 4 : (comps == kOfxImageComponentAlpha ? 1 : 3);
}

// The image of a clip: bounds b (x1, y1, x2, y2), components comps, float.
void
setupImage(Clip* c,
           const int b[4],
           const std::string& comps)
{
    const int w = b[2] - b[0], h = b[3] - b[1];
    setupClipImage(c, w, h, comps.c_str(), kOfxBitDepthFloat, true);
    pSetIntN(PH(&c->image), kOfxImagePropBounds, 4, b);
    pSetIntN(PH(&c->image), kOfxImagePropRegionOfDefinition, 4, b);
    double rod[4] = { (double)b[0], (double)b[1], (double)b[2], (double)b[3] };
    pSetDoubleN(PH(&c->props), kOfxImageEffectPropRegionOfDefinition, 4, rod);
    double fr[2] = { 1., 1. };
    pSetDoubleN(PH(&c->props), kOfxImageEffectPropFrameRange, 2, fr);
}

void
fillColour(Clip* c,
           const std::string& comps)
{
    const int n = nComps(comps);
    float* p = reinterpret_cast<float*>(c->buffer.data());
    const size_t npix = c->buffer.size() / (sizeof(float) * n);
    for (size_t i = 0; i < npix; ++i) {
        for (int k = 0; k < n; ++k) {
            p[i * n + k] = (n == 1) ? kColour[3] : kColour[k];
        }
    }
}

std::string
clipPref(PropSet& outArgs,
         const char* prefix,
         const std::string& clip,
         const std::string& def)
{
    const char* v = nullptr;
    if ((pGetString(PH(&outArgs), (std::string(prefix) + "_" + clip).c_str(), 0, &v) == kOfxStatOK) && v && *v) {
        return v;
    }
    return def;
}

PropSet
sequenceArgs(double t)
{
    PropSet a;
    OfxPropertySetHandle ap = PH(&a);
    double fr[2] = { t, t };
    pSetDoubleN(ap, kOfxImageEffectPropFrameRange, 2, fr);
    pSetDouble(ap, kOfxImageEffectPropFrameStep, 0, 1.);
    pSetInt(ap, kOfxPropIsInteractive, 0, 0);
    double rs[2] = { 1., 1. };
    pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
    pSetInt(ap, kOfxImageEffectPropSequentialRenderStatus, 0, 1);
    pSetInt(ap, kOfxImageEffectPropInteractiveRenderStatus, 0, 0);
    return a;
}

PropSet
timeArgs(double t)
{
    PropSet a;
    pSetDouble(PH(&a), kOfxPropTime, 0, t);
    double rs[2] = { 1., 1. };
    pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
    return a;
}

// "error messages" that the plug-in posted since the last call
bool
errorMessages(std::string* what)
{
    std::lock_guard<std::mutex> lock(gMessagesMutex);
    bool err = false;
    size_t pos = 0;
    while (pos < gMessages.size()) {
        size_t e = gMessages.find('\n', pos);
        if (e == std::string::npos) e = gMessages.size();
        std::string line = gMessages.substr(pos, e - pos);
        if (!line.compare(0, std::strlen(kOfxMessageError), kOfxMessageError) ||
            !line.compare(0, std::strlen(kOfxMessageFatal), kOfxMessageFatal)) {
            err = true;
            *what += line + "; ";
        }
        pos = e + 1;
    }
    gMessages.clear();
    gPersistentError.clear();

    return err;
}

// The overlay interact: Describe it, and if inst is not NULL, create an instance for it and destroy it.
void
testOverlay(OfxPluginEntryPoint* entry,
            Effect* inst)
{
    PropSet ip;
    OfxStatus st = entry(kOfxActionDescribe, &ip, nullptr, nullptr);
    if (!ok(st)) {
        fail("overlay Describe = %d", st);
        return;
    }
    if (!inst) {
        return;
    }
    PropSet ii;
    OfxPropertySetHandle h = PH(&ii);
    pSetPointer(h, kOfxPropEffectInstance, 0, inst);
    double ps[2] = { 1., 1. };
    pSetDoubleN(h, kOfxInteractPropPixelScale, 2, ps);
    double bg[3] = { 0., 0., 0. };
    pSetDoubleN(h, kOfxInteractPropBackgroundColour, 3, bg);
    pSetInt(h, kOfxInteractPropBitDepth, 0, 8);
    pSetInt(h, kOfxInteractPropHasAlpha, 0, 0);
    st = entry(kOfxActionCreateInstance, &ii, nullptr, nullptr);
    if (!ok(st)) {
        fail("overlay CreateInstance = %d", st);
        return;
    }
    st = entry(kOfxActionDestroyInstance, &ii, nullptr, nullptr);
    if (!ok(st)) {
        fail("overlay DestroyInstance = %d", st);
    }
}

// Run the tests of one plug-in.
void
testPlugin(OfxPlugin* p,
           const Spec& s)
{
    std::printf("== %s v%d.%d ==\n", p->pluginIdentifier, p->pluginVersionMajor, p->pluginVersionMinor);
    const int failuresBefore = gFailures;
    std::string msgs;
    (void)errorMessages(&msgs);
    msgs.clear();
    const std::string file = s.file.empty() ? std::string() : gWork + "/" + s.file;
    if ((s.kind == eReader) && (fileSize(file) <= 0)) {
        fail("the input %s was not written", file.c_str());
        return;
    }
    if (s.kind == eWriter) {
        std::remove(file.c_str());
    }

    p->setHost(&gOfxHost);
    OfxStatus st = p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr);
    if (!ok(st)) {
        fail("Load = %d", st);
        return;
    }
    Effect desc;
    st = p->mainEntry(kOfxActionDescribe, EH(desc), nullptr, nullptr);
    if (!ok(st)) {
        fail("Describe = %d", st);
        p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
        return;
    }
    std::vector<std::string> contexts;
    {
        auto it = desc.props.props.find(kOfxImageEffectPropSupportedContexts);
        if (it != desc.props.props.end()) contexts = it->second.s;
    }
    // overlays: an OverlayInteractV2 (OFX 1.5 DrawSuite) needs an OverlayInteractV1 (OpenGL) too
    OfxPluginEntryPoint* overlay = nullptr;
    {
        auto v1 = desc.props.props.find(kOfxImageEffectPluginPropOverlayInteractV1);
        auto v2 = desc.props.props.find("OfxImageEffectPluginPropOverlayInteractV2");
        const bool hasV1 = (v1 != desc.props.props.end()) && !v1->second.p.empty() && v1->second.p[0];
        const bool hasV2 = (v2 != desc.props.props.end()) && !v2->second.p.empty() && v2->second.p[0];
        if (hasV2 && !hasV1) fail("an OverlayInteractV2 without an OverlayInteractV1");
        if (hasV1) {
            overlay = reinterpret_cast<OfxPluginEntryPoint*>(v1->second.p[0]);
            std::printf("  overlay: OverlayInteractV1%s\n", hasV2 ? " and V2" : "");
            testOverlay(overlay, nullptr);
        }
    }
    std::string context;
    const char* wanted = (s.kind == eFilter) ? kOfxImageEffectContextFilter : kOfxImageEffectContextGeneral;
    for (const std::string& c : contexts) {
        Effect ce;
        PropSet inArgs;
        pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, c.c_str());
        st = p->mainEntry(kOfxImageEffectActionDescribeInContext, EH(ce), PH(&inArgs), nullptr);
        if (!ok(st)) fail("DescribeInContext(%s) = %d", c.c_str(), st);
        if (c == wanted) context = c;
    }
    if (context.empty()) {
        fail("no %s context (contexts: %zu)", wanted, contexts.size());
        p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
        return;
    }
    std::printf("  context %s\n", context.c_str());

    Effect ctx;
    {
        PropSet inArgs;
        pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, context.c_str());
        st = p->mainEntry(kOfxImageEffectActionDescribeInContext, EH(ctx), PH(&inArgs), nullptr);
        if (!ok(st)) {
            fail("DescribeInContext(%s) = %d", context.c_str(), st);
            p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
            return;
        }
    }
    if (!file.empty()) {
        if (!ctx.params.count(kOfxImageEffectFileParamName)) {
            fail("no %s param", kOfxImageEffectFileParamName);
        }
        setOverrideString(&ctx, kOfxImageEffectFileParamName, file);
    }
    for (const auto& sp : s.strings) {
        if (!ctx.params.count(sp.first)) fail("no %s param", sp.first.c_str());
        setOverrideString(&ctx, sp.first.c_str(), expand(sp.second));
    }
    for (const auto& ip : s.ints) {
        if (!ctx.params.count(ip.first)) fail("no %s param", ip.first.c_str());
        setOverrideInt(&ctx, ip.first.c_str(), ip.second);
    }

    Clip* src = ctx.clips.count(kOfxImageEffectSimpleSourceClipName) ? ctx.clips[kOfxImageEffectSimpleSourceClipName] : nullptr;
    Clip* out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
    if (!out) {
        fail("no Output clip");
        p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
        return;
    }
    const int full[4] = { 0, 0, kW, kH };
    if (src) {
        src->connected = (s.kind != eReader);
        setupImage(src, full, kOfxImageComponentRGBA);
        fillColour(src, kOfxImageComponentRGBA);
    }
    out->connected = true;
    setupImage(out, full, kOfxImageComponentRGBA);

    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, kW, kH, 1, context.c_str());
    st = p->mainEntry(kOfxActionCreateInstance, EH(inst), nullptr, nullptr);
    if (!ok(st)) {
        fail("CreateInstance = %d", st);
        p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
        return;
    }
    if (overlay) {
        testOverlay(overlay, &inst);
    }
    if (s.kind == eReader) {
        // the file changed, as when the user sets it: the reader sets up its frame range and format
        PropSet a = timeArgs(1.);
        pSetString(PH(&a), kOfxPropType, 0, kOfxTypeParameter);
        pSetString(PH(&a), kOfxPropName, 0, kOfxImageEffectFileParamName);
        pSetString(PH(&a), kOfxPropChangeReason, 0, kOfxChangeUserEdited);
        st = p->mainEntry(kOfxActionInstanceChanged, EH(inst), PH(&a), nullptr);
        if (!ok(st)) fail("InstanceChanged(%s) = %d", kOfxImageEffectFileParamName, st);
    }

    // clip preferences: the components of each clip (the depth is float, which every plug-in supports)
    std::string srcComps = kOfxImageComponentRGBA, outComps = kOfxImageComponentRGBA;
    {
        PropSet o;
        st = p->mainEntry(kOfxImageEffectActionGetClipPreferences, EH(inst), nullptr, PH(&o));
        if (!ok(st)) fail("GetClipPreferences = %d", st);
        outComps = clipPref(o, "OfxImageClipPropComponents", kOfxImageEffectOutputClipName, outComps);
        if (src) srcComps = clipPref(o, "OfxImageClipPropComponents", kOfxImageEffectSimpleSourceClipName, srcComps);
        const std::string outDepth = clipPref(o, "OfxImageClipPropDepth", kOfxImageEffectOutputClipName, kOfxBitDepthFloat);
        if (outDepth != kOfxBitDepthFloat) fail("output depth %s (float expected)", outDepth.c_str());
    }
    if (src) {
        setupImage(src, full, srcComps);
        fillColour(src, srcComps);
    }

    double t = 1.;
    {
        PropSet o;
        st = p->mainEntry(kOfxImageEffectActionGetTimeDomain, EH(inst), nullptr, PH(&o));
        double fr[2] = { 1., 1. };
        if (st == kOfxStatOK) {
            pGetDoubleN(PH(&o), kOfxImageEffectPropFrameRange, 2, fr);
            t = fr[0];
        } else if (!ok(st)) {
            fail("GetTimeDomain = %d", st);
        }
        std::printf("  time domain %g..%g (%s)\n", fr[0], fr[1], st == kOfxStatOK ? "plug-in" : "default");
    }

    // the region of definition (the default one is the source's, or the project's)
    int rod[4] = { 0, 0, kW, kH };
    {
        PropSet a = timeArgs(t);
        PropSet o;
        st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, EH(inst), PH(&a), PH(&o));
        if (st == kOfxStatOK) {
            double r[4] = { 0., 0., 0., 0. };
            if (pGetDoubleN(PH(&o), kOfxImageEffectPropRegionOfDefinition, 4, r) != kOfxStatOK) {
                fail("GetRegionOfDefinition: no RoD");
            }
            // (a RoD outside of [-1e6, 1e6] fails, NaN included)
            bool sane = true;
            for (int k = 0; k < 4; ++k) {
                sane = sane && (r[k] >= -1e6) && (r[k] <= 1e6);
            }
            if (!sane || !(r[2] > r[0]) || !(r[3] > r[1])) {
                fail("GetRegionOfDefinition: RoD %g %g %g %g", r[0], r[1], r[2], r[3]);
            } else {
                rod[0] = (int)std::floor(r[0]);
                rod[1] = (int)std::floor(r[1]);
                rod[2] = (int)std::ceil(r[2]);
                rod[3] = (int)std::ceil(r[3]);
            }
        } else if (!ok(st)) {
            fail("GetRegionOfDefinition = %d", st);
        }
        std::printf("  RoD %d %d %d %d\n", rod[0], rod[1], rod[2], rod[3]);
    }
    if ((s.kind == eReader) && ((rod[0] != 0) || (rod[1] != 0) || (rod[2] != kW) || (rod[3] != kH))) {
        fail("the reader's RoD is not 0 0 %d %d", kW, kH);
    }
    setupImage(out, rod, outComps);
    {
        PropSet a = timeArgs(t);
        double roi[4] = { (double)rod[0], (double)rod[1], (double)rod[2], (double)rod[3] };
        pSetDoubleN(PH(&a), kOfxImageEffectPropRegionOfInterest, 4, roi);
        PropSet o;
        st = p->mainEntry(kOfxImageEffectActionGetRegionsOfInterest, EH(inst), PH(&a), PH(&o));
        if (!ok(st)) fail("GetRegionsOfInterest = %d", st);
    }
    {
        PropSet a = timeArgs(t);
        PropSet o;
        st = p->mainEntry(kOfxImageEffectActionGetFramesNeeded, EH(inst), PH(&a), PH(&o));
        if (!ok(st)) fail("GetFramesNeeded = %d", st);
    }
    {
        PropSet a = timeArgs(t);
        pSetString(PH(&a), kOfxImageEffectPropFieldToRender, 0, kOfxImageFieldNone);
        int win[4] = { rod[0], rod[1], rod[2], rod[3] };
        pSetIntN(PH(&a), kOfxImageEffectPropRenderWindow, 4, win);
        PropSet o;
        st = p->mainEntry(kOfxImageEffectActionIsIdentity, EH(inst), PH(&a), PH(&o));
        if (!ok(st)) fail("IsIdentity = %d", st);
        if (st == kOfxStatOK) std::printf("  identity\n");
    }

    {
        PropSet a = sequenceArgs(t);
        st = p->mainEntry(kOfxImageEffectActionBeginSequenceRender, EH(inst), PH(&a), nullptr);
        if (!ok(st)) fail("BeginSequenceRender = %d", st);
    }
    const int on = nComps(outComps);
    float* op = reinterpret_cast<float*>(out->buffer.data());
    const size_t npix = (size_t)(rod[2] - rod[0]) * (rod[3] - rod[1]);
    fillUnwritten(op, npix * on);
    {
        PropSet a = makeRenderInArgs(t, kW, kH);
        int win[4] = { rod[0], rod[1], rod[2], rod[3] };
        pSetIntN(PH(&a), kOfxImageEffectPropRenderWindow, 4, win);
        st = p->mainEntry(kOfxImageEffectActionRender, EH(inst), PH(&a), nullptr);
        if (!ok(st)) fail("Render = %d", st);
    }
    {
        PropSet a = sequenceArgs(t);
        st = p->mainEntry(kOfxImageEffectActionEndSequenceRender, EH(inst), PH(&a), nullptr);
        if (!ok(st)) fail("EndSequenceRender = %d", st);
    }

    if (s.kind != eWriter) {
        size_t unwritten = 0;
        double sum[4] = { 0., 0., 0., 0. };
        double maxDiff = 0.;
        for (size_t i = 0; i < npix; ++i) {
            for (int k = 0; k < on; ++k) {
                const float* v = op + i * on + k;
                if (isUnwritten(v)) {
                    ++unwritten;
                    continue;
                }
                sum[k] += *v;
                const float expected = (on == 1) ? kColour[3] : kColour[k];
                const double d = std::fabs((double)*v - expected);
                if (!(d <= maxDiff)) maxDiff = d; // (NaN: never <=)
            }
        }
        if (unwritten) fail("%zu of the %zu output values were not written", unwritten, npix * on);
        std::printf("  output %s, mean", outComps.c_str());
        for (int k = 0; k < on; ++k) std::printf(" %.4f", sum[k] / (double)npix);
        std::printf(", max |output - source colour| = %g\n", maxDiff);
        if ((s.tol >= 0.) && !(maxDiff <= s.tol)) {
            fail("the output differs from the source colour by %g (tolerance %g)", maxDiff, s.tol);
        }
    } else {
        const long sz = fileSize(file);
        std::printf("  wrote %s (%ld bytes)\n", file.c_str(), sz);
        if (sz <= 0) fail("did not write %s", file.c_str());
    }

    st = p->mainEntry(kOfxActionDestroyInstance, EH(inst), nullptr, nullptr);
    if (!ok(st)) fail("DestroyInstance = %d", st);
    st = p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    if (!ok(st)) fail("Unload = %d", st);
    if (errorMessages(&msgs)) fail("error messages: %s", msgs.c_str());
    std::printf("  %s\n", gFailures == failuresBefore ? "PASS" : "FAIL");
}

} // namespace

int
main(int argc,
     char** argv)
{
    int arg = 1;
    gOfx14Only = true;
    gDefaultUnsetProps = true;
    if ((arg < argc) && !std::strcmp(argv[arg], "--full")) {
        gOfx14Only = false;
        ++arg;
    }
    std::string only;
    if ((arg + 1 < argc) && !std::strcmp(argv[arg], "--only")) {
        only = argv[arg + 1];
        arg += 2;
    }
    if (arg >= argc) {
        std::fprintf(stderr, "Usage: %s [--full] <work directory>\n", argv[0]);
        return 2;
    }
    gWork = argv[arg];
#ifdef _WIN32
    _mkdir(gWork.c_str());
#else
    mkdir(gWork.c_str(), 0777);
#endif
    std::printf("host: %s\n", gOfx14Only ? "OFX 1.4 only (no newer suite or property, no extension)" : "full");
    initHost();

    // an identity 1D LUT for OCIOFileTransform (which has no default file)
    {
        std::FILE* f = std::fopen((gWork + "/identity.cube").c_str(), "w");
        if (f) {
            std::fputs("LUT_1D_SIZE 2\n0 0 0\n1 1 1\n", f);
            std::fclose(f);
        }
    }

    // tolerances: lossless formats (with 8-bit quantization), lossy formats (MOV)
    const double kLossless = 0.003, kLossy = 0.03, kNone = -1.;
    const std::vector<Spec> specs = {
        // writers first, then the readers of their files
        { "fr.inria.openfx.WritePNG", eWriter, "image.png", kNone, {}, {} },
        { "fr.inria.openfx.WriteEXR", eWriter, "image.exr", kNone, {}, {} },
        { "fr.inria.openfx.WritePFM", eWriter, "image.pfm", kNone, {}, {} },
        { "fr.inria.openfx.WriteOIIO", eWriter, "image.tif", kNone, {}, {} },
        { "fr.inria.openfx.WriteFFmpeg", eWriter, "movie.mov", kNone, {}, {} },
        { "fr.inria.openfx.ReadPNG", eReader, "image.png", kLossless, {}, {} },
        { "fr.inria.openfx.ReadEXR", eReader, "image.exr", kLossless, {}, {} },
        { "fr.inria.openfx.ReadPFM", eReader, "image.pfm", kLossless, {}, {} },
        { "fr.inria.openfx.ReadOIIO", eReader, "image.tif", kLossless, {}, {} },
        { "fr.inria.openfx.ReadFFmpeg", eReader, "movie.mov", kLossy, {}, {} },
        // filters, with default parameters (OCIO: test.ocio, whose colour spaces are identities)
        { "fr.inria.openfx.OCIOColorSpace", eFilter, "", kLossless, {}, {} },
        { "fr.inria.openfx.OCIODisplay", eFilter, "", kNone, {}, {} },
        { "fr.inria.openfx.OCIOCDLTransform", eFilter, "", kLossless, {}, {} },
        { "fr.inria.openfx.OCIOFileTransform", eFilter, "", kLossless, { { "file", "@/identity.cube" } }, {} },
        { "fr.inria.openfx.OCIOLogConvert", eFilter, "", kNone, {}, {} },
        { "fr.inria.openfx.OCIOLookTransform", eFilter, "", kNone, {}, {} },
        { "fr.inria.openfx.OIIOText", eFilter, "", kNone, {}, {} },
        { "fr.inria.openfx.OIIOResize", eFilter, "", kNone, {}, {} },
        // SeExpr, SeExprSimple (in hosts other than Natron) and RunScript don't render until their script is
        // validated: validate the default one
        { "fr.inria.openfx.SeExpr", eFilter, "", kNone, {}, { { "validate", 1 } } },
        { "fr.inria.openfx.SeExprSimple", eFilter, "", kNone, {}, { { "validate", 1 } } },
        { "net.sf.openfx.SeGrain", eFilter, "", kNone, {}, {} },
        { "net.sf.openfx.SeNoise", eFilter, "", kNone, {}, {} },
        { "fr.inria.openfx.RunScript", eFilter, "", kNone, {}, { { "validate", 1 } } },
    };

    const int n = OfxGetNumberOfPlugins();
    std::printf("plugins=%d\n", n);
    std::vector<bool> tested(n, false);
    for (const Spec& s : specs) {
        if (!only.empty() && (only != s.id)) {
            for (int i = 0; i < n; ++i) {
                OfxPlugin* p = OfxGetPlugin(i);
                if (p && p->pluginIdentifier && !std::strcmp(p->pluginIdentifier, s.id)) tested[i] = true;
            }
            continue;
        }
        bool found = false;
        for (int i = 0; i < n; ++i) {
            OfxPlugin* p = OfxGetPlugin(i);
            if (p && p->pluginIdentifier && !std::strcmp(p->pluginIdentifier, s.id)) {
                found = true;
                tested[i] = true;
                testPlugin(p, s);
            }
        }
        if (!found) {
            std::printf("--   %s is not in this build\n", s.id);
        }
    }
    for (int i = 0; i < n; ++i) {
        OfxPlugin* p = OfxGetPlugin(i);
        if (!tested[i]) fail("%s has no test in ofx14_host_test.cpp", p && p->pluginIdentifier ? p->pluginIdentifier : "?");
    }
    if (gOfx14Only) {
        std::printf("suites fetched:");
        for (const std::string& f : gFetchedSuites) std::printf(" [%s]", f.c_str());
        std::printf("\n");
    }

    std::printf("Result: failures=%d\n", gFailures);
    std::printf(gFailures == 0 ? "PASS\n" : "FAIL\n");
    return gFailures == 0 ? 0 : 1;
}
