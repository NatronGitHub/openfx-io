/*
 * oiio_threadpool_test — the OIIO plug-ins share OpenImageIO's thread pool, which is stopped by the unload of
 * the last OIIO plug-in loaded, not by the first one (#118), driven by the in-process host (ofx_test_host.h).
 *
 * In the combined IO.ofx, ReadOIIO, WriteOIIO, OIIOText and OIIOResize (two versions) all use OpenImageIO's
 * default thread pool, and stop it in their unload action (kOfxActionUnload), so that its threads are not
 * joined while the binary is unloaded (a deadlock on MS Windows). Before #118, the first OIIO plug-in to be
 * unloaded stopped the pool that the others still used.
 *
 *   - load and describe ReadOIIO, WriteOIIO and OIIOResize, create an OIIOResize instance: the pool has
 *     hardware_concurrency() - 1 threads
 *   - WriteOIIO's load creates an ImageOutput for each extension, and OpenImageIO's OpenEXR writer starts
 *     OpenEXR's pool then, outside of any instance (checked except on MS Windows); on MS Windows it is stopped at the end of the
 *     load (#256), and the instance (ExrThreadsUser) starts it again
 *   - unload ReadOIIO: the pool keeps its threads, and OIIOResize renders (ImageBufAlgo::resize(), with the
 *     host's 4 CPUs, uses the pool)
 *   - reload and unload ReadOIIO (as during a plug-in scan), load and unload OIIOResize v1 (another plug-in):
 *     the pool keeps its threads
 *   - two instances: destroying one keeps the pool's threads; destroying the last live instance stops the pool
 *     (0 threads, if OpenImageIO has default_thread_pool_shutdown()), while the plug-ins are still loaded, for a
 *     host that exits without kOfxActionUnload (#250); a new instance starts it again
 *   - unload WriteOIIO and OIIOResize, the last OIIO plug-in loaded: the pool stays stopped (the unload's
 *     shutdown is a fallback, and does nothing more)
 *   - load, render and unload OIIOResize again (the pool is restarted by the instance, stopped by its destruction)
 *   - 8 threads create and destroy OIIOThreadsUser objects (what each OIIO plug-in instance holds) concurrently, and
 *     check that the pool runs while theirs exists; at the end, nothing is alive and the pool is stopped (also built
 *     with ThreadSanitizer: make test-oiio-threads-tsan)
 *
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include "ExrThreads.h"
#include "OIIOGlobal.h"

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/oiioversion.h>
#include <OpenImageIO/sysutil.h>
#include <OpenImageIO/thread.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

void check(bool cond, const char* what)
{
    std::printf("  %s: %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        ++gFailures;
    }
}

// the host has 4 CPUs: OIIOResize passes MultiThread::getNumCPUs() to ImageBufAlgo, which then uses the pool
OfxStatus mtNum4(unsigned int* n) { *n = 4; return kOfxStatOK; }

int poolSize() { return OIIO::default_thread_pool()->size(); }

// The pool size while an OIIO plug-in instance exists: the first one (OIIOThreadsUser) sets the "threads" attribute to 0
// (all the cores), i.e. a pool of hardware_concurrency() - 1 threads (the calling thread is the other one).
int expectedPoolSize() { return (int)OIIO::Sysutil::hardware_concurrency() - 1; }

// OpenImageIO >= 2.5.3 (or a patched version) can stop its pool (OIIOGlobal.cpp)
#if defined(NATRON_OIIO_HAS_DEFAULT_THREAD_POOL_SHUTDOWN) || OIIO_VERSION >= OIIO_MAKE_VERSION(2, 5, 3)
const bool kHasPoolShutdown = true;
#else
const bool kHasPoolShutdown = false;
#endif

OfxPlugin* findPluginVersion(const char* id, int major)
{
    int n = OfxGetNumberOfPlugins();
    for (int i = 0; i < n; ++i) {
        OfxPlugin* p = OfxGetPlugin(i);
        if (p && p->pluginIdentifier && !std::strcmp(p->pluginIdentifier, id) && (int)p->pluginVersionMajor == major) {
            return p;
        }
    }
    return nullptr;
}

// OfxGetPlugin()'s plug-in, which the Support library deletes when it is unloaded: fetch it again to reload it,
// as a host does after it reloads the binary
OfxPlugin* readOIIO() { return findPlugin("fr.inria.openfx.ReadOIIO"); }
OfxPlugin* writeOIIO() { return findPlugin("fr.inria.openfx.WriteOIIO"); }
OfxPlugin* resizeOIIO(int major) { return findPluginVersion("fr.inria.openfx.OIIOResize", major); }

bool unload(OfxPlugin* p) { return ok(p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr)); }

// An OIIOResize instance (Filter context) that downscales a constant W x H RGBA float image to W/2 x H/2.
struct ResizeInstance {
    OfxPlugin* p;
    Effect ctx;
    Effect* c = &ctx; // the context descriptor (clips and params): ctx, or another instance's (shareContext())
    Effect inst;
    bool created = false;

    explicit ResizeInstance(OfxPlugin* plugin) : p(plugin) {}

    // another instance of the same context descriptor
    void shareContext(ResizeInstance& other) { c = &other.ctx; }
    ~ResizeInstance() { destroy(); }

    bool create()
    {
        inst.delegate = c;
        setInstanceProps(inst, 64, 64, 1, kOfxImageEffectContextFilter);
        created = ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr));
        return created;
    }

    void destroy()
    {
        if (created) {
            p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
            created = false;
        }
    }

    // render, and check that the output is the (constant) input color, away from the borders (the filter's
    // support, lanczos3 at scale 1/2, reaches 3 pixels outside of the image, where it is black)
    bool render()
    {
        const int W = 64, H = 64, w = W / 2, h = H / 2;
        const float color[4] = { 0.25f, 0.5f, 0.75f, 1.f };
        Clip* src = c->clips.count(kOfxImageEffectSimpleSourceClipName) ? c->clips[kOfxImageEffectSimpleSourceClipName] : nullptr;
        Clip* dst = c->clips.count(kOfxImageEffectOutputClipName) ? c->clips[kOfxImageEffectOutputClipName] : nullptr;
        if (!src || !dst) {
            std::printf("  no Source or Output clip\n");
            return false;
        }
        src->connected = true;
        setupClipImage(src, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        float* s = reinterpret_cast<float*>(src->buffer.data());
        for (int k = 0; k < W * H; ++k) {
            std::memcpy(s + 4 * k, color, sizeof(color));
        }
        dst->connected = true;
        setupClipImage(dst, w, h, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
        gPersistentError.clear();
        PropSet a = makeRenderInArgs(1., w, h);
        if (!ok(p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr))) {
            std::printf("  render failed\n");
            return false;
        }
        if (!gPersistentError.empty()) {
            std::printf("  render error: %s\n", gPersistentError.c_str());
            return false;
        }
        const float* d = reinterpret_cast<const float*>(dst->buffer.data());
        const int margin = 4;
        for (int y = margin; y < h - margin; ++y) {
            for (int x = margin; x < w - margin; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const float v = d[4 * (y * w + x) + c];
                    if (!(std::fabs(v - color[c]) <= 1e-4f)) {
                        std::printf("  pixel (%d,%d), channel %d: %g, expected %g\n", x, y, c, v, color[c]);
                        return false;
                    }
                }
            }
        }
        return true;
    }
};

} // namespace

int main()
{
    initHost();
    gMTSuite.multiThreadNumCPUs = mtNum4;

    OfxPlugin* read = readOIIO();
    OfxPlugin* write = writeOIIO();
    OfxPlugin* resize = resizeOIIO(2);
    if (!read || !write || !resize || !resizeOIIO(1)) {
        std::printf("FAIL: ReadOIIO, WriteOIIO or OIIOResize (v1, v2) not found\n");
        return 1;
    }
    const int expected = expectedPoolSize();
    std::printf("hardware_concurrency() - 1 = %d, default_thread_pool_shutdown(): %s\n", expected, kHasPoolShutdown ? "yes" : "no");
    if (expected <= 0) {
        // a 1-core machine has no pool threads: "the pool keeps its threads" would pass trivially
        std::printf("SKIP: hardware_concurrency() is 1, the pool has no threads to keep\n");
        return 0;
    }

    std::printf("== load ReadOIIO, WriteOIIO, OIIOResize; create an OIIOResize instance\n");
    Effect readCtx, writeCtx;
    check(loadAndDescribe(read, readCtx, readCtx, kOfxImageEffectContextReader) == 0, "ReadOIIO load/describe");
    check(loadAndDescribe(write, writeCtx, writeCtx, kOfxImageEffectContextWriter) == 0, "WriteOIIO load/describe");
    std::printf("  OpenEXR's pool: %d threads, %d live users\n", exrPoolThreads(), liveExrThreadsUsers());
    if (exrPoolThreads() >= 0) {
        // OpenEXROutput's constructor (ImageOutput::create("x.exr"), in WriteOIIO's load) starts OpenEXR's pool,
        // which a host that exits without kOfxActionUnload would leave to OpenEXR's static destructors (#256)
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
        check(exrPoolThreads() == 0, "no instance: OpenEXR's pool is stopped after WriteOIIO's load (#256)");
#else
        check(exrPoolThreads() > 0, "WriteOIIO's load starts OpenEXR's pool (OpenEXROutput, #256 precondition)");
#endif
    } else {
        // -1: built without OpenEXR's headers, so the #256 checks cannot run; that is only expected if OpenImageIO
        // has no EXR writer either (this creates an OpenEXROutput, but the pool state is not checked any more)
        check(!OIIO::ImageOutput::create("x.exr"), "built without OpenEXR's headers: OpenImageIO has no EXR writer");
    }
    {
        ResizeInstance r(resize);
        check(loadAndDescribe(resize, r.ctx, r.ctx, kOfxImageEffectContextFilter) == 0, "OIIOResize load/describe");
        check(r.create(), "OIIOResize CreateInstance");
        std::printf("  pool size: %d\n", poolSize());
        check(poolSize() == expected, "the pool has hardware_concurrency() - 1 threads");
        if (exrPoolThreads() >= 0) { // (-1: built without OpenEXR)
            check(exrPoolThreads() > 0, "an instance: OpenEXR's pool runs");
        }
        check(r.render(), "OIIOResize renders"); // (OIIO runs the work inline if the pool is stopped: only poolSize() detects #118)

        std::printf("== unload ReadOIIO (OIIOResize and WriteOIIO are still loaded)\n");
        check(unload(read), "ReadOIIO unload");
        std::printf("  pool size: %d\n", poolSize());
        check(poolSize() == expected, "the pool keeps its threads");
        check(r.render(), "OIIOResize renders");

        std::printf("== reload and unload ReadOIIO (plug-in scan)\n");
        {
            Effect ctx2;
            read = readOIIO();
            check(read && loadAndDescribe(read, ctx2, ctx2, kOfxImageEffectContextReader) == 0, "ReadOIIO load/describe");
            check(unload(read), "ReadOIIO unload");
        }
        check(poolSize() == expected, "the pool keeps its threads");
        check(r.render(), "OIIOResize renders");

        std::printf("== load and unload OIIOResize v1 (OIIOResize v2 is another plug-in)\n");
        {
            Effect ctx1;
            OfxPlugin* resize1 = resizeOIIO(1);
            check(resize1 && loadAndDescribe(resize1, ctx1, ctx1, kOfxImageEffectContextFilter) == 0, "OIIOResize v1 load/describe");
            check(unload(resize1), "OIIOResize v1 unload");
        }
        check(poolSize() == expected, "the pool keeps its threads");
        check(r.render(), "OIIOResize renders");

        std::printf("== a second instance; destroy the first one\n");
        {
            ResizeInstance r2(resize);
            r2.shareContext(r);
            check(r2.create(), "OIIOResize CreateInstance (second)");
            check(liveOIIOInstances() == 2, "2 live instances");
            r.destroy();
            check(liveOIIOInstances() == 1, "1 live instance");
            check(poolSize() == expected, "the pool keeps its threads");
            check(r2.render(), "the second instance renders");
            std::printf("== destroy the last live instance (the plug-ins are still loaded)\n");
            r2.destroy();
        }
        check(liveOIIOInstances() == 0, "no live instance");
        std::printf("  pool size: %d\n", poolSize());
        if (kHasPoolShutdown) {
            check(poolSize() == 0, "the pool is stopped (#250: a host may exit without kOfxActionUnload)");
        }
        std::printf("== a new instance\n");
        check(r.create(), "OIIOResize CreateInstance");
        check(poolSize() == expected, "the pool has hardware_concurrency() - 1 threads again");
        check(r.render(), "OIIOResize renders");
        r.destroy();
        if (kHasPoolShutdown) {
            check(poolSize() == 0, "the pool is stopped");
        }

        std::printf("== unload WriteOIIO, then OIIOResize, the last OIIO plug-in loaded\n");
        check(unload(write), "WriteOIIO unload");
        check(unload(resize), "OIIOResize unload");
        std::printf("  pool size: %d\n", poolSize());
        if (kHasPoolShutdown) {
            check(poolSize() == 0, "the pool stays stopped");
        }
    }

    std::printf("== load OIIOResize again, render, unload\n");
    resize = resizeOIIO(2);
    if (resize) {
        ResizeInstance r(resize);
        check(loadAndDescribe(resize, r.ctx, r.ctx, kOfxImageEffectContextFilter) == 0, "OIIOResize load/describe");
        check(r.create(), "OIIOResize CreateInstance");
        check(poolSize() == expected, "the pool has hardware_concurrency() - 1 threads again");
        check(r.render(), "OIIOResize renders");
        r.destroy();
        if (kHasPoolShutdown) {
            check(poolSize() == 0, "the pool is stopped by the destruction of the instance");
        }
        check(unload(resize), "OIIOResize unload");
        if (kHasPoolShutdown) {
            check(poolSize() == 0, "the pool stays stopped");
        }
    } else {
        check(false, "OIIOResize found");
    }

    std::printf("== 8 threads create and destroy OIIOThreadsUser objects concurrently\n");
    {
        std::atomic<int> notRunning(0);
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&notRunning, expected, t]() {
                for (int i = 0; i < 200; ++i) {
                    OIIOThreadsUser user;
                    if (poolSize() != expected) {
                        ++notRunning;
                    }
                    if ((i + t) % 3 == 0) {
                        OIIOThreadsUser nested; // a second instance on the same thread
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (std::thread& t : threads) {
            t.join();
        }
        std::printf("  the pool was not running %d times while an OIIOThreadsUser existed\n", notRunning.load());
        check(notRunning == 0, "the pool runs while an OIIOThreadsUser exists");
        check(liveOIIOInstances() == 0, "no live instance");
        if (kHasPoolShutdown) {
            check(poolSize() == 0, "the pool is stopped");
        }
    }

    if (gFailures) {
        std::printf("FAIL: %d check(s) failed\n", gFailures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
