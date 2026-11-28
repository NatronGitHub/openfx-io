/*
 * ocio_processor_cache_test — the processor caches of the OCIO plug-ins (IOSupport/OCIOProcessorCache.h, used by
 * OCIO/OCIOCDLTransform.cpp, OCIODisplay.cpp, OCIOFileTransform.cpp, OCIOLogConvert.cpp, OCIOLookTransform.cpp; issues
 * #282, #283, #284), and the config of GenericOCIO (OCIOColorSpace, #275).
 *
 * 1. Concurrent renders (#282): the plug-ins are eRenderFullySafe, so a host may render an instance from several
 *    threads at once. For each plug-in, a param that changes the processor is animated (it has one value at odd
 *    frames, another at even ones), and 4 threads render the instance at alternating frames, each its own row of the
 *    output image. Every row must have the value of its frame. Before the fix, OCIODisplay, OCIOFileTransform and
 *    OCIOLogConvert copied the cached processor after releasing its mutex, while another thread replaced it: a data
 *    race (reported by ThreadSanitizer: ctest test-ocio-processor-cache-tsan) that can also apply the processor of the
 *    other frame. (OCIOLogConvert replaced it at every render: its key was never set.) Also, the main thread changes
 *    the OCIO config file param of OCIOLogConvert, OCIOColorSpace and OCIODisplay (GenericOCIO, #275) while 3 threads
 *    render: their config was reset and reloaded without a lock. OCIOColorSpace is also rendered with its Inverse
 *    param animated.
 *    And OCIOProcessorCache itself: a hit doesn't build (a build counter), a miss replaces the processor, a failed
 *    build leaves the cache unchanged, concurrent gets of one key build it at most once per thread.
 * 2. A changed config or context gives a new processor (#283, #284): OCIODisplay and OCIOLookTransform render with
 *    the colorspace "ctx", whose transform is the file "scale${S}.cc" (a CDL slope of S), then the context param
 *    sets S to 3, and OCIODisplay is rendered again after its OCIO config file param is set to another config. Before
 *    the fix, OCIODisplay kept the processor of the old context or config, and OCIOLookTransform ignored the context
 *    params.
 * 3. Reload re-reads a modified LUT file (#361): OCIOFileTransform renders a CDL file with a slope of 2, the file is
 *    rewritten with a slope of 3, and the Reload button is pressed: the next render must use the new slope. Before the
 *    fix, Reload cleared OCIO's file caches (OCIO::ClearAllCaches()) but not the processor cache of the config, which
 *    returned the processor of the old file contents.
 * 4. Purging the caches re-reads a modified LUT file of a colorspace (#402): OCIOColorSpace, OCIODisplay and
 *    OCIOLookTransform render the colorspace "lut", whose transform is the file purge.cc (a CDL slope of 2), the file is
 *    rewritten with a slope of 3, and the host purges the caches (kOfxActionPurgeCaches): the next render must use the
 *    new slope. Before the fix, none of them handled the action, and GenericOCIO::purgeCaches() (the readers and
 *    writers) cleared OCIO's file caches but not the processor caches of its config and its instance. Then the main
 *    thread purges the caches while 3 threads render (ctest test-ocio-processor-cache-tsan checks the data races).
 * 5. An OCIO 2 view (#354): the display "none" of the configs has the shared view Film, whose view transform scales by
 *    4. OCIODisplay must render it. (Its View menu is only built in Natron, which this host is not.)
 *
 * The test writes its OCIO configs and CDL files in <dir> and sets $OCIO to the first config.
 * The plug-ins are linked directly and driven through the OFX actions by the in-process host of ofx_test_host.h.
 * Usage: ocio_processor_cache_test [--no-purge-during-renders] <dir>
 * --no-purge-during-renders: without the purges while 3 threads render (part 4), whose only ThreadSanitizer reports
 * with uninstrumented OpenColorIO libraries are false positives in OCIO's file cache (CI's
 * test-ocio-processor-cache-renders-tsan; the concurrent renders of the other parts, e.g. the GetEnvVariable() race of
 * OpenColorIO < 2.6 that OCIOOptimizedProcessorLock fixes, are still checked).
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include "OCIOProcessorCache.h"

#include <OpenColorIO/OpenColorIO.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

const int kW = 4;
const int kThreads = 4; // (one row each: the image is kW x kThreads)
const int kRendersPerThread = 100;
const float kInput = 0.25f;

bool
writeFile(const std::string& path,
          const std::string& contents)
{
    std::ofstream ofs(path.c_str(), std::ios::binary);
    ofs << contents;

    return static_cast<bool>(ofs);
}

// xN is N times the reference x1 (x2: kX2 times); "ctx" is the file scale${S}.cc, S = 2 by default (a context
// variable); "lut" is the file purge.cc; the look "double" multiplies by 2; the shared view Film of the display
// "none" is the view transform "film" (x4, an OCIO 2 view, #354) to the display colorspace "disp"
std::string
configText(int kX2)
{
    const std::string x2 = std::to_string(kX2);

    return "ocio_profile_version: 2\n"
           "search_path: \".\"\n"
           "strictparsing: true\n"
           "environment: {S: \"2\"}\n"
           "roles: {default: x1, reference: x1, scene_linear: x1, compositing_log: x2, data: x1}\n"
           "displays:\n"
           "  none:\n"
           "    - !<View> {name: Raw, colorspace: x1}\n"
           "    - !<Views> [Film]\n"
           "active_displays: [none]\n"
           "active_views: [Raw, Film]\n"
           "shared_views:\n"
           "  - !<View> {name: Film, view_transform: film, display_colorspace: disp}\n"
           "view_transforms:\n"
           "  - !<ViewTransform> {name: film, from_scene_reference: !<MatrixTransform> {matrix: [4, 0, 0, 0, 0, 4, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1]}}\n"
           "display_colorspaces:\n"
           "  - !<ColorSpace> {name: disp, family: display, bitdepth: 32f, isdata: false}\n"
           "looks:\n"
           "  - !<Look> {name: double, process_space: x1, transform: !<MatrixTransform> {matrix: [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1]}}\n"
           "colorspaces:\n"
           "  - !<ColorSpace> {name: x1, family: scale, bitdepth: 32f, isdata: false}\n"
           "  - !<ColorSpace> {name: x2, family: scale, bitdepth: 32f, isdata: false, to_scene_reference: !<MatrixTransform> {matrix: ["
        + x2 + ", 0, 0, 0, 0, " + x2 + ", 0, 0, 0, 0, " + x2 + ", 0, 0, 0, 0, 1]}}\n"
           "  - !<ColorSpace> {name: ctx, family: scale, bitdepth: 32f, isdata: false, to_scene_reference: !<FileTransform> {src: \"scale${S}.cc\"}}\n"
           "  - !<ColorSpace> {name: lut, family: scale, bitdepth: 32f, isdata: false, to_scene_reference: !<FileTransform> {src: \"purge.cc\"}}\n";
}

std::string
ccText(int slope)
{
    const std::string s = std::to_string(slope);

    return "<ColorCorrection id=\"s" + s + "\">\n"
           "  <SOPNode><Slope>" + s + " " + s + " " + s + "</Slope><Offset>0 0 0</Offset><Power>1 1 1</Power></SOPNode>\n"
           "  <SatNode><Saturation>1</Saturation></SatNode>\n"
           "</ColorCorrection>\n";
}

ParamRec&
param(Effect& ctx,
      const char* name)
{
    auto it = ctx.params.find(name);
    if (it == ctx.params.end()) {
        std::printf("FAIL: no param %s\n", name);
        std::exit(1);
    }

    return it->second;
}

void
setInt(Effect& ctx,
       const char* name,
       int v)
{
    ParamRec& r = param(ctx, name);
    r.hasOv = true;
    r.ov.i = { v };
}

void
setString(Effect& ctx,
          const char* name,
          const std::string& v)
{
    ParamRec& r = param(ctx, name);
    r.hasOv = true;
    r.ov.s = { v };
}

void
setDoubles(Effect& ctx,
           const char* name,
           std::vector<double> v)
{
    ParamRec& r = param(ctx, name);
    r.hasOv = true;
    r.ov.d = std::move(v);
}

// an instance of a plug-in in the filter context, with its source and output images (kInput, opaque)
struct Instance {
    OfxPlugin* p = nullptr;   // (non-null: loaded, to be unloaded)
    bool created = false;     // (to be destroyed)
    Effect describe, ctx, inst;

    // (a failure is a FAIL)
    bool load(const char* id)
    {
        OfxPlugin* plugin = findPlugin(id);
        if (!plugin) {
            check(false, std::string("plugin ") + id + " found");

            return false;
        }
        if (loadAndDescribe(plugin, describe, ctx, kOfxImageEffectContextFilter) != 0) {
            check(false, std::string(id) + ": load and describe");

            return false;
        }
        p = plugin;
        inst.delegate = &ctx;
        setInstanceProps(inst, kW, kThreads, 10, kOfxImageEffectContextFilter);
        Clip* src = ctx.clips[kOfxImageEffectSimpleSourceClipName];
        Clip* dst = ctx.clips[kOfxImageEffectOutputClipName];
        src->connected = true;
        dst->connected = true;
        setupClipImage(src, kW, kThreads, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
        setupClipImage(dst, kW, kThreads, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
        float* s = reinterpret_cast<float*>(src->buffer.data());
        for (int i = 0; i < kW * kThreads; ++i) {
            s[4 * i] = s[4 * i + 1] = s[4 * i + 2] = kInput;
            s[4 * i + 3] = 1.f;
        }

        return true;
    }

    bool create()
    {
        created = ok(p->mainEntry(kOfxActionCreateInstance, EH(inst), nullptr, nullptr));

        return created;
    }

    ~Instance()
    {
        if (created) {
            p->mainEntry(kOfxActionDestroyInstance, EH(inst), nullptr, nullptr);
        }
        if (p) {
            p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
        }
    }

    // the user edited the param name (kOfxActionInstanceChanged)
    OfxStatus changed(const char* name)
    {
        return instanceChanged(p, inst, name);
    }

    // the host purges its caches (kOfxActionPurgeCaches)
    OfxStatus purgeCaches() { return p->mainEntry(kOfxActionPurgeCaches, EH(inst), nullptr, nullptr); }

    // render row y of the output at time t; true if it succeeded and the row is kInput * f (RGB) for one f of factors
    bool renderRowAnyOf(double t,
                        int y,
                        const std::vector<double>& factors)
    {
        float* d = reinterpret_cast<float*>(ctx.clips.at(kOfxImageEffectOutputClipName)->buffer.data()) + 4 * kW * y;
        fillUnwritten(d, 4 * kW);
        PropSet a = makeRenderInArgs(t, kW, kThreads);
        int win[4] = { 0, y, kW, y + 1 };
        pSetIntN(PH(&a), kOfxImageEffectPropRenderWindow, 4, win);
        if (!ok(p->mainEntry(kOfxImageEffectActionRender, EH(inst), PH(&a), nullptr))) {
            return false;
        }
        for (double factor : factors) {
            const float expected = static_cast<float>(kInput * factor);
            bool same = true;
            for (int i = 0; i < 4 * kW; ++i) {
                if ((i % 4) != 3 && (isUnwritten(&d[i]) || !(std::fabs(d[i] - expected) <= 1e-5f))) {
                    same = false;
                }
            }
            if (same) {
                return true;
            }
        }

        return false;
    }

    // render row y of the output at time t; true if it succeeded and the row is kInput * factor (RGB)
    bool renderRow(double t,
                   int y,
                   double factor)
    {
        return renderRowAnyOf(t, y, { factor });
    }

    // the value of row 0 at time 1 (NaN bits if unwritten: then any comparison fails)
    float render1()
    {
        renderRow(1., 0, 1.);

        return reinterpret_cast<const float*>(ctx.clips.at(kOfxImageEffectOutputClipName)->buffer.data())[0];
    }
};

// 1. Concurrent renders: kThreads threads render inst at times 1 and 2, alternately, where the animated param gives
// the factors f1 and f2
void
testConcurrentRenders(const char* what,
                      Instance& in,
                      double f1,
                      double f2)
{
    std::atomic<int> bad { 0 };
    std::vector<std::thread> threads;
    for (int k = 0; k < kThreads; ++k) {
        threads.emplace_back([&in, &bad, k, f1, f2] {
            for (int i = 0; i < kRendersPerThread; ++i) {
                const bool odd = ((i + k) % 2) == 0;
                if (!in.renderRow(odd ? 1. : 2., k, odd ? f1 : f2)) {
                    ++bad;
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    check(bad == 0, std::string(what) + ": " + std::to_string(kThreads) + " threads x " + std::to_string(kRendersPerThread)
                        + " renders at alternating frames: every row has the value of its frame (" + std::to_string(bad.load()) + " wrong)");
}

// The host purges the caches of in (what) kPurges times on the main thread while kThreads - 1 threads render, with
// a factor of f (the LUT file doesn't change meanwhile): no data race (purgeCaches() swaps in a copy of the config
// under the lock, and each render uses one config), and every row is f.
void
testPurgeDuringRenders(const std::string& what,
                       Instance& in,
                       double f)
{
    const int kPurges = 40;
    std::atomic<bool> stop { false };
    std::atomic<int> bad { 0 };
    std::atomic<int> renders { 0 };
    std::vector<std::thread> threads;
    for (int k = 1; k < kThreads; ++k) {
        threads.emplace_back([&in, &stop, &bad, &renders, k, f] {
            while (!stop) {
                if (!in.renderRow(1., k, f)) {
                    ++bad;
                }
                ++renders;
            }
        });
    }
    int failedPurges = 0;
    for (int i = 0; i < kPurges; ++i) {
        if (!ok(in.purgeCaches())) {
            ++failedPurges;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop = true;
    for (std::thread& t : threads) {
        t.join();
    }
    check(failedPurges == 0, what + ": " + std::to_string(kPurges) + " purges of the caches during renders succeed");
    check(renders > 0, what + ": renders ran during the purges (" + std::to_string(renders.load()) + ")");
    check(bad == 0, what + ": " + std::to_string(renders.load()) + " renders during the purges: every row has the new slope (" + std::to_string(bad.load()) + " wrong)");
}

// The OCIO config file param of in (what) is set to the other config (kToggles times) by the main thread while
// kThreads - 1 threads render, with a factor of fA with configA (a.ocio, the first one) and fB with configB: no data race (the config is swapped
// in under a lock, and each render uses one config), every row is one of the two, and the last config is used once the
// toggles are done.
void
testConfigChangeDuringRenders(const std::string& what,
                              Instance& in,
                              const std::string& configA,
                              const std::string& configB,
                              double fA,
                              double fB)
{
    const int kToggles = 40;
    std::atomic<bool> stop { false };
    std::atomic<int> bad { 0 };
    std::atomic<int> renders { 0 };
    std::vector<std::thread> threads;
    for (int k = 1; k < kThreads; ++k) {
        threads.emplace_back([&in, &stop, &bad, &renders, k, fA, fB] {
            while (!stop) {
                if (!in.renderRowAnyOf(1., k, { fA, fB })) {
                    ++bad;
                }
                ++renders;
            }
        });
    }
    int failedChanges = 0;
    for (int i = 0; i < kToggles; ++i) {
        {
            HostWrite lock; // (the renders read the params meanwhile)
            setString(in.ctx, "ocioConfigFile", (i % 2) == 0 ? configB : configA);
        }
        if (!ok(in.changed("ocioConfigFile"))) {
            ++failedChanges;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop = true;
    for (std::thread& t : threads) {
        t.join();
    }
    check(failedChanges == 0, what + ": " + std::to_string(kToggles) + " changes of the OCIO config file param during renders succeed");
    check(renders > 0, what + ": renders ran during the changes (" + std::to_string(renders.load()) + ")");
    check(bad == 0, what + ": " + std::to_string(renders.load()) + " renders during the changes: every row is the transform of a.ocio or b.ocio (" + std::to_string(bad.load()) + " wrong)");
    // (kToggles is even: the last config is a.ocio)
    check(in.renderRow(1., 0, fA), what + ": after the changes, the render uses the last config, a.ocio");
}

// OCIOProcessorCache itself: a hit doesn't build, a miss builds and replaces the cached processor, a build that throws
// leaves the cache unchanged; and concurrent gets of one key build it at most once per thread that missed it
void
testCacheDirectly()
{
    namespace OCIO = OCIO_NAMESPACE;
    const OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig();
    std::atomic<int> builds { 0 };
    auto builder = [&config, &builds](const char* dst) {
        return [&config, &builds, dst] {
            ++builds;
            return config->getProcessor("x1", dst);
        };
    };
    OFX::IO::OCIOProcessorCache<int> cache;
    OCIO::ConstProcessorRcPtr p1 = cache.get(1, builder("x2"));
    check(p1 && builds == 1, "a miss builds the processor");
    OCIO::ConstProcessorRcPtr p1b = cache.get(1, builder("x2"));
    check(p1b == p1 && builds == 1, "a hit returns the cached processor and doesn't build");
    OCIO::ConstProcessorRcPtr p2 = cache.get(2, builder("ctx"));
    check(p2 && p2 != p1 && builds == 2, "another key builds a new processor");
    bool threw = false;
    try {
        cache.get(3, []() -> OCIO::ConstProcessorRcPtr { throw OCIO::Exception("build failed"); });
    } catch (const OCIO::Exception&) {
        threw = true;
    }
    check(threw, "the exception of a build propagates");
    check(cache.get(2, builder("ctx")) == p2 && builds == 2, "after a failed build, the cache still has the last processor (a hit)");

    builds = 0;
    std::vector<std::thread> threads;
    std::atomic<int> different { 0 };
    for (int k = 0; k < kThreads; ++k) {
        threads.emplace_back([&] {
            for (int i = 0; i < kRendersPerThread; ++i) {
                if (!cache.get(4, builder("x2"))) {
                    ++different;
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    check(different == 0 && builds >= 1 && builds <= kThreads, std::to_string(kThreads) + " threads x " + std::to_string(kRendersPerThread)
                                                                  + " gets of one key: " + std::to_string(builds.load()) + " builds (at most one per thread)");
}

std::function<double(double)>
alternate(double v1,
          double v2)
{
    return [v1, v2](double t) { return (t < 1.5) ? v1 : v2; };
}

void
setEnv(const char* name,
       const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::string
str(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);

    return buf;
}

} // namespace

int
main(int argc,
     char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // (line-buffered: a crash keeps the lines before it, under ctest or a pipe)
    bool purgeDuringRenders = true;
    int arg = 1;
    if (arg < argc && std::string(argv[arg]) == "--no-purge-during-renders") {
        purgeDuringRenders = false;
        ++arg;
    }
    if (arg + 1 != argc) {
        std::printf("usage: %s [--no-purge-during-renders] <dir>\n", argv[0]);

        return 2;
    }
    const std::string dir = argv[arg];
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string configA = dir + "/a.ocio";
    const std::string configB = dir + "/b.ocio";
    const std::string cc = dir + "/scale2.cc";
    if (!writeFile(configA, configText(2)) || !writeFile(configB, configText(3)) || !writeFile(cc, ccText(2))
        || !writeFile(dir + "/scale3.cc", ccText(3))) {
        std::printf("FAIL: cannot write in %s\n", dir.c_str());

        return 1;
    }
    setEnv("OCIO", configA); // (before OCIO reads it, and the plug-ins' descriptions)
    initHost();

    std::printf("==== 1. concurrent renders, with a param that changes the processor at each frame (#282) ====\n");
    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIOCDLTransform")) {
            setDoubles(in.ctx, "slope", { 2., 2., 2. });
            param(in.ctx, "direction").numberAtTime = alternate(0, 1); // forward, inverse
            check(in.create(), "OCIOCDLTransform: create instance");
            testConcurrentRenders("OCIOCDLTransform (direction)", in, 2., 0.5);
        }
    }
    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIODisplay")) {
            setString(in.ctx, "display", "none");
            setString(in.ctx, "view", "Raw");
            setString(in.ctx, "ocioInputSpace", "x1");
            param(in.ctx, "gain").numberAtTime = alternate(1., 2.);
            check(in.create(), "OCIODisplay: create instance");
            testConcurrentRenders("OCIODisplay (gain)", in, 1., 2.);
        }
    }
    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIOFileTransform")) {
            setString(in.ctx, "file", cc);
            param(in.ctx, "direction").numberAtTime = alternate(0, 1);
            check(in.create(), "OCIOFileTransform: create instance");
            testConcurrentRenders("OCIOFileTransform (direction)", in, 2., 0.5);
        }
    }
    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIOLogConvert")) {
            // log2lin: compositing_log (x2) to scene_linear (x1); lin2log
            param(in.ctx, "operation").numberAtTime = alternate(0, 1);
            check(in.create(), "OCIOLogConvert: create instance");
            testConcurrentRenders("OCIOLogConvert (operation)", in, 2., 0.5);
        }
    }
    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIOLookTransform")) {
            setInt(in.ctx, "singleLook", 1);
            setInt(in.ctx, "lookChoice", 0); // "double"
            setString(in.ctx, "ocioInputSpace", "x1");
            setString(in.ctx, "ocioOutputSpace", "x1");
            param(in.ctx, "direction").numberAtTime = alternate(0, 1);
            check(in.create(), "OCIOLookTransform: create instance");
            testConcurrentRenders("OCIOLookTransform (direction)", in, 2., 0.5);
        }
    }
    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIOColorSpace")) {
            // (GenericOCIO's processor: getOrCreateProcessor() returns the one it got, not the last one cached)
            setString(in.ctx, "ocioInputSpace", "x2");
            setString(in.ctx, "ocioOutputSpace", "x1");
            param(in.ctx, "ocioInverse").numberAtTime = alternate(0, 1);
            check(in.create(), "OCIOColorSpace: create instance");
            testConcurrentRenders("OCIOColorSpace (inverse)", in, 2., 0.5);
        }
    }

    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIOLogConvert")) {
            param(in.ctx, "operation").numberAtTime = [](double) { return 0.; }; // log2lin
            check(in.create(), "OCIOLogConvert: create instance");
            // (log2lin: x2 of a.ocio, 2, or of b.ocio, 3)
            testConfigChangeDuringRenders("OCIOLogConvert", in, configA, configB, 2., 3.);
        }
    }
    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIOColorSpace")) {
            setString(in.ctx, "ocioInputSpace", "x2");
            setString(in.ctx, "ocioOutputSpace", "x1");
            check(in.create(), "OCIOColorSpace: create instance");
            // (x2 to x1: 2 with a.ocio, 3 with b.ocio)
            testConfigChangeDuringRenders("OCIOColorSpace", in, configA, configB, 2., 3.);
        }
    }
    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIODisplay")) {
            setString(in.ctx, "display", "none");
            setString(in.ctx, "view", "Raw");
            setString(in.ctx, "ocioInputSpace", "x2");
            check(in.create(), "OCIODisplay: create instance");
            // (x2 to the view Raw, x1: 2 with a.ocio, 3 with b.ocio)
            testConfigChangeDuringRenders("OCIODisplay", in, configA, configB, 2., 3.);
        }
    }
    std::printf("==== OCIOProcessorCache: hits, misses, a failed build ====\n");
    testCacheDirectly();

    std::printf("==== 2. a changed context or config gives a new processor (#283, #284) ====\n");
    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIODisplay")) {
            setString(in.ctx, "display", "none");
            setString(in.ctx, "view", "Raw");
            setString(in.ctx, "ocioInputSpace", "ctx");
            check(in.create(), "OCIODisplay: create instance");
            float v = in.render1();
            check(v == 0.5f, "OCIODisplay, colorspace ctx, S = 2 (the config's default): 0.25 * 2 = 0.5 (got " + str(v) + ")");
            setString(in.ctx, "key1", "S");
            in.changed("key1");
            setString(in.ctx, "value1", "3");
            in.changed("value1");
            v = in.render1();
            check(v == 0.75f, "OCIODisplay, context param S = 3: 0.25 * 3 = 0.75 (got " + str(v) + ")");
            setString(in.ctx, "value1", "2");
            in.changed("value1");
            v = in.render1();
            check(v == 0.5f, "OCIODisplay, context param S = 2: 0.5 (got " + str(v) + ")");

            setString(in.ctx, "ocioInputSpace", "x2");
            in.changed("ocioInputSpace");
            v = in.render1();
            check(v == 0.5f, "OCIODisplay, colorspace x2 of a.ocio: 0.25 * 2 = 0.5 (got " + str(v) + ")");
            setString(in.ctx, "ocioConfigFile", configB);
            in.changed("ocioConfigFile");
            v = in.render1();
            check(v == 0.75f, "OCIODisplay, colorspace x2 of b.ocio (the OCIO config file param): 0.25 * 3 = 0.75 (got " + str(v) + ")");
        }
    }
    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIOLookTransform")) {
            setInt(in.ctx, "singleLook", 1);
            setInt(in.ctx, "lookChoice", 0); // "double"
            setString(in.ctx, "ocioInputSpace", "ctx");
            setString(in.ctx, "ocioOutputSpace", "x1");
            check(in.create(), "OCIOLookTransform: create instance");
            float v = in.render1();
            check(v == 1.f, "OCIOLookTransform, look double from ctx, S = 2: 0.25 * 2 * 2 = 1 (got " + str(v) + ")");
            setString(in.ctx, "key1", "S");
            in.changed("key1");
            setString(in.ctx, "value1", "3");
            in.changed("value1");
            v = in.render1();
            check(v == 1.5f, "OCIOLookTransform, context param S = 3: 0.25 * 3 * 2 = 1.5 (got " + str(v) + ")");
        }
    }

    std::printf("==== 3. Reload re-reads a modified LUT file (#361) ====\n");
    {
        Instance in;
        const std::string lut = dir + "/reload.cc";
        check(writeFile(lut, ccText(2)), "write reload.cc with a slope of 2");
        if (in.load("fr.inria.openfx.OCIOFileTransform")) {
            setString(in.ctx, "file", lut);
            check(in.create(), "OCIOFileTransform: create instance");
            float v = in.render1();
            check(v == 0.5f, "OCIOFileTransform, reload.cc with a slope of 2: 0.25 * 2 = 0.5 (got " + str(v) + ")");
            check(writeFile(lut, ccText(3)), "rewrite reload.cc with a slope of 3");
            in.changed("reload");
            v = in.render1();
            check(v == 0.75f, "OCIOFileTransform, after Reload: the new slope, 0.25 * 3 = 0.75 (got " + str(v) + ")");
        }
    }

    std::printf("==== 4. purging the caches re-reads a modified LUT file of a colorspace (#402) ====\n");
    for (const char* id : { "fr.inria.openfx.OCIOColorSpace", "fr.inria.openfx.OCIODisplay", "fr.inria.openfx.OCIOLookTransform" }) {
        const std::string name(id);
        const std::string lut = dir + "/purge.cc";
        check(writeFile(lut, ccText(2)), "write purge.cc with a slope of 2");
        OCIO_NAMESPACE::ClearAllCaches(); // (OCIO's file cache has the slope of 3 of the previous plug-in)
        Instance in;
        if (in.load(id)) {
            setString(in.ctx, "ocioInputSpace", "lut");
            double look = 1.; // (OCIOLookTransform: the look "double")
            if (name == "fr.inria.openfx.OCIODisplay") {
                setString(in.ctx, "display", "none");
                setString(in.ctx, "view", "Raw");
            } else {
                setString(in.ctx, "ocioOutputSpace", "x1");
            }
            if (name == "fr.inria.openfx.OCIOLookTransform") {
                setInt(in.ctx, "singleLook", 1);
                setInt(in.ctx, "lookChoice", 0); // "double"
                look = 2.;
            }
            check(in.create(), name + ": create instance");
            float v = in.render1();
            check(v == static_cast<float>(kInput * 2. * look), name + ", colorspace lut, purge.cc with a slope of 2: 0.25 * 2 * " + str(look) + " (got " + str(v) + ")");
            check(writeFile(lut, ccText(3)), "rewrite purge.cc with a slope of 3");
            check(ok(in.purgeCaches()), name + ": purge caches");
            v = in.render1();
            check(v == static_cast<float>(kInput * 3. * look), name + ", after purging the caches: the new slope, 0.25 * 3 * " + str(look) + " (got " + str(v) + ")");
            if (purgeDuringRenders) {
                testPurgeDuringRenders(name, in, 3. * look);
            } else {
                std::printf("  (%s: no purge during renders: --no-purge-during-renders)\n", name.c_str());
            }
        }
    }

    std::printf("==== 5. an OCIO 2 shared view with a view transform (#354) ====\n");
    {
        Instance in;
        if (in.load("fr.inria.openfx.OCIODisplay")) {
            setString(in.ctx, "display", "none");
            setString(in.ctx, "view", "Film");
            setString(in.ctx, "ocioInputSpace", "x1");
            check(in.create(), "OCIODisplay: create instance");
            const float v = in.render1();
            check(v == 1.f, "OCIODisplay, view Film (the view transform film): 0.25 * 4 = 1 (got " + str(v) + ")");
        }
    }

    std::printf("Result: failures=%d\n", gFailures);
    std::printf(gFailures == 0 ? "PASS\n" : "FAIL\n");

    return gFailures == 0 ? 0 : 1;
}
