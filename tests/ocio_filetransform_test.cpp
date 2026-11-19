/*
 * ocio_filetransform_test — OCIOFileTransform's CCC Id param (OCIO/OCIOFileTransform.cpp), built with AddressSanitizer
 * (tests/Makefile: test-ocio-filetransform).
 *
 * The CCC Id param is shown only for a .cdl, .cc or .ccc file. updateCCCId() compared the end of the file name with "ccc"
 * and "cc" by std::equal on the reversed strings, which reads before the start of a file name shorter than the
 * extension: the default file name is empty, so every new instance did (an ASan stack-buffer-underflow, or a heap
 * one for a longer std::string).
 * Create an instance (empty file name), then change the file name to names shorter than, as long as, and longer than
 * the extensions: the CCC Id param must be shown for the .cdl, .cc and .ccc files only (the extension after the last
 * '.', in any case, as OCIO's FileTransform: "Load the CDL from the src .cdl, .cc, or .ccc file"), not for "cc",
 * "ccc" or "x.icc".
 * On libstdc++ (Linux), the read before an empty or short std::string stays in the string object itself (its
 * short-string buffer follows its pointer and size), which ASan doesn't report: there, only libc++ (macOS) catches the
 * old bug. The test still checks which files show the param.
 *
 * The plug-in is linked directly and driven through the OFX actions by the in-process host of ofx_test_host.h.
 * Usage: ocio_filetransform_test   (needs $OCIO, see tests/Makefile)
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// from OCIO/OCIOFileTransform.cpp
const char* const kPluginIdentifier = "fr.inria.openfx.OCIOFileTransform";
const char* const kParamFile = "file";
const char* const kParamCCCID = "cccId";

OfxImageEffectHandle
H(Effect& e)
{
    return reinterpret_cast<OfxImageEffectHandle>(&e);
}

// is the param secret (kOfxParamPropSecret, set by the instance on the param of the context descriptor)?
bool
isSecret(Effect& ctx,
         const char* name)
{
    auto it = ctx.params.find(name);
    if (it == ctx.params.end()) {
        return false;
    }
    int v = 0;
    pGetInt(PH(it->second.props), kOfxParamPropSecret, 0, &v);

    return v != 0;
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
    if (!ctx.params.count(kParamFile) || !ctx.params.count(kParamCCCID)) {
        std::printf("FAIL: no %s or %s param\n", kParamFile, kParamCCCID);

        return 1;
    }
    inst.delegate = &ctx;
    setInstanceProps(inst, 8, 8, 1, kOfxImageEffectContextFilter);
    // (the constructor calls updateCCCId() with the default, empty, file name)
    if (!ok(p->mainEntry(kOfxActionCreateInstance, H(inst), nullptr, nullptr))) {
        std::printf("FAIL: createInstance\n");

        return 1;
    }
    int failures = 0;
    if (!isSecret(ctx, kParamCCCID)) {
        std::printf("FAIL: the %s param is shown for an empty file name\n", kParamCCCID);
        ++failures;
    }
    const struct {
        const char* file;
        bool shown;
    } cases[] = {
        { "", false }, { "a", false }, { "c", false }, { "cc", false }, { "ccc", false }, { "x.cc", true },
        { "x.ccc", true }, { "x.cdl", true }, { "x.CCC", true }, { "x.Cc", true }, { "x.icc", false }, { "xcc", false },
        { "x.cube", false }, { "x.3dl", false }, { "a/b/lut.csp", false },
    };
    for (const auto& c : cases) {
        setOverrideString(&ctx, kParamFile, c.file);
        PropSet a;
        pSetString(PH(&a), kOfxPropType, 0, kOfxTypeParameter);
        pSetString(PH(&a), kOfxPropName, 0, kParamFile);
        pSetString(PH(&a), kOfxPropChangeReason, 0, kOfxChangeUserEdited);
        pSetDouble(PH(&a), kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. };
        pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
        const OfxStatus st = p->mainEntry(kOfxActionInstanceChanged, H(inst), PH(&a), nullptr);
        const bool shown = !isSecret(ctx, kParamCCCID);
        const bool pass = ok(st) && (shown == c.shown);
        std::printf("  %s: file \"%s\": %s %s (status %d)\n", pass ? "PASS" : "FAIL", c.file, kParamCCCID, shown ? "shown" : "hidden", st);
        if (!pass) {
            ++failures;
        }
    }
    p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

    std::printf("Result: failures=%d\n", failures);
    std::printf(failures == 0 ? "PASS\n" : "FAIL\n");

    return failures == 0 ? 0 : 1;
}
