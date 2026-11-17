/*
 * ocio_cdl_test — OCIOCDLTransform's Export param (OCIO/OCIOCDLTransform.cpp, issue #105).
 *
 * Setting the Export string param (a user edit) must write the grade as a ColorCorrection
 * XML file (.cc) that OCIO reads back with the same slope, offset, power and saturation,
 * must not overwrite an existing file, and must reset the param to its default.
 * (With OCIO >= 2, Export used to fail: GroupTransform::write() was called with a
 * nonexistent "CDL" format on an empty group, leaving an empty file, and the error path
 * called fclose(NULL).)
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
#include <sstream>
#include <string>
#include <system_error>

namespace OCIO = OCIO_NAMESPACE;

namespace {

// from OCIO/OCIOCDLTransform.cpp
const char* const kPluginIdentifier = "fr.inria.openfx.OCIOCDLTransform";
const char* const kParamExport = "export";
const char* const kParamExportDefault = "Set filename to export this grade as .cc";

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
near(double a, double b)
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
            same = same && near(sop[c], kSlope[c]) && near(sop[3 + c], kOffset[c]) && near(sop[6 + c], kPower[c]);
        }
        check(same, "slope (%g %g %g), offset (%g %g %g), power (%g %g %g) read back",
              sop[0], sop[1], sop[2], sop[3], sop[4], sop[5], sop[6], sop[7], sop[8]);
        check(near(cdl->getSat(), kSaturation), "saturation %g read back", cdl->getSat());
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

    testExport(p, ctx, inst, dir);
    testExistingFile(p, ctx, inst, dir);

    p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::filesystem::remove_all(dirPath, ec);

    std::printf("\n%s (%d failure%s)\n", gFailures ? "FAILED" : "ALL PASSED", gFailures, gFailures == 1 ? "" : "s");

    return gFailures ? 1 : 0;
}
