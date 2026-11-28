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
 *   - the obsolete plug-ins (ReadEXR, WriteEXR, OIIOText, OIIOResize v1) must set kOfxImageEffectPluginPropObsolete
 *     to 1 (not in the OFX 1.4-only host, which doesn't know it);
 *   - a filter or reader must write its whole output image;
 *   - a writer writes a file (in the work directory), which the readers then read: WritePNG -> ReadPNG and ReadOIIO,
 *     WriteEXR -> ReadEXR, WritePFM -> ReadPFM, WriteOIIO (TIFF) -> ReadOIIO, WriteFFmpeg (MOV) -> ReadFFmpeg;
 *   - a reader's image must be the writer's input (a constant colour), within a tolerance;
 *   - the filters that are an identity with their default parameters (OCIOColorSpace, OCIOCDLTransform,
 *     OCIOFileTransform with an identity LUT, with the identity colour spaces of test.ocio) must output their input;
 *   - host quirks (openfx-misc's README-hosts.txt), in every context: every XY or XYAbsolute param with a non-zero
 *     default sets kOfxParamPropDefaultCoordinateSystem (Nuke defaults it to normalised), every int or double param
 *     (1D, 2D or 3D) has a range and display range (Resolve clamps it to 0..0 or -1..1 otherwise), the
 *     GenericReader and GenericWriter frame params range over INT_MIN..INT_MAX, a plug-in whose descriptor sets
 *     kOfxImageEffectInstancePropSequentialRender sets it on its instance too (Resolve and Nuke ignore it on the
 *     descriptor), and in a host named DaVinciResolve* with the OpenGL render suite (which Resolve never uses), the
 *     enableGPU param is disabled. The host honours normalised defaults (gHonourNormalisedDefaults), except at
 *     OFX 1.1 and in the Resolve host, which reject kOfxParamPropDefaultCoordinateSystem: there, the default transform
 *     center must still be the project centre, and not count as a user edit.
 *   - in every context, each param keeps its display range in a host that resets it when the range is set (OFX 1.5.1
 *     ofxParam.h, kOfxParamPropMin: "Setting this will also reset ::kOfxParamPropDisplayMin"): the plug-in must set
 *     the range first.
 * Each writer also gets an InstanceChanged on its filename, its time domain must be its input's frame range (1..3:
 * the default frame range is the time line's, and the host never has the optional TimeLine suite), and its RoD the
 * input's, also with the input format (formatType 0, without Natron's kOfxImageClipPropFormat).
 * Every context of every plug-in is also instantiated, with an InstanceChanged on each of its params.
 * Every plug-in of the binary must have a test below: a new plug-in without one fails.
 * Environment variables select other hosts (see tests/README.md): OFX14_LEVEL=11 (OFX 1.1: no property of OFX 1.2 to
 * 1.4, no Message suite V2, no RGB), OFX14_GL=1 (the optional OpenGL render suite), OFX14_SEQSTATUS=0 (the sequential
 * render status), OFX14_NOSUITE, OFX14_COMPS, OFX14_DEPTHS, OFX14_SCALE...
 * OFX14_ROWBYTES=1 (#456): the source is a pattern instead of a constant colour (so the colour checks are skipped), and
 * each plug-in renders four times, with the images of every RowLayout of ofx_test_host.h: packed bottom-up rows, padded
 * rows, top-down rows (a negative kOfxImagePropRowBytes) and padded top-down rows. Each render must give the same
 * output image (or the same file, for a writer) as the packed one, and leave the padding of the rows untouched.
 * `ctest -R test-ofx14-rowbytes` runs it with AddressSanitizer and UBSan's pointer-overflow check.
 *
 * Usage: ofx14_host_test [--full] [--scan] [--only <plug-in identifier>] <work directory>
 *   --full: the same tests in the full host (the default mode of ofx_test_host.h), for comparison. In that mode, a
 *           plug-in that registers an OverlayInteractV2 (DrawSuite) must also register an OverlayInteractV1.
 *   --scan: first, the plug-in scan of a host that unloads the binary after it (HostSupport's default since openfx
 *           #174): every plug-in gets Load and Describe, then they all get Unload, before the tests load, describe
 *           and use each one again. The binary stays in memory (as on macOS, where a binary with thread-local
 *           variables is never unmapped): the global state of the plug-ins must survive Unload, and be initialised
 *           again by Load and Describe. After the scan, the Support library must have forgotten the host
 *           description, and OpenEXR's global thread pool, which the "host" started, must still have threads.
 *   --only: test only this plug-in (its input file, for a reader, must have been written by an earlier run).
 * Needs $OCIO (tests/test.ocio).
 *
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <IlmThreadPool.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h> // _mkdir
#endif
#include <utility>
#include <vector>

namespace OFX {
struct ImageEffectHostDescription;
ImageEffectHostDescription* getImageEffectHostDescription(); // ofxsImageEffect.cpp
}

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

std::string gWork;

// The host variant, from the environment (see tests/README.md; initHost() reads the others), read once by main()
struct Variant {
    const char* seqStatus = nullptr;  // OFX14_SEQSTATUS: kOfxImageEffectPropSequentialRenderStatus (default 1)
    const char* skipParams = nullptr; // OFX14_SKIPPARAMS: the params without an InstanceChanged (comma-separated)
    const char* setInt = nullptr;     // OFX14_SETINT: name=value of int params (comma-separated)
    bool forceScale = false;          // OFX14_FORCESCALE: OFX14_SCALE also for the plug-ins without multi-resolution
    bool scaleWriters1 = false;       // OFX14_SCALE_WRITERS1: the writers render at scale 1
    bool noContexts = false;          // OFX14_NOCTX: no instance in every context
    // (for the banner only: initHost() reads them)
    const char* noSuite = nullptr;    // OFX14_NOSUITE
    const char* depths = nullptr;     // OFX14_DEPTHS
    const char* comps = nullptr;      // OFX14_COMPS
    const char* hostName = nullptr;   // OFX14_HOSTNAME
    bool noTiles = false;             // OFX14_NOTILES
    bool noMultiRes = false;          // OFX14_NOMULTIRES
    bool noOverlays = false;          // OFX14_NOOVERLAYS
    bool rowLayouts = false;          // OFX14_ROWBYTES: render in every RowLayout (see the top of this file)
};
Variant gVariant;

Variant
readVariant()
{
    Variant v;
    v.seqStatus = std::getenv("OFX14_SEQSTATUS");
    v.skipParams = std::getenv("OFX14_SKIPPARAMS");
    v.setInt = std::getenv("OFX14_SETINT");
    v.forceScale = std::getenv("OFX14_FORCESCALE") != nullptr;
    v.scaleWriters1 = std::getenv("OFX14_SCALE_WRITERS1") != nullptr;
    v.noContexts = std::getenv("OFX14_NOCTX") != nullptr;
    v.noSuite = std::getenv("OFX14_NOSUITE");
    v.depths = std::getenv("OFX14_DEPTHS");
    v.comps = std::getenv("OFX14_COMPS");
    v.hostName = std::getenv("OFX14_HOSTNAME");
    v.noTiles = std::getenv("OFX14_NOTILES") != nullptr;
    v.noMultiRes = std::getenv("OFX14_NOMULTIRES") != nullptr;
    v.noOverlays = std::getenv("OFX14_NOOVERLAYS") != nullptr;
    v.rowLayouts = std::getenv("OFX14_ROWBYTES") != nullptr;
    return v;
}

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

std::string
expand(const std::string& v)
{
    return (!v.empty() && v[0] == '@') ? gWork + v.substr(1) : v;
}

// the frame range of the clips: 1..gLastFrame (3 for the writers, whose default frame range is the time line's: in a
// host without the optional TimeLine suite, it must be the input's)
double gLastFrame = 1.;

double gReqScale = 1.; // requested OFX14_SCALE
std::string gDepth = kOfxBitDepthFloat; // depth of the current images
float getV(const std::vector<uint8_t>& buf, size_t i, const std::string& d)
{
    if (d == kOfxBitDepthFloat) { float f; std::memcpy(&f, buf.data() + 4 * i, 4); return f; }
    if (d == kOfxBitDepthShort) { uint16_t v; std::memcpy(&v, buf.data() + 2 * i, 2); return v / 65535.f; }
    return buf[i] / 255.f;
}
void setV(std::vector<uint8_t>& buf, size_t i, const std::string& d, float f)
{
    if (d == kOfxBitDepthFloat) { std::memcpy(buf.data() + 4 * i, &f, 4); return; }
    if (d == kOfxBitDepthShort) { uint16_t v = (uint16_t)std::lround(f * 65535.f); std::memcpy(buf.data() + 2 * i, &v, 2); return; }
    buf[i] = (uint8_t)std::lround(f * 255.f);
}
// The value of the byte and short components that the plug-in did not write (float: a NaN, see fillUnwritten())
const int kUnwrittenInt = 7;
void fillUnwrittenV(std::vector<uint8_t>& buf, size_t n, const std::string& d)
{
    if (d == kOfxBitDepthFloat) { fillUnwritten(reinterpret_cast<float*>(buf.data()), n); return; }
    for (size_t i = 0; i < n; ++i) {
        if (d == kOfxBitDepthShort) { const uint16_t v = kUnwrittenInt; std::memcpy(buf.data() + 2 * i, &v, 2); }
        else buf[i] = kUnwrittenInt;
    }
}
bool unwrittenV(const std::vector<uint8_t>& buf, size_t i, const std::string& d)
{
    if (d == kOfxBitDepthFloat) return isUnwritten(reinterpret_cast<const float*>(buf.data()) + i);
    if (d == kOfxBitDepthShort) { uint16_t v; std::memcpy(&v, buf.data() + 2 * i, 2); return v == kUnwrittenInt; }
    return buf[i] == kUnwrittenInt;
}
// The image of a clip: bounds b (x1, y1, x2, y2, in pixels), components comps, depth depth (by default gDepth's)
void
setupImage(Clip* c,
           const int b[4],
           const std::string& comps,
           const std::string& depth = gDepth)
{
    const int w = b[2] - b[0], h = b[3] - b[1];
    setupClipImage(c, w, h, comps.c_str(), depth.c_str(), true);
    pSetIntN(PH(&c->image), kOfxImagePropBounds, 4, b);
    pSetIntN(PH(&c->image), kOfxImagePropRegionOfDefinition, 4, b);
    double rod[4] = { (double)b[0] / gRS, (double)b[1] / gRS, (double)b[2] / gRS, (double)b[3] / gRS };
    pSetDoubleN(PH(&c->props), kOfxImageEffectPropRegionOfDefinition, 4, rod);
    double fr[2] = { 1., gLastFrame };
    pSetDoubleN(PH(&c->props), kOfxImageEffectPropFrameRange, 2, fr);
}

// Fills the image of c with kColour, or with OFX14_ROWBYTES a pattern that differs in every row and column, alpha
// included (in (0, 1], so that an unpremultiplication stays finite)
void
fillColour(Clip* c,
           const std::string& comps)
{
    const int n = componentCount(comps.c_str());
    const std::string d = gDepth;
    std::vector<uint8_t> v(c->pixelRowBytes * c->rows);
    const size_t w = c->pixelRowBytes / (bytesPerComponent(d.c_str()) * n);
    const size_t npix = v.size() / (bytesPerComponent(d.c_str()) * n);
    for (size_t i = 0; i < npix; ++i) {
        for (int k = 0; k < n; ++k) {
            const int x = (int)(i % w), y = (int)(i / w);
            const float pattern = ((k == 3) || (n == 1)) ? (float)(1 + (x + 2 * y) % 4) / 4.f
                                                         : (float)((3 * x + 7 * y + 5 * k) % 16) / 15.f;
            setV(v, i * n + k, d, gVariant.rowLayouts ? pattern : ((n == 1) ? kColour[3] : kColour[k]));
        }
    }
    setPackedPixels(c, v);
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
    double rs[2] = { gRS, gRS };
    pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
    // OFX14_SEQSTATUS: the value of kOfxImageEffectPropSequentialRenderStatus (OFX 1.2; a 1.4 host may pass 0, and
    // with OFX14_LEVEL < 12 it is unknown, so absent)
    pSetInt(ap, kOfxImageEffectPropSequentialRenderStatus, 0, gVariant.seqStatus ? std::atoi(gVariant.seqStatus) : 1);
    pSetInt(ap, kOfxImageEffectPropInteractiveRenderStatus, 0, 0);
    return a;
}

PropSet
timeArgs(double t)
{
    PropSet a;
    pSetDouble(PH(&a), kOfxPropTime, 0, t);
    double rs[2] = { gRS, gRS };
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
    PropSet ip(PropSet::eInteract);
    OfxStatus st = entry(kOfxActionDescribe, &ip, nullptr, nullptr);
    if (!ok(st)) {
        fail("overlay Describe = %d", st);
        return;
    }
    if (!inst) {
        return;
    }
    PropSet ii(PropSet::eInteract);
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

// InstanceChanged on a param (inside Begin/EndInstanceChanged)
void
paramChanged(OfxPlugin* p, Effect& inst, const std::string& name, double t)
{
    PropSet b; pSetString(PH(&b), kOfxPropChangeReason, 0, kOfxChangeUserEdited);
    OfxStatus st = p->mainEntry(kOfxActionBeginInstanceChanged, EH(inst), PH(&b), nullptr);
    if (!ok(st)) fail("BeginInstanceChanged = %d", st);
    PropSet a = timeArgs(t);
    pSetString(PH(&a), kOfxPropType, 0, kOfxTypeParameter);
    pSetString(PH(&a), kOfxPropName, 0, name.c_str());
    pSetString(PH(&a), kOfxPropChangeReason, 0, kOfxChangeUserEdited);
    st = p->mainEntry(kOfxActionInstanceChanged, EH(inst), PH(&a), nullptr);
    if (!ok(st)) fail("InstanceChanged(%s) = %d", name.c_str(), st);
    st = p->mainEntry(kOfxActionEndInstanceChanged, EH(inst), PH(&b), nullptr);
    if (!ok(st)) fail("EndInstanceChanged = %d", st);
}

// The params of a context descriptor must not depend on the host's defaults (see the comment at the top).
void
checkHostQuirkParams(const std::string& c,
                     const Effect& ctx)
{
    auto props = [&ctx](const char* name) -> const std::map<std::string, Value>* {
        auto it = ctx.params.find(name);
        return (it != ctx.params.end()) ? &it->second.props->props : nullptr;
    };
    for (const auto& kv : ctx.params) {
        const std::map<std::string, Value>& pr = kv.second.props->props;
        const char* name = kv.first.c_str();
        if (kv.second.type == kOfxParamTypeDouble2D) {
            auto dt = pr.find(kOfxParamPropDoubleType);
            const bool xy = (dt != pr.end()) && !dt->second.s.empty()
                            && ((dt->second.s[0] == kOfxParamDoubleTypeXY) || (dt->second.s[0] == kOfxParamDoubleTypeXYAbsolute));
            // a default of (0, 0) (also the default default) is the origin in both coordinate systems
            auto def = pr.find(kOfxParamPropDefault);
            const bool zero = (def == pr.end())
                              || std::all_of(def->second.d.begin(), def->second.d.end(), [](double d) { return d == 0.; });
            // kOfxParamPropDefaultCoordinateSystem is OFX 1.2: an older host (and Resolve) rejects it
            if (xy && !zero && (unknownProp(kOfxParamPropDefaultCoordinateSystem) == kOfxStatOK)
                && !pr.count(kOfxParamPropDefaultCoordinateSystem)) {
                fail("[%s] the XY param %s has no kOfxParamPropDefaultCoordinateSystem", c.c_str(), name);
            }
        }
        // Resolve clamps a numeric param without a range and display range (to (0,0) or (-1,1)): Support sets the
        // spec's defaults (OFXS_SUPPORT_HAS_SPEC_PARAM_DEFAULTS), the plug-in may narrow them
        const std::string& t = kv.second.type;
        const bool isInt = (t == kOfxParamTypeInteger) || (t == kOfxParamTypeInteger2D) || (t == kOfxParamTypeInteger3D);
        const bool isDouble = (t == kOfxParamTypeDouble) || (t == kOfxParamTypeDouble2D) || (t == kOfxParamTypeDouble3D);
        if ((isInt || isDouble)
            && (!pr.count(kOfxParamPropMin) || !pr.count(kOfxParamPropMax) || !pr.count(kOfxParamPropDisplayMin)
                || !pr.count(kOfxParamPropDisplayMax))) {
            fail("[%s] the %s param %s has no range or display range", c.c_str(), t.c_str(), name);
        }
    }
    // The GenericReader and GenericWriter frame params: frames may be negative, and kParamOriginalFrameRange's
    // default (INT_MIN, INT_MAX) means "not set by the host" (GenericReader getSequenceTimeDomainInternal()), so their
    // range and display range must be the whole int range. Every reader has the five, every writer the first two.
    static const struct { const char* name; int value; } kWholeIntRange[] = {
        { kOfxParamPropMin, INT_MIN }, { kOfxParamPropMax, INT_MAX },
        { kOfxParamPropDisplayMin, INT_MIN }, { kOfxParamPropDisplayMax, INT_MAX },
    };
    std::vector<const char*> frameParams;
    if (c == kOfxImageEffectContextReader) {
        frameParams = { "firstFrame", "lastFrame", "startingTime", "timeOffset", "originalFrameRange" };
    } else if (c == kOfxImageEffectContextWriter) {
        frameParams = { "firstFrame", "lastFrame" };
    }
    for (const char* name : frameParams) {
        const std::map<std::string, Value>* pr = props(name);
        if (!pr) {
            fail("[%s] no frame param %s", c.c_str(), name);
            continue;
        }
        for (const auto& r : kWholeIntRange) {
            auto it = pr->find(r.name);
            if ((it == pr->end()) || it->second.i.empty()
                || !std::all_of(it->second.i.begin(), it->second.i.end(), [&r](int v) { return v == r.value; })) {
                fail("[%s] the frame param %s: %s is not %d", c.c_str(), name, r.name, r.value);
            }
        }
    }
    // Resolve: "even though OfxImageEffectOpenGLRenderSuite exists, the render action is never called with OpenGL
    // enabled"
    const char* hostName = nullptr;
    pGetString(PH(&gHostProps), kOfxPropName, 0, &hostName);
    if (gOpenGLRender && hostName && !std::strncmp(hostName, "DaVinciResolve", 14)) {
        if (const std::map<std::string, Value>* pr = props("enableGPU")) {
            auto it = pr->find(kOfxParamPropEnabled);
            if ((it == pr->end()) || it->second.i.empty() || it->second.i[0]) {
                fail("[%s] the enableGPU param is enabled in %s", c.c_str(), hostName);
            }
        }
    }
}

// A host that follows OFX 1.5.1 resets the display range on setRange (gResetDisplayRangeOnSetRange): the plug-in must
// set the range first, so that each display range it sets in the context descriptor (ctx) is kept in the one
// described there (ctx151). A display range that the plug-in doesn't set is Support's default (the spec's default,
// the whole int or double range), which the 1.5.1 host may replace by the range: as the spec asks.
void
checkDisplayRangeKept(const std::string& c,
                      const Effect& ctx,
                      const Effect& ctx151)
{
    static const struct { const char* name; int i; double d; } kDisplay[] = {
        { kOfxParamPropDisplayMin, INT_MIN, std::numeric_limits<double>::lowest() },
        { kOfxParamPropDisplayMax, INT_MAX, std::numeric_limits<double>::max() },
    };
    for (const auto& kv : ctx.params) {
        auto it151 = ctx151.params.find(kv.first);
        if (it151 == ctx151.params.end()) {
            fail("[%s] the param %s is not described in the OFX 1.5.1 host", c.c_str(), kv.first.c_str());
            continue;
        }
        for (const auto& r : kDisplay) {
            auto a = kv.second.props->props.find(r.name);
            if (a == kv.second.props->props.end()) continue;
            auto b = it151->second.props->props.find(r.name);
            const Value none;
            const Value& v151 = (b != it151->second.props->props.end()) ? b->second : none;
            bool lost = false;
            for (size_t k = 0; k < a->second.i.size(); ++k) {
                lost |= (a->second.i[k] != r.i) && ((k >= v151.i.size()) || (v151.i[k] != a->second.i[k]));
            }
            for (size_t k = 0; k < a->second.d.size(); ++k) {
                lost |= (a->second.d[k] != r.d) && ((k >= v151.d.size()) || (v151.d[k] != a->second.d[k]));
            }
            if (lost) {
                fail("[%s] the param %s loses its %s in an OFX 1.5.1 host: the plug-in sets it before the range",
                     c.c_str(), kv.first.c_str(), r.name);
            }
        }
    }
}

// Every context: describe, instantiate, InstanceChanged on every param, clip preferences, destroy.
void
exerciseContext(OfxPlugin* p, const std::string& c, const std::string& file)
{
    Effect ce;
    PropSet inArgs;
    pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, c.c_str());
    OfxStatus st = p->mainEntry(kOfxImageEffectActionDescribeInContext, EH(ce), PH(&inArgs), nullptr);
    if (!ok(st)) { fail("[%s] DescribeInContext = %d", c.c_str(), st); return; }
    if (!file.empty()) setOverrideString(&ce, kOfxImageEffectFileParamName, file);
    const int full[4] = { 0, 0, (int)std::lround(kW * gRS), (int)std::lround(kH * gRS) };
    for (auto& kv : ce.clips) {
        kv.second->connected = (kv.first == kOfxImageEffectOutputClipName) || (kv.first == kOfxImageEffectSimpleSourceClipName && c != kOfxImageEffectContextReader);
        setupImage(kv.second, full, kOfxImageComponentRGBA);
        fillColour(kv.second, kOfxImageComponentRGBA);
    }
    Effect inst;
    inst.delegate = &ce;
    setInstanceProps(inst, kW, kH, 1, c.c_str());
    st = p->mainEntry(kOfxActionCreateInstance, EH(inst), nullptr, nullptr);
    if (!ok(st)) { fail("[%s] CreateInstance = %d", c.c_str(), st); return; }
    int nchanged = 0;
    for (auto& kv : ce.params) {
        if (kv.second.type == kOfxParamTypePage || kv.second.type == kOfxParamTypeGroup) continue;
        if (gVariant.skipParams && std::strstr(gVariant.skipParams, kv.first.c_str())) continue;
        std::string m; (void)errorMessages(&m);
        paramChanged(p, inst, kv.first, 1.);
        ++nchanged;
        m.clear();
        if (errorMessages(&m)) std::printf("  [%s] note: messages after changing %s: %s\n", c.c_str(), kv.first.c_str(), m.c_str());
    }
    PropSet o;
    st = p->mainEntry(kOfxImageEffectActionGetClipPreferences, EH(inst), nullptr, PH(&o));
    if (!ok(st)) fail("[%s] GetClipPreferences = %d", c.c_str(), st);
    st = p->mainEntry(kOfxActionDestroyInstance, EH(inst), nullptr, nullptr);
    if (!ok(st)) fail("[%s] DestroyInstance = %d", c.c_str(), st);
    std::printf("  context %s: instance, %d params changed\n", c.c_str(), nchanged);
}

// The contents of a written file, with the date and time of a TIFF DateTime tag ("YYYY:MM:DD HH:MM:SS", which
// OpenImageIO writes) replaced by zeros
std::string
writtenContents(const std::string& file)
{
    static const std::regex dateTime("[0-9]{4}:[0-9]{2}:[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}");
    return std::regex_replace(fileContents(file), dateTime, "0000:00:00 00:00:00");
}

// Run the tests of one plug-in.
void
testPlugin(OfxPlugin* p,
           const Spec& s)
{
    std::printf("== %s v%d.%d ==\n", p->pluginIdentifier, p->pluginVersionMajor, p->pluginVersionMinor);
    const int failuresBefore = gFailures;
    gLastFrame = (s.kind == eWriter) ? 3. : 1.;
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
    {
        // obsolete plug-ins (#237): kOfxImageEffectPluginPropObsolete (next OFX release, after 1.5.1), which the
        // OFX 1.4-only host rejects as unknown (the plug-in must still describe)
        const char* id = p->pluginIdentifier;
        const bool expected = !gOfx14Only
                              && (!std::strcmp(id, "fr.inria.openfx.ReadEXR") || !std::strcmp(id, "fr.inria.openfx.WriteEXR")
                                  || !std::strcmp(id, "fr.inria.openfx.OIIOText")
                                  || (!std::strcmp(id, "fr.inria.openfx.OIIOResize") && p->pluginVersionMajor < 2));
        auto it = desc.props.props.find(kOfxImageEffectPluginPropObsolete);
        const bool obsolete = it != desc.props.props.end() && !it->second.i.empty() && it->second.i[0];
        if (obsolete != expected) {
            fail("kOfxImageEffectPluginPropObsolete = %d, expected %d", int(obsolete), int(expected));
        }
    }
    {
        // honour the plug-in's kOfxImageEffectPropSupportsMultiResolution (default 1), unless OFX14_FORCESCALE
        int mr = 1;
        auto it = desc.props.props.find(kOfxImageEffectPropSupportsMultiResolution);
        if (it != desc.props.props.end() && !it->second.i.empty()) mr = it->second.i[0];
        gRS = (mr || gVariant.forceScale) ? gReqScale : 1.;
        if (s.kind == eWriter && gVariant.scaleWriters1) gRS = 1.;
        gDepth = kOfxBitDepthFloat;
        if (gRS != 1.) std::printf("  render scale %g\n", gRS);
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
        checkHostQuirkParams(c, ce);
        {
            Effect ce151;
            gResetDisplayRangeOnSetRange = true;
            st = p->mainEntry(kOfxImageEffectActionDescribeInContext, EH(ce151), PH(&inArgs), nullptr);
            gResetDisplayRangeOnSetRange = false;
            if (!ok(st)) fail("DescribeInContext(%s), OFX 1.5.1 display range = %d", c.c_str(), st);
            checkDisplayRangeKept(c, ce, ce151);
        }
        if (c == wanted) context = c;
        if (!gVariant.noContexts) exerciseContext(p, c, file.empty() ? std::string() : (s.kind == eWriter ? file + ".ctx." + c + file.substr(file.rfind('.')) : file));
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
    if (gVariant.setInt) {
        for (auto& kv : splitc(gVariant.setInt)) {
            size_t q = kv.find('='); std::string n = kv.substr(0, q);
            if (ctx.params.count(n)) { setOverrideInt(&ctx, n.c_str(), std::atoi(kv.c_str() + q + 1)); std::printf("  set %s\n", kv.c_str()); }
        }
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
    const int full[4] = { 0, 0, (int)std::lround(kW * gRS), (int)std::lround(kH * gRS) };
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
    {
        // kOfxImageEffectInstancePropSequentialRender: "plugin descriptor (read/write) or plugin instance (read/write)"
        auto sequential = [](const PropSet& ps) {
            auto it = ps.props.find(kOfxImageEffectInstancePropSequentialRender);
            return (it != ps.props.end()) && !it->second.i.empty() ? it->second.i[0] : 0;
        };
        if (sequential(desc.props) && !sequential(inst.props)) {
            fail("kOfxImageEffectInstancePropSequentialRender is %d on the descriptor, but not set on the instance", sequential(desc.props));
        }
    }
    if (ctx.params.count("transformCenter") && ctx.params.count("transformCenterChanged")) {
        // ofxsTransformDescribeParams(): the default center (0.5, 0.5) is normalised. In every host, including those
        // that ignore normalised defaults (OFX 1.1, Resolve: the plug-in converts it), it is the project centre, and
        // not a user edit.
        ParamRec* c = &ctx.params["transformCenter"];
        const double x = paramDouble(c, 0), y = paramDouble(c, 1);
        if ((x != kW / 2.) || (y != kH / 2.)) {
            fail("the default transformCenter is (%g, %g), not the project centre (%g, %g)", x, y, kW / 2., kH / 2.);
        }
        if (paramInt(&ctx.params["transformCenterChanged"], 0)) {
            fail("transformCenterChanged is set after CreateInstance");
        }
    }
    if (overlay) {
        testOverlay(overlay, &inst);
    }
    if (s.kind == eWriter) {
        paramChanged(p, inst, kOfxImageEffectFileParamName, 1.);
        // (the file may have set params, e.g. the output colour space from the file type: the spec's values win)
        for (const auto& sp : s.strings) {
            setOverrideString(&ctx, sp.first.c_str(), expand(sp.second));
        }
        for (const auto& ip : s.ints) {
            setOverrideInt(&ctx, ip.first.c_str(), ip.second);
        }
        // the Manual frame range starts at the time line bounds, here the input's frame range, which may be infinite
        // ("infinite if there are no non optional input clips"): the first and last frames are clamped to +-INT_MAX/2
        // (a conversion of infinity to int is undefined behaviour)
        if (src && ctx.params.count("frameRange") && ctx.params.count("firstFrame") && ctx.params.count("lastFrame")) {
            const double inf[2] = { -HUGE_VAL, HUGE_VAL };
            pSetDoubleN(PH(&src->props), kOfxImageEffectPropFrameRange, 2, inf);
            setOverrideInt(&ctx, "frameRange", 2);
            paramChanged(p, inst, "frameRange", 1.);
            const int first = paramInt(&ctx.params["firstFrame"], 0), last = paramInt(&ctx.params["lastFrame"], 0);
            std::printf("  Manual frame range from an infinite input range: %d..%d\n", first, last);
            if ((first != -INT_MAX / 2) || (last != INT_MAX / 2)) {
                fail("first and last frames %d..%d, not -INT_MAX/2..INT_MAX/2", first, last);
            }
            setOverrideInt(&ctx, "frameRange", 1);
            setOverrideInt(&ctx, "firstFrame", 0);
            setOverrideInt(&ctx, "lastFrame", 0);
            paramChanged(p, inst, "frameRange", 1.);
            const double fr[2] = { 1., gLastFrame };
            pSetDoubleN(PH(&src->props), kOfxImageEffectPropFrameRange, 2, fr);
        }
    }
    if (s.kind == eReader) {
        // the file changed, as when the user sets it: the reader sets up its frame range and format
        paramChanged(p, inst, kOfxImageEffectFileParamName, 1.);
        // (the file may have set params, e.g. the file colour space: the spec's values win)
        for (const auto& sp : s.strings) {
            setOverrideString(&ctx, sp.first.c_str(), expand(sp.second));
        }
        for (const auto& ip : s.ints) {
            setOverrideInt(&ctx, ip.first.c_str(), ip.second);
        }
    }

    // clip preferences: the components of each clip (the depth is float, which every plug-in supports)
    std::string srcComps = kOfxImageComponentRGBA, outComps = kOfxImageComponentRGBA;
    {
        PropSet o;
        st = p->mainEntry(kOfxImageEffectActionGetClipPreferences, EH(inst), nullptr, PH(&o));
        if (!ok(st)) fail("GetClipPreferences = %d", st);
        outComps = clipPref(o, "OfxImageClipPropComponents", kOfxImageEffectOutputClipName, outComps);
        if (src) srcComps = clipPref(o, "OfxImageClipPropComponents", kOfxImageEffectSimpleSourceClipName, srcComps);
        std::string hostDepth = kOfxBitDepthFloat;
        { const char* d0 = nullptr; if (pGetString(PH(&gHostProps), kOfxImageEffectPropSupportedPixelDepths, 0, &d0) == kOfxStatOK && d0) hostDepth = d0; int nd = 0; pGetDimension(PH(&gHostProps), kOfxImageEffectPropSupportedPixelDepths, &nd); for (int k = 0; k < nd; ++k) { const char* dk; pGetString(PH(&gHostProps), kOfxImageEffectPropSupportedPixelDepths, k, &dk); if (!std::strcmp(dk, kOfxBitDepthFloat)) hostDepth = kOfxBitDepthFloat; } }
        const std::string outDepth = clipPref(o, "OfxImageClipPropDepth", kOfxImageEffectOutputClipName, hostDepth);
        const std::string srcDepth = src ? clipPref(o, "OfxImageClipPropDepth", kOfxImageEffectSimpleSourceClipName, hostDepth) : outDepth;
        std::printf("  depths: source %s output %s\n", srcDepth.c_str(), outDepth.c_str());
        int nd = 0; pGetDimension(PH(&gHostProps), kOfxImageEffectPropSupportedPixelDepths, &nd);
        bool hostHas = false;
        for (int k = 0; k < nd; ++k) { const char* dk; pGetString(PH(&gHostProps), kOfxImageEffectPropSupportedPixelDepths, k, &dk); if (outDepth == dk) hostHas = true; }
        if (!hostHas) fail("output depth %s is not supported by the host", outDepth.c_str());
        if (srcDepth != outDepth) std::printf("  note: source and output depths differ\n");
        gDepth = outDepth;
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
        // a writer renders its input's frames by default (frame range: time line bounds, or the input's without the
        // TimeLine suite, which the test host never has)
        if ((s.kind == eWriter) && ((fr[0] != 1.) || (fr[1] != gLastFrame))) {
            fail("the writer's time domain is %g..%g, not the input's frame range 1..%g", fr[0], fr[1], gLastFrame);
        }
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
    if (((s.kind == eReader) || (s.kind == eWriter)) && ((rod[0] != 0) || (rod[1] != 0) || (rod[2] != kW) || (rod[3] != kH))) {
        fail("the %s's RoD is not 0 0 %d %d", s.kind == eReader ? "reader" : "writer", kW, kH);
    }
    const int crod[4] = { rod[0], rod[1], rod[2], rod[3] }; // canonical
    for (int k = 0; k < 4; ++k) rod[k] = (int)(k < 2 ? std::floor(crod[k] * gRS) : std::ceil(crod[k] * gRS)); // pixel
    setupImage(out, rod, outComps);
    {
        PropSet a = timeArgs(t);
        double roi[4] = { (double)crod[0], (double)crod[1], (double)crod[2], (double)crod[3] };
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

    const int on = componentCount(outComps.c_str());
    const size_t npix = (size_t)(rod[2] - rod[0]) * (rod[3] - rod[1]);
    // the output image (packed), or the written file, of the render in the first layout (packed rows)
    std::vector<uint8_t> output;
    std::string written;
    const std::vector<RowLayout> layouts = gVariant.rowLayouts
        ? std::vector<RowLayout>{ RowLayout::ePacked, RowLayout::ePadded, RowLayout::eTopDown, RowLayout::eTopDownPadded }
        : std::vector<RowLayout>{ RowLayout::ePacked };
    for (RowLayout layout : layouts) {
        // the source and output images in this layout (with the same pixels)
        gRowLayout = layout;
        if (src) {
            setupImage(src, full, srcComps);
            fillColour(src, srcComps);
        }
        setupImage(out, rod, outComps);
        {
            PropSet a = sequenceArgs(t);
            st = p->mainEntry(kOfxImageEffectActionBeginSequenceRender, EH(inst), PH(&a), nullptr);
            if (!ok(st)) fail("BeginSequenceRender = %d", st);
        }
        {
            std::vector<uint8_t> v(out->pixelRowBytes * out->rows);
            fillUnwrittenV(v, npix * on, gDepth);
            setPackedPixels(out, v);
        }
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
        if (layout == RowLayout::ePacked) {
            output = packedPixels(out);
            written = (s.kind == eWriter) ? writtenContents(file) : std::string();
            continue;
        }
        // the same output as with packed rows, and nothing written in the padding of the rows
        const char* name = rowLayoutName(layout);
        if (!rowPaddingIntact(out) || (src && !rowPaddingIntact(src))) {
            fail("%s rows: the padding of the rows was written", name);
        }
        if (s.kind == eWriter) {
            const std::string w = writtenContents(file);
            if (w != written) fail("%s rows: the file differs from the one written from packed rows (%zu bytes, not %zu)", name, w.size(), written.size());
            else std::printf("  %s rows: the same file\n", name);
        } else if (packedPixels(out) != output) {
            const std::vector<uint8_t> o = packedPixels(out);
            size_t nDiff = 0;
            for (size_t i = 0; i < o.size(); ++i) nDiff += (o[i] != output[i]);
            fail("%s rows: %zu of the %zu output bytes differ from the output with packed rows", name, nDiff, o.size());
        } else {
            std::printf("  %s rows: the same output\n", name);
        }
    }
    gRowLayout = RowLayout::ePacked;

    if (s.kind != eWriter) {
        size_t unwritten = 0;
        double sum[4] = { 0., 0., 0., 0. };
        double maxDiff = 0.;
        for (size_t i = 0; i < npix; ++i) {
            for (int k = 0; k < on; ++k) {
                if (unwrittenV(output, i * on + k, gDepth)) {
                    ++unwritten;
                    continue;
                }
                const float v = getV(output, i * on + k, gDepth);
                sum[k] += v;
                const float expected = (on == 1) ? kColour[3] : kColour[k];
                const double d = std::fabs((double)v - expected);
                if (!(d <= maxDiff)) maxDiff = d; // (NaN: never <=)
            }
        }
        if (unwritten) fail("%zu of the %zu output values were not written", unwritten, npix * on);
        std::printf("  output %s, mean", outComps.c_str());
        for (int k = 0; k < on; ++k) std::printf(" %.4f", sum[k] / (double)npix);
        std::printf(", max |output - source colour| = %g\n", maxDiff);
        if (gVariant.rowLayouts) {
            // (the source is a pattern, not the colour)
        } else if ((s.tol >= 0.) && !(maxDiff <= s.tol)) {
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

// --scan: Load and Describe every plug-in of the binary, then Unload them all (see the top of this file)
void
scanPlugins(int n)
{
    std::printf("== scan: Load and Describe each plug-in, then Unload them ==\n");
#ifndef _WIN32
    // started by the "host" (OpenImageIO may also set its number of threads, when the OIIO plug-ins load it): the
    // Unload of the plug-ins must not stop it (on MS Windows, the OIIO plug-ins stop OpenEXR's pool at their last
    // Unload, and start it again in their next instance, see OIIOGlobal.cpp)
    IlmThread::ThreadPool::globalThreadPool().setNumThreads(2);
#endif
    std::vector<std::unique_ptr<Effect>> descs(n);
    for (int i = 0; i < n; ++i) {
        OfxPlugin* p = OfxGetPlugin(i);
        if (!p) {
            fail("scan: OfxGetPlugin(%d) = NULL", i);
            continue;
        }
        p->setHost(&gOfxHost);
        OfxStatus st = p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr);
        if (!ok(st)) {
            fail("scan: %s: Load = %d", p->pluginIdentifier, st);
            continue;
        }
        descs[i].reset(new Effect);
        st = p->mainEntry(kOfxActionDescribe, EH(*descs[i]), nullptr, nullptr);
        if (!ok(st)) fail("scan: %s: Describe = %d", p->pluginIdentifier, st);
    }
    if (!OFX::getImageEffectHostDescription()) fail("scan: no host description after Load");
    for (int i = 0; i < n; ++i) {
        if (descs[i]) {
            OfxPlugin* p = OfxGetPlugin(i);
            OfxStatus st = p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
            if (!ok(st)) fail("scan: %s: Unload = %d", p->pluginIdentifier, st);
        }
    }
    std::printf("  %d plug-ins loaded, described and unloaded\n", n);
    // the Support library forgets the host description at the last Unload (openfx #174)
    if (OFX::getImageEffectHostDescription()) fail("scan: the Support library kept the host description after the last Unload");
#ifndef _WIN32
    const int exrThreads = IlmThread::ThreadPool::globalThreadPool().numThreads();
    std::printf("  OpenEXR's global thread pool after the scan: %d threads\n", exrThreads);
    if (exrThreads <= 0) fail("scan: the Unload of the plug-ins stopped OpenEXR's thread pool");
    IlmThread::ThreadPool::globalThreadPool().setNumThreads(0);
#endif
    std::string msgs;
    if (errorMessages(&msgs)) fail("scan: error messages: %s", msgs.c_str());
}

} // namespace

int
main(int argc,
     char** argv)
{
    int arg = 1;
    gOfx14Only = true;
    gDefaultUnsetProps = true;
    bool scan = false;
    for (; arg < argc; ++arg) {
        if (!std::strcmp(argv[arg], "--full")) {
            gOfx14Only = false;
        } else if (!std::strcmp(argv[arg], "--scan")) {
            scan = true;
        } else {
            break;
        }
    }
    std::string only;
    if ((arg + 1 < argc) && !std::strcmp(argv[arg], "--only")) {
        only = argv[arg + 1];
        arg += 2;
    }
    if (arg >= argc) {
        std::fprintf(stderr, "Usage: %s [--full] [--scan] [--only <plug-in identifier>] <work directory>\n", argv[0]);
        return 2;
    }
    gWork = argv[arg];
#ifdef _WIN32
    _mkdir(gWork.c_str());
#else
    mkdir(gWork.c_str(), 0777);
#endif
    std::printf("host: %s\n", gOfx14Only ? "OFX 1.4 only (no newer suite or property, no extension)" : "full");
    gHonourNormalisedDefaults = true; // as Natron and Nuke do (kOfxParamPropDefaultCoordinateSystem)
    gVariant = readVariant();
    initHost();
    gReqScale = gRS;
    const Variant& v = gVariant;
    std::printf("variant: level %d, nosuite=%s, depths=%s, comps=%s, tiles=%d, multires=%d, scale=%g, overlays=%d, opengl=%d, seqstatus=%s, hostname=%s, rowbytes=%d\n", gOfxLevel, v.noSuite ? v.noSuite : "-", v.depths ? v.depths : "all", v.comps ? v.comps : "default", !v.noTiles, !v.noMultiRes, gRS, !v.noOverlays, (int)gOpenGLRender, v.seqStatus ? v.seqStatus : "1", v.hostName ? v.hostName : "-", (int)v.rowLayouts);

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
    std::vector<Spec> specs = {
        // writers first, then the readers of their files
        { "fr.inria.openfx.WritePNG", eWriter, "image.png", kNone, {}, {} },
        // the input format (formatType 0): without kOfxImageClipPropFormat (Natron), the input's RoD
        { "fr.inria.openfx.WritePNG", eWriter, "image-input.png", kNone, {}, { { "formatType", 0 } } },
        { "fr.inria.openfx.WriteEXR", eWriter, "image.exr", kNone, {}, {} },
        { "fr.inria.openfx.WritePFM", eWriter, "image.pfm", kNone, {}, {} },
        { "fr.inria.openfx.WriteOIIO", eWriter, "image.tif", kNone, {}, {} },
        { "fr.inria.openfx.WriteFFmpeg", eWriter, "movie.mov", kNone, {}, {} },
        // (OFX14_ROWBYTES: the writers again, inserted here, and the readers again after the readers)
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

    if (gVariant.rowLayouts) {
        // the writers again, with the same input and output colour spaces, and an unpremultiplied or opaque input:
        // GenericWriter may then give the input image itself to encode(), whose rows must be packed (GenericWriter.h),
        // except for an opaque input with alpha, whose alpha it sets to 1 (it must not depend on the rows either)
        const auto readers = std::find_if(specs.begin(), specs.end(), [](const Spec& sp) { return sp.kind == eReader; });
        std::vector<Spec> writers;
        for (auto it = specs.begin(); it != readers; ++it) {
            if (it->ints.empty()) {
                for (const int premult : { 2 /* UnPreMultiplied */, 0 /* Opaque */ }) {
                    Spec w = *it;
                    w.file = (premult ? "identity-unpremult-" : "identity-opaque-") + w.file;
                    w.strings = { { "ocioInputSpace", "linear" }, { "ocioOutputSpace", "linear" } };
                    w.ints = { { "inputPremult", premult } };
                    writers.push_back(w);
                }
            }
        }
        const auto end = specs.insert(readers, writers.begin(), writers.end()) + writers.size();
        // the readers again, with the same file and output colour spaces and an unpremultiplied output: GenericReader
        // then decodes straight into the output image (GenericReader::render())
        std::vector<Spec> readers2;
        for (auto it = end; it != specs.end() && it->kind == eReader; ++it) {
            Spec r = *it;
            r.strings = { { "ocioInputSpace", "linear" }, { "ocioOutputSpace", "linear" } };
            r.ints = { { "outputPremult", 2 } }; // UnPreMultiplied
            readers2.push_back(r);
        }
        const auto filters = std::find_if(end, specs.end(), [](const Spec& sp) { return sp.kind == eFilter; });
        specs.insert(filters, readers2.begin(), readers2.end());
    }

    const int n = OfxGetNumberOfPlugins();
    std::printf("plugins=%d\n", n);
    if (scan) {
        scanPlugins(n);
    }
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
