/*
 * ocio_cdl_test — OCIOCDLTransform's Export param (OCIO/OCIOCDLTransform.cpp, issue #105) and
 * its "Read from file" param (issue #280).
 *
 * Setting the Export string param (a user edit) must write the grade as a ColorCorrection
 * XML file (.cc) that OCIO reads back with the same slope, offset, power and saturation,
 * must not overwrite an existing file, must refuse an inverse grade (which a .cc file can't
 * express), must report errors without a persistent message (which would stop rendering),
 * and must reset the param to its default in every case. Export with the param's default (empty)
 * file name must write no file and report an error (issue #302).
 * (With OCIO >= 2, Export used to fail: GroupTransform::write() was called with a
 * nonexistent "CDL" format on an empty group, leaving an empty file, and the error path
 * called fclose(NULL).)
 *
 * With "Read from file" on, creating an instance must succeed even if the CDL file is missing (a
 * kOfxStatFailed from kOfxActionCreateInstance makes Natron drop the node when it loads the
 * project), and neither creating an instance nor rendering may set a param (ofxParam.h:
 * "paramSetValue should only be called from within a kOfxActionInstanceChanged or interact
 * action"): render reads the grade from the file, and a missing file fails the render with a
 * persistent error message. The file is loaded into the params by kOfxActionInstanceChanged.
 * Also: turning "Read from file" off copies the file's grade into the params; Export exports the
 * file's grade; the CCC Id selects a grade of a .ccc file (and a wrong one is an error); a missing
 * file that appears is read by the next render, without Reload (failures are not cached).
 *
 * The plug-in is linked directly and driven through the OFX actions by the in-process host
 * of ofx_test_host.h; the exported file is read back by an OCIO FileTransform.
 *
 * Usage: ocio_cdl_test   (needs $OCIO, see tests/Makefile)
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <OpenColorIO/OpenColorIO.h>

#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace OCIO = OCIO_NAMESPACE;

namespace {

// from OCIO/OCIOCDLTransform.cpp
const char* const kPluginIdentifier = "fr.inria.openfx.OCIOCDLTransform";
const char* const kParamExport = "export";
// the Export param has no setDefault(): its default is the OFX default of a string param
const char* const kParamExportDefault = "";

const double kSlope[3] = { 1.25, 0.875, 1.5 };
const double kOffset[3] = { 0.0625, -0.03125, 0.1 };
const double kPower[3] = { 1.1, 0.9, 1.2 };
const double kSaturation = 0.7;

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

OfxImageEffectHandle
H(Effect& e)
{
    return reinterpret_cast<OfxImageEffectHandle>(&e);
}

void
setOverrideDoubles(Effect* ctx, const char* name, std::vector<double> v)
{
    auto it = ctx->params.find(name);
    if (it == ctx->params.end()) {
        std::printf("  FAIL: no param %s\n", name);
        ++gFailures;

        return;
    }
    it->second.hasOv = true;
    it->second.ov.d = v;
}

bool
fileExists(const std::string& path)
{
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (f) {
        std::fclose(f);
    }

    return f != nullptr;
}

std::string
fileContents(const std::string& path)
{
    std::ifstream ifs(path.c_str(), std::ios::binary);
    std::ostringstream os;
    os << ifs.rdbuf();

    return os.str();
}

// The user sets the Export param to filename: kOfxActionInstanceChanged (user edit)
OfxStatus
exportTo(OfxPlugin* p, Effect& ctx, Effect& inst, const std::string& filename)
{
    setOverrideString(&ctx, kParamExport, filename);
    PropSet a;
    OfxPropertySetHandle ap = PH(&a);
    pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
    pSetString(ap, kOfxPropName, 0, kParamExport);
    pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
    pSetDouble(ap, kOfxPropTime, 0, 1.);
    double rs[2] = { 1., 1. };
    pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);

    return p->mainEntry(kOfxActionInstanceChanged, H(inst), PH(&a), nullptr);
}

std::string
exportValue(Effect& ctx)
{
    return paramString(&ctx.params[kParamExport]);
}

// Read filename with an OCIO FileTransform: the CDL it contains, or null
OCIO::ConstCDLTransformRcPtr
readCDL(const std::string& filename, std::string* error)
{
    try {
        OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();
        OCIO::FileTransformRcPtr ft = OCIO::FileTransform::Create();
        ft->setSrc(filename.c_str());
        ft->setInterpolation(OCIO::INTERP_LINEAR);
        OCIO::ConstProcessorRcPtr proc = config->getProcessor(ft);
        OCIO::GroupTransformRcPtr g = proc->createGroupTransform();
        for (int i = 0; i < g->getNumTransforms(); ++i) {
            OCIO::ConstCDLTransformRcPtr cdl = OCIO_DYNAMIC_POINTER_CAST<const OCIO::CDLTransform>(g->getTransform(i));
            if (cdl) {
                return cdl;
            }
        }
        *error = "no CDL in the file";
    } catch (const std::exception& e) {
        *error = e.what();
    }

    return OCIO::ConstCDLTransformRcPtr();
}

bool
closeEnough(double a, double b)
{
    return std::fabs(a - b) <= 1e-5;
}

void
testExport(OfxPlugin* p, Effect& ctx, Effect& inst, const std::string& dir)
{
    std::printf("==== Export to a new .cc file ====\n");
    const std::string filename = dir + "/grade.cc";
    gMessages.clear();
    gPersistentError.clear();
    OfxStatus st = exportTo(p, ctx, inst, filename);
    check(ok(st), "instanceChanged(export) returns OK (status %d)", st);
    check(gMessages.empty(), "no error message (got \"%s\")", gMessages.c_str());
    check(gPersistentError.empty(), "no persistent error (got \"%s\")", gPersistentError.c_str());
    check(fileExists(filename), "%s exists", filename.c_str());
    const std::string xml = fileContents(filename);
    check(xml.find("<ColorCorrection") != std::string::npos, "the file is a ColorCorrection XML (%zu bytes)", xml.size());
    std::string error;
    OCIO::ConstCDLTransformRcPtr cdl = readCDL(filename, &error);
    check((bool)cdl, "OCIO reads a CDL from it (%s)", cdl ? "ok" : error.c_str());
    if (cdl) {
        double sop[9];
        cdl->getSOP(sop);
        bool same = true;
        for (int c = 0; c < 3; ++c) {
            same = same && closeEnough(sop[c], kSlope[c]) && closeEnough(sop[3 + c], kOffset[c]) && closeEnough(sop[6 + c], kPower[c]);
        }
        check(same, "slope (%g %g %g), offset (%g %g %g), power (%g %g %g) read back",
              sop[0], sop[1], sop[2], sop[3], sop[4], sop[5], sop[6], sop[7], sop[8]);
        check(closeEnough(cdl->getSat(), kSaturation), "saturation %g read back", cdl->getSat());
        check(cdl->getDirection() == OCIO::TRANSFORM_DIR_FORWARD, "direction forward");
    }
    check(exportValue(ctx) == kParamExportDefault, "Export param reset to its default (\"%s\")", exportValue(ctx).c_str());
    std::remove(filename.c_str());
}

void
testExistingFile(OfxPlugin* p, Effect& ctx, Effect& inst, const std::string& dir)
{
    std::printf("==== Export to an existing file ====\n");
    const std::string filename = dir + "/existing.cc";
    const std::string contents = "not to be overwritten\n";
    {
        std::ofstream ofs(filename.c_str(), std::ios::binary);
        ofs << contents;
    }
    gMessages.clear();
    OfxStatus st = exportTo(p, ctx, inst, filename);
    check(ok(st), "instanceChanged(export) returns OK (status %d)", st);
    check(gMessages.find("already exists") != std::string::npos, "error message: file already exists (got \"%s\")", gMessages.c_str());
    check(fileContents(filename) == contents, "the existing file is not overwritten");
    check(exportValue(ctx) == kParamExportDefault, "Export param reset to its default (\"%s\")", exportValue(ctx).c_str());
    std::remove(filename.c_str());
}

void
testInverse(OfxPlugin* p, Effect& ctx, Effect& inst, const std::string& dir)
{
    std::printf("==== Export an inverse grade (the .cc format has no direction) ====\n");
    const std::string filename = dir + "/inverse.cc";
    setOverrideInt(&ctx, "direction", 1);
    gMessages.clear();
    OfxStatus st = exportTo(p, ctx, inst, filename);
    check(ok(st), "instanceChanged(export) returns OK (status %d)", st);
    check(gMessages.find("inverse") != std::string::npos, "error message: inverse grade (got \"%s\")", gMessages.c_str());
    check(!std::ifstream(filename.c_str()).good(), "no file is written");
    check(exportValue(ctx) == kParamExportDefault, "Export param reset to its default (\"%s\")", exportValue(ctx).c_str());
    setOverrideInt(&ctx, "direction", 0);
    std::remove(filename.c_str());
}

void
testUnwritable(OfxPlugin* p, Effect& ctx, Effect& inst, const std::string& dir)
{
    std::printf("==== Export to a file that can't be created ====\n");
    const std::string filename = dir + "/no such directory/grade.cc";
    gMessages.clear();
    gPersistentError.clear();
    OfxStatus st = exportTo(p, ctx, inst, filename);
    check(ok(st), "instanceChanged(export) returns OK (status %d)", st);
    check(gMessages.find("cannot be written") != std::string::npos, "error message: cannot be written (got \"%s\")", gMessages.c_str());
    check(gPersistentError.empty(), "no persistent error, which would stop rendering (got \"%s\")", gPersistentError.c_str());
    check(exportValue(ctx) == kParamExportDefault, "Export param reset to its default (\"%s\")", exportValue(ctx).c_str());
}

// the names of the entries of dir, or false if dir can't be read
bool
dirEntries(const std::filesystem::path& dir, std::vector<std::string>* names)
{
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        names->push_back(it->path().filename().string());
    }

    return !ec;
}

// sets the current directory, and restores the previous one when destroyed
class ScopedCurrentPath
{
public:
    explicit ScopedCurrentPath(const std::filesystem::path& dir)
        : _previous(std::filesystem::current_path(_ec))
    {
        if (!_ec) {
            std::filesystem::current_path(dir, _ec);
        }
    }
    ~ScopedCurrentPath()
    {
        if (!_ec) {
            std::error_code ec;
            std::filesystem::current_path(_previous, ec);
        }
    }
    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

    bool ok() const { return !_ec; }

private:
    std::error_code _ec; // declared before _previous, which is initialized with it
    const std::filesystem::path _previous;
};

void
testDefaultFilename(OfxPlugin* p, Effect& ctx, Effect& inst, const std::string& dir)
{
    std::printf("==== Export with the default file name (issue #302) ====\n");
    // a relative file name is created in the current directory: use an empty scratch directory
    std::error_code ec;
    const std::filesystem::path sub = std::filesystem::absolute(std::filesystem::path(dir) / "default", ec);
    if (ec || !std::filesystem::create_directory(sub, ec) || ec) {
        check(false, "create directory %s", sub.string().c_str());

        return;
    }
    std::vector<std::string> names;
    {
        const ScopedCurrentPath cd(sub);
        if (!cd.ok()) {
            check(false, "change the current directory to %s", sub.string().c_str());

            return;
        }
        ParamRec& param = ctx.params[kParamExport];
        param.hasOv = false;
        const std::string defaultName = paramString(&param); // the descriptor's default
        gMessages.clear();
        gPersistentError.clear();
        // a host or script sends a user edit while the param holds its default (e.g. sets the same value)
        OfxStatus st = exportTo(p, ctx, inst, defaultName);
        check(ok(st), "instanceChanged(export) returns OK (status %d)", st);
        const bool listed = dirEntries(sub, &names);
        check(listed && names.empty(), "no file is written (%s)",
              !listed ? "can't list the directory" : names.empty() ? "none" : names[0].c_str());
        check(gMessages.find("file name is empty") != std::string::npos, "error message: empty file name (got \"%s\")", gMessages.c_str());
        check(gPersistentError.empty(), "no persistent error, which would stop rendering (got \"%s\")", gPersistentError.c_str());
        check(exportValue(ctx) == defaultName, "Export param keeps its default (\"%s\")", exportValue(ctx).c_str());
    }
    for (const std::string& n : names) {
        std::filesystem::remove(sub / n, ec);
    }
}

// the number of occurrences of what in s
int
occurrences(const std::string& s, const char* what)
{
    int n = 0;
    for (std::size_t pos = s.find(what); pos != std::string::npos; pos = s.find(what, pos + 1)) {
        ++n;
    }

    return n;
}

// the number of paramSetValue/paramSetValueAtTime calls by the plug-in on the params of ctx
int
setValueCount(const Effect& ctx)
{
    int n = 0;
    for (const auto& kv : ctx.params) {
        n += kv.second.nSet;
    }

    return n;
}

// a CDL grade: slope, offset, power (OCIO's SOP order), saturation
struct Grade {
    double sop[9];
    double saturation;
};
const Grade kGrade = { { kSlope[0], kSlope[1], kSlope[2], kOffset[0], kOffset[1], kOffset[2], kPower[0], kPower[1], kPower[2] }, kSaturation };
const Grade kGrade2 = { { 0.8, 1.1, 0.95, -0.05, 0.02, 0.04, 0.9, 1.15, 1.05 }, 1.3 };

// a ColorCorrection element with grade
void
writeColorCorrection(std::ostream& os, const char* id, const Grade& g)
{
    os << "<ColorCorrection id=\"" << id << "\">\n"
       << "  <SOPNode>\n"
       << "    <Slope>" << g.sop[0] << " " << g.sop[1] << " " << g.sop[2] << "</Slope>\n"
       << "    <Offset>" << g.sop[3] << " " << g.sop[4] << " " << g.sop[5] << "</Offset>\n"
       << "    <Power>" << g.sop[6] << " " << g.sop[7] << " " << g.sop[8] << "</Power>\n"
       << "  </SOPNode>\n"
       << "  <SatNode>\n"
       << "    <Saturation>" << g.saturation << "</Saturation>\n"
       << "  </SatNode>\n"
       << "</ColorCorrection>\n";
}

// a ColorCorrection file (.cc) with the grade g (kGrade by default)
void
writeCC(const std::string& filename, const Grade& g = kGrade)
{
    std::ofstream ofs(filename.c_str(), std::ios::binary);
    ofs.imbue(std::locale::classic());
    writeColorCorrection(ofs, "test", g);
}

// a ColorCorrectionCollection file (.ccc): "first" with kGrade, then "second" with kGrade2
void
writeCCC(const std::string& filename)
{
    std::ofstream ofs(filename.c_str(), std::ios::binary);
    ofs.imbue(std::locale::classic());
    ofs << "<ColorCorrectionCollection xmlns=\"urn:ASC:CDL:v1.01\">\n";
    writeColorCorrection(ofs, "first", kGrade);
    writeColorCorrection(ofs, "second", kGrade2);
    ofs << "</ColorCorrectionCollection>\n";
}

// OCIO's CDL of g applied to rgb
void
applyGrade(const Grade& g, float rgb[3])
{
    try {
        OCIO::CDLTransformRcPtr cc = OCIO::CDLTransform::Create();
        cc->setSOP(g.sop);
        cc->setSat(g.saturation);
        OCIO::GetCurrentConfig()->getProcessor(cc)->getDefaultCPUProcessor()->applyRGB(rgb);
    } catch (const std::exception& e) {
        check(false, "OCIO computes the expected grade (%s)", e.what());
    }
}

// the grade params of ctx are g
bool
paramsAre(Effect& ctx, const Grade& g)
{
    bool same = true;
    for (int c = 0; c < 3; ++c) {
        same = same && closeEnough(paramDouble(&ctx.params["slope"], c), g.sop[c]) && closeEnough(paramDouble(&ctx.params["offset"], c), g.sop[3 + c])
            && closeEnough(paramDouble(&ctx.params["power"], c), g.sop[6 + c]);
    }

    return same && closeEnough(paramDouble(&ctx.params["saturation"], 0), g.saturation);
}

// kOfxActionInstanceChanged on param name (a user edit)
OfxStatus
instanceChanged(OfxPlugin* p, Effect& inst, const char* name)
{
    PropSet a;
    OfxPropertySetHandle ap = PH(&a);
    pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
    pSetString(ap, kOfxPropName, 0, name);
    pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
    pSetDouble(ap, kOfxPropTime, 0, 1.);
    double rs[2] = { 1., 1. };
    pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);

    return p->mainEntry(kOfxActionInstanceChanged, H(inst), PH(&a), nullptr);
}

const int kW = 4;
const int kH = 2;
const float kInputRGB[3] = { 0.2f, 0.4f, 0.6f };

// render the kInputRGB image at time 1 into the output clip of ctx
OfxStatus
render(OfxPlugin* p, Effect& ctx, Effect& inst)
{
    Clip* src = ctx.clips[kOfxImageEffectSimpleSourceClipName];
    Clip* dst = ctx.clips[kOfxImageEffectOutputClipName];
    src->connected = true;
    dst->connected = true;
    setupClipImage(src, kW, kH, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
    setupClipImage(dst, kW, kH, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
    float* s = reinterpret_cast<float*>(src->buffer.data());
    for (int i = 0; i < kW * kH; ++i) {
        s[4 * i] = kInputRGB[0];
        s[4 * i + 1] = kInputRGB[1];
        s[4 * i + 2] = kInputRGB[2];
        s[4 * i + 3] = 1.f;
    }
    fillUnwritten(reinterpret_cast<float*>(dst->buffer.data()), (size_t)4 * kW * kH);
    PropSet a = makeRenderInArgs(1., kW, kH);

    return p->mainEntry(kOfxImageEffectActionRender, H(inst), PH(&a), nullptr);
}

// kOfxImageEffectActionIsIdentity at time 1: kOfxStatOK if identity, kOfxStatReplyDefault if not
OfxStatus
isIdentity(OfxPlugin* p, Effect& inst)
{
    PropSet a;
    OfxPropertySetHandle ap = PH(&a);
    pSetDouble(ap, kOfxPropTime, 0, 1.);
    pSetString(ap, kOfxImageEffectPropFieldToRender, 0, kOfxImageFieldNone);
    int win[4] = { 0, 0, kW, kH };
    pSetIntN(ap, kOfxImageEffectPropRenderWindow, 4, win);
    double rs[2] = { 1., 1. };
    pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
    PropSet o;

    return p->mainEntry(kOfxImageEffectActionIsIdentity, H(inst), PH(&a), PH(&o));
}

// a new instance, in a new context descriptor ctx, with "Read from file" on, the File param set to filename and the
// CCC Id param to cccid
bool
createReadFromFileInstance(OfxPlugin* p, Effect& ctx, Effect& inst, const std::string& filename, OfxStatus* st,
                           const std::string& cccid = std::string())
{
    PropSet inArgs;
    pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
    if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, H(ctx), PH(&inArgs), nullptr))) {
        std::printf("  FAIL: describeInContext\n");
        ++gFailures;

        return false;
    }
    setOverrideInt(&ctx, "readFromFile", 1);
    setOverrideString(&ctx, "file", filename);
    setOverrideString(&ctx, "cccId", cccid);
    inst.delegate = &ctx;
    setInstanceProps(inst, kW, kH, 1, kOfxImageEffectContextFilter);
    gPersistentError.clear();
    *st = p->mainEntry(kOfxActionCreateInstance, H(inst), nullptr, nullptr);

    return true;
}

// render, which must succeed without setting a param, and output the kInputRGB image graded by g
void
checkRenderApplies(OfxPlugin* p, Effect& ctx, Effect& inst, const Grade& g, const char* what)
{
    float expected[3] = { kInputRGB[0], kInputRGB[1], kInputRGB[2] };
    applyGrade(g, expected);
    const int before = setValueCount(ctx);
    gPersistentError.clear();
    const OfxStatus st = render(p, ctx, inst);
    check(ok(st), "%s: render returns OK (status %d)", what, st);
    check(gPersistentError.empty(), "%s: no persistent error (got \"%s\")", what, gPersistentError.c_str());
    check(setValueCount(ctx) == before, "%s: render sets no param (%d paramSetValue calls)", what, setValueCount(ctx) - before);
    const float* d = reinterpret_cast<const float*>(ctx.clips[kOfxImageEffectOutputClipName]->buffer.data());
    bool same = true;
    for (int i = 0; i < kW * kH; ++i) {
        for (int c = 0; c < 3; ++c) {
            same = same && !isUnwritten(&d[4 * i + c]) && std::fabs(d[4 * i + c] - expected[c]) <= 1e-5;
        }
    }
    check(same, "%s: render applies the grade: (%g %g %g) -> (%g %g %g), expected (%g %g %g)", what,
          kInputRGB[0], kInputRGB[1], kInputRGB[2], d[0], d[1], d[2], expected[0], expected[1], expected[2]);
}

void
testMissingFile(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== Read from file: the file is missing (issue #280) ====\n");
    const std::string filename = dir + "/nonexistent/grade.cc";
    Effect ctx, inst;
    OfxStatus st;
    if (!createReadFromFileInstance(p, ctx, inst, filename, &st)) {
        return;
    }
    check(ok(st), "createInstance with readFromFile=1 and a missing file returns OK (status %d)", st);
    if (!ok(st)) {
        return;
    }
    check(setValueCount(ctx) == 0, "createInstance sets no param (%d paramSetValue calls)", setValueCount(ctx));

    int before = setValueCount(ctx);
    gPersistentError.clear();
    st = render(p, ctx, inst);
    check(!ok(st), "render fails (status %d)", st);
    check(gPersistentError.find("grade.cc") != std::string::npos, "render sets a persistent error (\"%s\")", gPersistentError.c_str());
    check(setValueCount(ctx) == before, "render sets no param (%d paramSetValue calls)", setValueCount(ctx) - before);

    // the user opens the node's UI, then presses Reload: the error is reported, the grade params are unchanged
    before = setValueCount(ctx);
    p->mainEntry(kOfxActionBeginInstanceEdit, H(inst), nullptr, nullptr);
    check(setValueCount(ctx) == before, "beginInstanceEdit sets no param (%d paramSetValue calls)", setValueCount(ctx) - before);
    gPersistentError.clear();
    gMessages.clear();
    st = instanceChanged(p, inst, "reload");
    check(ok(st), "instanceChanged(reload) returns OK (status %d)", st);
    check(gPersistentError.find("grade.cc") != std::string::npos, "instanceChanged(reload) sets a persistent error (\"%s\")", gPersistentError.c_str());
    // gMessages records both the persistent and the sent messages: the persistent one only, no modal dialog
    check(occurrences(gMessages, "Cannot open the CDL file") == 1, "instanceChanged(reload) doesn't also send the error (messages: \"%s\")", gMessages.c_str());
    // a host without the persistent message (OFX 1.4 without OfxMessageSuiteV2): the error is sent instead
    {
        const auto setPersistent = gMsgSuiteV2.setPersistentMessage;
        gMsgSuiteV2.setPersistentMessage = nullptr;
        gPersistentError.clear();
        gMessages.clear();
        st = instanceChanged(p, inst, "reload");
        gMsgSuiteV2.setPersistentMessage = setPersistent;
        check(ok(st), "no persistent message suite: instanceChanged(reload) returns OK (status %d)", st);
        check(gPersistentError.empty() && (occurrences(gMessages, "Cannot open the CDL file") == 1),
              "no persistent message suite: instanceChanged(reload) sends the error (messages: \"%s\")", gMessages.c_str());
    }
    check(ctx.params["slope"].nSet == 0 && ctx.params["offset"].nSet == 0 && ctx.params["power"].nSet == 0 && ctx.params["saturation"].nSet == 0,
          "instanceChanged(reload) leaves the grade params unchanged");
    p->mainEntry(kOfxActionEndInstanceEdit, H(inst), nullptr, nullptr);

    p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
}

void
testRenderFromFile(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== Read from file: render applies the grade of the file (issue #280) ====\n");
    const std::string filename = dir + "/read.cc";
    writeCC(filename);
    Effect ctx, inst;
    OfxStatus st;
    if (!createReadFromFileInstance(p, ctx, inst, filename, &st)) {
        return;
    }
    check(ok(st), "createInstance with readFromFile=1 returns OK (status %d)", st);
    if (!ok(st)) {
        return;
    }
    check(setValueCount(ctx) == 0, "createInstance sets no param (%d paramSetValue calls)", setValueCount(ctx));

    // the grade params still have their default (identity) values: the effect is not an identity
    int before = setValueCount(ctx);
    st = isIdentity(p, inst);
    check(st == kOfxStatReplyDefault, "isIdentity: not an identity, the grade comes from the file (status %d)", st);
    check(setValueCount(ctx) == before, "isIdentity sets no param (%d paramSetValue calls)", setValueCount(ctx) - before);

    checkRenderApplies(p, ctx, inst, kGrade, "render 1");
    checkRenderApplies(p, ctx, inst, kGrade, "render 2");

    // the user presses Reload: kOfxActionInstanceChanged loads the file into the params
    const int slopeSets = ctx.params["slope"].nSet;
    const int saturationSets = ctx.params["saturation"].nSet;
    st = instanceChanged(p, inst, "reload");
    check(ok(st), "instanceChanged(reload) returns OK (status %d)", st);
    check(gPersistentError.empty(), "instanceChanged(reload): no persistent error (got \"%s\")", gPersistentError.c_str());
    const bool loaded = (ctx.params["slope"].nSet > slopeSets) && (ctx.params["saturation"].nSet > saturationSets) && paramsAre(ctx, kGrade);
    check(loaded, "instanceChanged(reload) loads the grade of the file into the params");

    p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
    std::remove(filename.c_str());
}

void
testReadFromFileOff(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== Read from file turned off: the params get the grade of the file ====\n");
    const std::string filename = dir + "/off.cc";
    writeCC(filename);
    Effect ctx, inst;
    OfxStatus st;
    if (!createReadFromFileInstance(p, ctx, inst, filename, &st) || !ok(st)) {
        check(false, "createInstance (status %d)", st);

        return;
    }
    check(!paramsAre(ctx, kGrade), "the params don't have the grade of the file yet");
    setOverrideInt(&ctx, "readFromFile", 0);
    st = instanceChanged(p, inst, "readFromFile");
    check(ok(st), "instanceChanged(readFromFile) returns OK (status %d)", st);
    check(paramsAre(ctx, kGrade), "the params have the grade of the file, so the output doesn't change");
    checkRenderApplies(p, ctx, inst, kGrade, "render from the params");
    p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
    std::remove(filename.c_str());
}

void
testExportFromFile(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== Export with Read from file on: the grade of the file is exported ====\n");
    const std::string filename = dir + "/source.cc";
    const std::string exported = dir + "/exported.cc";
    writeCC(filename, kGrade2);
    Effect ctx, inst;
    OfxStatus st;
    if (!createReadFromFileInstance(p, ctx, inst, filename, &st) || !ok(st)) {
        check(false, "createInstance (status %d)", st);

        return;
    }
    gMessages.clear();
    st = exportTo(p, ctx, inst, exported);
    check(ok(st), "instanceChanged(export) returns OK (status %d)", st);
    check(gMessages.empty(), "no error message (got \"%s\")", gMessages.c_str());
    std::string error;
    OCIO::ConstCDLTransformRcPtr cdl = readCDL(exported, &error);
    check((bool)cdl, "OCIO reads a CDL from the exported file (%s)", cdl ? "ok" : error.c_str());
    if (cdl) {
        double sop[9];
        cdl->getSOP(sop);
        bool same = closeEnough(cdl->getSat(), kGrade2.saturation);
        for (int k = 0; k < 9; ++k) {
            same = same && closeEnough(sop[k], kGrade2.sop[k]);
        }
        check(same, "the exported grade is the file's (not the params')");
    }
    p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
    std::remove(filename.c_str());
    std::remove(exported.c_str());
}

void
testCCC(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== Read from a .ccc file: the CCC Id selects the grade ====\n");
    const std::string filename = dir + "/grades.ccc";
    writeCCC(filename);
    {
        Effect ctx, inst;
        OfxStatus st;
        if (createReadFromFileInstance(p, ctx, inst, filename, &st, "second") && ok(st)) {
            checkRenderApplies(p, ctx, inst, kGrade2, "cccId \"second\"");
            st = instanceChanged(p, inst, "cccId");
            check(ok(st), "instanceChanged(cccId) returns OK (status %d)", st);
            check(paramsAre(ctx, kGrade2), "instanceChanged(cccId) loads the grade of \"second\" into the params");
            p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
        } else {
            check(false, "createInstance (status %d)", st);
        }
    }
    {
        Effect ctx, inst;
        OfxStatus st;
        if (createReadFromFileInstance(p, ctx, inst, filename, &st, "third") && ok(st)) {
            gPersistentError.clear();
            st = render(p, ctx, inst);
            check(!ok(st), "cccId \"third\" (not in the file): render fails (status %d)", st);
            check(!gPersistentError.empty(), "cccId \"third\": persistent error (\"%s\")", gPersistentError.c_str());
            p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
        } else {
            check(false, "createInstance (status %d)", st);
        }
    }
    std::remove(filename.c_str());
}

void
testFileAppears(OfxPlugin* p, const std::string& dir)
{
    std::printf("==== Read from file: a missing file that appears is read by the next render, without Reload ====\n");
    const std::string filename = dir + "/later.cc";
    Effect ctx, inst;
    OfxStatus st;
    if (!createReadFromFileInstance(p, ctx, inst, filename, &st) || !ok(st)) {
        check(false, "createInstance (status %d)", st);

        return;
    }
    gPersistentError.clear();
    st = render(p, ctx, inst);
    check(!ok(st), "the file is missing: render fails (status %d)", st);
    writeCC(filename, kGrade2);
    checkRenderApplies(p, ctx, inst, kGrade2, "the file was created");
    p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
    std::remove(filename.c_str());
}

} // namespace

int
main(int, char**)
{
    if (!std::getenv("OCIO")) {
        std::printf("FAIL: $OCIO is not set (see tests/Makefile)\n");

        return 1;
    }
    initHost();
    OfxPlugin* p = findPlugin(kPluginIdentifier);
    if (!p) {
        std::printf("FAIL: plugin %s not found\n", kPluginIdentifier);

        return 1;
    }
    Effect describeEffect, ctx, inst;
    if (loadAndDescribe(p, describeEffect, ctx, kOfxImageEffectContextFilter) != 0) {
        std::printf("FAIL: load/describe\n");

        return 1;
    }
    setOverrideDoubles(&ctx, "slope", { kSlope[0], kSlope[1], kSlope[2] });
    setOverrideDoubles(&ctx, "offset", { kOffset[0], kOffset[1], kOffset[2] });
    setOverrideDoubles(&ctx, "power", { kPower[0], kPower[1], kPower[2] });
    setOverrideDoubles(&ctx, "saturation", { kSaturation });
    inst.delegate = &ctx;
    setInstanceProps(inst, 8, 8, 1, kOfxImageEffectContextFilter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, H(inst), nullptr, nullptr))) {
        std::printf("FAIL: createInstance\n");

        return 1;
    }

    // a new scratch directory in the temporary directory
    std::error_code ec;
    std::filesystem::path dirPath = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dirPath = ".";
    }
    dirPath /= "ocio_cdl_test." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (!std::filesystem::create_directory(dirPath, ec) || ec) {
        std::printf("FAIL: cannot create directory %s\n", dirPath.string().c_str());

        return 1;
    }
    const std::string dir = dirPath.generic_string();

    testDefaultFilename(p, ctx, inst, dir);
    testExport(p, ctx, inst, dir);
    testExistingFile(p, ctx, inst, dir);
    testInverse(p, ctx, inst, dir);
    testUnwritable(p, ctx, inst, dir);
    testMissingFile(p, dir);
    testRenderFromFile(p, dir);
    testReadFromFileOff(p, dir);
    testExportFromFile(p, dir);
    testCCC(p, dir);
    testFileAppears(p, dir);

    p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::filesystem::remove_all(dirPath, ec);

    std::printf("\n%s (%d failure%s)\n", gFailures ? "FAILED" : "ALL PASSED", gFailures, gFailures == 1 ? "" : "s");

    return gFailures ? 1 : 0;
}
