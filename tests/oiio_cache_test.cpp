/*
 * oiio_cache_test — ReadOIIO reads the pixels through OpenImageIO's ImageCache only for an interactive,
 * non-sequential render (kOfxImageEffectPropInteractiveRenderStatus = 1 and
 * kOfxImageEffectPropSequentialRenderStatus = 0), driven by the in-process host of ofx_test_host.h.
 *
 * ReadOIIO only creates its (shared) ImageCache in Natron < 2.2, so the host says it is Natron 2.1. A TIFF
 * file is rendered with each combination of the two statuses, and with none of them (an OFX 1.1 host);
 * the cache was used if its "stat:find_tile_calls" counter increased during the render.
 *
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include "ofxNatron.h"

#include <OpenImageIO/imagecache.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/oiioversion.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void
check(bool cond, const char* fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    std::printf("  %s: ", cond ? "PASS" : "FAIL");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    if (!cond) {
        ++gFailures;
    }
}

const int kWidth = 32, kHeight = 16;

bool
writeTiff(const std::string& filename)
{
    auto out = OIIO::ImageOutput::create(filename);
    if (!out) {
        return false;
    }
    OIIO::ImageSpec spec(kWidth, kHeight, 4, OIIO::TypeDesc::FLOAT);
    std::vector<float> pixels((size_t)kWidth * kHeight * 4, 0.25f);
    const bool ok = out->open(filename, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) && out->close();

    return ok;
}

// the shared ImageCache's find_tile_calls statistic: it increases each time pixels are read through the cache
long long
findTileCalls()
{
    auto cache = OIIO::ImageCache::create(true); // the shared cache, which ReadOIIO uses
    // documented as an int, but some OpenImageIO versions store it as an int64
    long long n = -1;
    int i = -1;
    if (cache->getattribute("stat:find_tile_calls", OIIO::TypeDesc::INT, &i)) {
        n = i;
    } else if (!cache->getattribute("stat:find_tile_calls", OIIO::TypeDesc::INT64, &n)) {
        n = -1;
    }
#if OIIO_VERSION < 30000
    OIIO::ImageCache::destroy(cache);
#endif

    return n;
}

} // namespace

int
main(int argc, char** argv)
{
    initHost();
    // ReadOIIO uses the OpenImageIO cache only in Natron < 2.2 (ReadOIIOPluginFactory::createInstance)
    pSetInt(PH(&gHostProps), kNatronOfxHostIsNatron, 0, 1);
    pSetString(PH(&gHostProps), kOfxPropName, 0, "fr.inria.Natron");
    pSetInt(PH(&gHostProps), kOfxPropVersion, 0, 2);
    pSetInt(PH(&gHostProps), kOfxPropVersion, 1, 1);
    pSetInt(PH(&gHostProps), kOfxPropVersion, 2, 0);

    const std::string filename = std::string((argc > 1) ? argv[1] : "/tmp/oiio_cache_test") + ".tif";
    if (!writeTiff(filename)) {
        std::printf("FAIL: cannot write %s\n", filename.c_str());

        return 1;
    }

    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadOIIO");
    if (!p) {
        std::printf("FAIL: ReadOIIO not found\n");

        return 1;
    }
    Effect desc, ctx, inst;
    if (loadAndDescribe(p, desc, ctx, kOfxImageEffectContextReader) != 0) {
        std::printf("FAIL: ReadOIIO load/describe\n");

        return 1;
    }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, filename);
    inst.delegate = &ctx;
    setInstanceProps(inst, kWidth, kHeight, 1, kOfxImageEffectContextReader);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) {
        std::printf("FAIL: ReadOIIO createInstance\n");

        return 1;
    }
    {
        // the user set the filename
        PropSet a;
        OfxPropertySetHandle ap = PH(&a);
        pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
        pSetString(ap, kOfxPropName, 0, kOfxImageEffectFileParamName);
        pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
        pSetDouble(ap, kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. };
        pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        check(ok(p->mainEntry(kOfxActionInstanceChanged, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr)), "instanceChanged(filename)");
    }
    Clip* out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
    if (!out) {
        std::printf("FAIL: no output clip\n");

        return 1;
    }
    out->connected = true;
    setupClipImage(out, kWidth, kHeight, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);

    std::printf("==== ReadOIIO uses the OpenImageIO cache only for interactive, non-sequential renders ====\n");
    const struct {
        int sequential, interactive; // -1: not set
        bool cached;
        const char* what;
    } cases[] = {
        { 0, 1, true, "interactive (scrubbing, paused viewer)" },
        { 1, 0, false, "sequential (disk render)" },
        { 0, 0, false, "neither (playback, out-of-order disk render)" },
        { 1, 1, false, "both" },
        { -1, -1, false, "not set (OFX 1.1 host)" },
        { 0, 1, true, "interactive again" },
    };
    for (const auto& c : cases) {
        PropSet a = makeRenderInArgs(1., kWidth, kHeight);
        if (c.sequential < 0) {
            a.props.erase(kOfxImageEffectPropSequentialRenderStatus);
        } else {
            pSetInt(PH(&a), kOfxImageEffectPropSequentialRenderStatus, 0, c.sequential);
        }
        if (c.interactive < 0) {
            a.props.erase(kOfxImageEffectPropInteractiveRenderStatus);
        } else {
            pSetInt(PH(&a), kOfxImageEffectPropInteractiveRenderStatus, 0, c.interactive);
        }
        float* d = reinterpret_cast<float*>(out->buffer.data());
        d[0] = -1.f;
        const long long before = findTileCalls();
        OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
        const long long after = findTileCalls();
#if OIIO_VERSION >= 10605
        const bool expected = c.cached;
#else
        const bool expected = false; // ReadOIIO never reads pixels through the cache with OIIO 1.5
#endif
        check(ok(st) && d[0] == 0.25f && before >= 0 && (after > before) == expected,
              "sequential %d, interactive %d (%s): render = %d, R = %g, find_tile_calls %lld -> %lld (cache %s, expected %s)",
              c.sequential, c.interactive, c.what, st, d[0], before, after, after > before ? "used" : "not used",
              expected ? "used" : "not used");
    }

    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::remove(filename.c_str());

    std::printf("Result: failures=%d\n", gFailures);
    std::printf(gFailures == 0 ? "PASS\n" : "FAIL\n");

    return gFailures == 0 ? 0 : 1;
}
