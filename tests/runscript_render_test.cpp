/*
 * runscript_render_test — RunScript's render on POSIX (issue #287): a script that runs while the host aborts the
 * render must be killed, and the render must return (kOfxStatFailed) soon after the abort, instead of waiting for the
 * script to exit. Also: a script that floods its standard error, and one that ends normally, still render; a render
 * at a render scale other than 1 fails with an error message (issue #438), and so does a render whose script file can
 * only be written in part (a short write, provoked with RLIMIT_FSIZE: the truncated script was run).
 * ofxImageEffect.h, abort(): "This function indicates whether a plugin should stop whatever processing it is doing."
 *
 * Each render runs on a thread, with a time limit: a render that does not return in time is a failure, and the
 * program exits at once (before #287, the aborted render waited for "sleep 60").
 * The plug-in is linked directly and driven through the OFX actions by the in-process host of ofx_test_host.h.
 * POSIX only (not built on MS Windows, which has its own abort test in runscript_win32_test).
 *
 * Usage: runscript_render_test
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>

#include <cerrno>
#include <sys/resource.h> // setrlimit
#include <sys/types.h>
#include <unistd.h>

namespace {

const char* const kPluginIdentifier = "fr.inria.openfx.RunScript";
const int kW = 4;
const int kH = 2;
// the scripts that are aborted write their PID there ("$$"): the abort waits for it, and a hung script is killed
std::string gPidFile;

// the PID in gPidFile, or 0
long
readPid()
{
    long pid = 0;
    if (std::FILE* f = std::fopen(gPidFile.c_str(), "r")) {
        if (std::fscanf(f, "%ld", &pid) != 1) {
            pid = 0;
        }
        std::fclose(f);
    }

    return pid;
}

// Render script at time 1, setting the host's abort flag after abortAfterMs ms (if >= 0) once the script wrote its
// PID to gPidFile, and return the status of the render. If it does not return within limitMs ms, the program exits at
// once (the render thread still uses the effect).
OfxStatus
renderScript(OfxPlugin* p,
             const char* what,
             const std::string& script,
             int abortAfterMs,
             int limitMs)
{
    Effect ctx, inst;
    PropSet inArgs;
    pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
    if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, EH(ctx), PH(&inArgs), nullptr))) {
        check(false, "describeInContext");

        return kOfxStatFailed;
    }
    setOverrideString(&ctx, "script", script);
    setOverrideInt(&ctx, "paramCount", 0);
    setOverrideInt(&ctx, "validate", 1);
    Clip* src = ctx.clips[kOfxImageEffectSimpleSourceClipName];
    Clip* dst = ctx.clips[kOfxImageEffectOutputClipName];
    src->connected = true;
    dst->connected = true;
    setupClipImage(src, kW, kH, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
    setupClipImage(dst, kW, kH, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
    inst.delegate = &ctx;
    setInstanceProps(inst, kW, kH, 1, kOfxImageEffectContextFilter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, EH(inst), nullptr, nullptr))) {
        check(false, "createInstance");

        return kOfxStatFailed;
    }
    gAbort = 0;
    PropSet a = makeRenderInArgs(1., kW, kH);
    const auto start = std::chrono::steady_clock::now();
    std::packaged_task<OfxStatus()> task([&]() { return p->mainEntry(kOfxImageEffectActionRender, EH(inst), PH(&a), nullptr); });
    std::future<OfxStatus> done = task.get_future();
    std::thread renderThread(std::move(task));
    if (abortAfterMs >= 0 && done.wait_for(std::chrono::milliseconds(abortAfterMs)) == std::future_status::timeout) {
        // the script is running (for at most 5 s more if it never writes its PID: the checks then fail)
        for (int i = 0; i < 500 && readPid() <= 0 && done.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout; ++i) {
        }
        gAbort = 1;
    }
    const bool returned = done.wait_until(start + std::chrono::milliseconds(limitMs)) == std::future_status::ready;
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (!returned) {
        std::printf("  FAIL: %s: the render did not return within %lld ms (hung)\n\nFAILED\n", what, ms);
        std::fflush(stdout);
        // kill the script (and its group, if it has one), so that it does not keep running and hold ctest's output
        const long pid = readPid();
        if (pid > 0) {
            (void)::killpg((pid_t)pid, SIGKILL);
            (void)::kill((pid_t)pid, SIGKILL);
        }
        std::remove(gPidFile.c_str());
        std::_Exit(1);
    }
    renderThread.join();
    const OfxStatus st = done.get();
    std::printf("  (%s: render returned %d after %lld ms)\n", what, st, ms);
    gAbort = 0;
    p->mainEntry(kOfxActionDestroyInstance, EH(inst), nullptr, nullptr);

    return st;
}

} // namespace

int
main()
{
    initHost();
    OfxPlugin* p = findPlugin(kPluginIdentifier);
    if (!p) {
        std::printf("FAIL: plugin %s not found\n", kPluginIdentifier);

        return 1;
    }
    Effect describeEffect, ctx;
    if (loadAndDescribe(p, describeEffect, ctx, kOfxImageEffectContextFilter) != 0) {
        std::printf("FAIL: load/describe\n");

        return 1;
    }

    std::printf("a script that ends:\n");
    gPersistentWarning.clear();
    OfxStatus st = renderScript(p, "exit 0", "#!/bin/sh\necho hello >&2\nexit 0\n", -1, 30000);
    check(st == kOfxStatOK && gPersistentWarning.empty(), "renders, without a warning");

    // #438: RunScript renders at scale 1 only; at another scale the render used to fail with no message
    std::printf("a render at render scale 0.5:\n");
    gPersistentError.clear();
    gRS = 0.5; // makeRenderInArgs()'s render scale
    st = renderScript(p, "scale 0.5", "#!/bin/sh\nexit 0\n", -1, 30000);
    gRS = 1.;
    check(st == kOfxStatFailed && gPersistentError.find("render scale") != std::string::npos,
          "fails, with an error message: \"" + gPersistentError + "\"");

    // A short write of the script file (here, beyond RLIMIT_FSIZE: write() writes up to the limit; SIGXFSZ is
    // ignored, in case another write is made) fails the render: it used to run the first 64 bytes of the script,
    // which exit 0.
    // (While the limit is set, the output of this program to a file larger than 64 bytes is lost.)
    std::printf("a script that can only be written in part (RLIMIT_FSIZE 64 bytes):\n");
    std::fflush(stdout);
    {
        struct rlimit oldLimit;
        const bool limited = ::getrlimit(RLIMIT_FSIZE, &oldLimit) == 0;
        struct rlimit limit = oldLimit;
        limit.rlim_cur = 64;
        void (*const oldHandler)(int) = std::signal(SIGXFSZ, SIG_IGN);
        const bool set = limited && ::setrlimit(RLIMIT_FSIZE, &limit) == 0;
        gPersistentError.clear();
        const std::string script = std::string("#!/bin/sh\nexit 0\n") + std::string(64, '#') + "\nexit 7\n";
        st = renderScript(p, "short write", script, -1, 30000);
        if (set) {
            (void)::setrlimit(RLIMIT_FSIZE, &oldLimit);
        }
        std::signal(SIGXFSZ, oldHandler);
        check(set, "RLIMIT_FSIZE set to 64 bytes");
        check(st == kOfxStatFailed && gPersistentError.find("cannot write the script") != std::string::npos,
              "fails, with an error message: \"" + gPersistentError + "\"");
    }

    std::printf("a script that floods its standard error (20 MB) and fails:\n");
    gPersistentWarning.clear();
    st = renderScript(p, "flood", "#!/bin/sh\nhead -c 20000000 /dev/zero | tr '\\0' 'x' >&2\necho >&2\necho last >&2\nexit 3\n", -1, 60000);
    check(st == kOfxStatOK && gPersistentWarning.find("exited with code 3") != std::string::npos
              && gPersistentWarning.find("last") != std::string::npos,
          "renders, with the exit code and the last line in the warning: \"" + gPersistentWarning.substr(0, 80) + "\"");

    char pidFile[] = "/tmp/runscript_render_test_pidXXXXXX";
    const int fd = mkstemp(pidFile);
    if (fd < 0) {
        std::printf("FAIL: mkstemp\n");

        return 1;
    }
    close(fd);
    gPidFile = pidFile;
    // (exec: the process that sleeps or floods is the script, whose PID is in gPidFile)
    struct AbortCase {
        const char* title;
        const char* what;
        const char* command;
    };
    const AbortCase aborts[] = {
        { "a script that sleeps while the render is aborted", "abort", "echo started >&2\nexec sleep 60\n" },
        { "a script that floods its standard error while the render is aborted", "abort while flooding", "exec yes >&2\n" },
    };
    for (const AbortCase& c : aborts) {
        std::printf("%s:\n", c.title);
        if (std::FILE* f = std::fopen(gPidFile.c_str(), "w")) { // empty
            std::fclose(f);
        }
        st = renderScript(p, c.what, "#!/bin/sh\necho $$ > " + gPidFile + "\n" + c.command, 300, 10000);
        check(st == kOfxStatFailed, "the aborted render fails (kOfxStatFailed)");
        const long pid = readPid();
        // (the script is a child of this process, reaped by the render)
        const bool gone = pid > 0 && ::kill((pid_t)pid, 0) != 0 && errno == ESRCH;
        check(gone, "the script (PID " + std::to_string(pid) + ") is gone");
    }
    std::remove(gPidFile.c_str());

    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::printf("\n%s (%d failure%s)\n", gFailures ? "FAILED" : "ALL PASSED", gFailures, gFailures == 1 ? "" : "s");

    return gFailures ? 1 : 0;
}
