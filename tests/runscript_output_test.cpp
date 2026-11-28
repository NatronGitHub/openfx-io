/*
 * runscript_output_test — tests how RunScript splits the standard error of the script into lines
 * (RunScript/RunScriptOutput.h, used on all systems), with bounded memory:
 * - "\n" and "\r\n" end a line, a lone "\r" (progress output) starts it again, so the last segment is kept;
 * - long lines are cut (at a UTF-8 sequence boundary), and only the last lines are kept;
 * - on POSIX, the same through RunScriptPosix::Process, as RunScript runs a script there: "\r"-only output, a script
 *   that floods stderr with one huge line, and the detection of an exec() failure (a missing interpreter);
 * - on POSIX, aborting (#287): a script that sleeps, one that closed its stderr and sleeps, and one whose background
 *   child keeps stderr open, are killed with their process group within a second of the abort.
 * The MS Windows process launching is tested by runscript_win32_test.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "RunScriptOutput.h"

#if !(defined(_WIN32) || defined(__WIN32__) || defined(WIN32))
#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <set>

#include "RunScriptPosix.h"
#define TEST_POSIX
#endif

using RunScriptOutput::LineCollector;
using std::string;
using std::vector;

static int gFailures = 0;

#define CHECK(cond, what)                      \
    do {                                       \
        if (cond) {                            \
            std::printf("  ok: %s\n", what);   \
        } else {                               \
            std::printf("  FAIL: %s\n", what); \
            ++gFailures;                       \
        }                                      \
    } while (0)

static string
show(const vector<string>& v)
{
    string s;
    for (size_t i = 0; i < v.size(); ++i) {
        string l = v[i].size() > 40 ? v[i].substr(0, 40) + "...(" + std::to_string(v[i].size()) + " bytes)" : v[i];
        s += (i ? ", [" : "[") + l + "]";
    }

    return s;
}

// split data, given in chunks of chunkSize bytes
static vector<string>
split(const string& data,
      size_t chunkSize = 1,
      size_t maxLines = 1000,
      size_t maxBytes = 1024 * 1024,
      size_t maxLineBytes = 64 * 1024)
{
    vector<string> lines;
    LineCollector c(&lines, maxLines, maxBytes, maxLineBytes);
    for (size_t i = 0; i < data.size(); i += chunkSize) {
        c.add(data.data() + i, data.size() - i < chunkSize ? data.size() - i : chunkSize);
    }
    c.finish();

    return lines;
}

static vector<string>
V(const char* a = NULL,
  const char* b = NULL,
  const char* c = NULL)
{
    vector<string> v;
    if (a) {
        v.push_back(a);
    }
    if (b) {
        v.push_back(b);
    }
    if (c) {
        v.push_back(c);
    }

    return v;
}

static void
testSplit()
{
    std::printf("line splitting:\n");
    for (size_t chunk = 1; chunk <= 7; chunk += 6) {
        const string by = " (chunks of " + std::to_string(chunk) + ")";
        CHECK(split("a\nb\n", chunk) == V("a", "b"), ("\\n" + by).c_str());
        CHECK(split("a\r\nb\r\n", chunk) == V("a", "b"), ("\\r\\n" + by).c_str());
        CHECK(split("a\nb", chunk) == V("a", "b"), ("last line without a line break" + by).c_str());
        CHECK(split("", chunk).empty() && split("\r", chunk).empty(), ("nothing" + by).c_str());
        CHECK(split("\n\n", chunk) == V("", ""), ("empty lines" + by).c_str());
        CHECK(split("frame=1\rframe=2\rframe=3\n", chunk) == V("frame=3"), ("lone \\r: the last segment" + by).c_str());
        CHECK(split("start\nframe=1\rframe=2\rframe=3", chunk) == V("start", "frame=3"), ("lone \\r at the end of the output" + by).c_str());
        CHECK(split("50%\r100%\r", chunk) == V("100%"), ("trailing \\r: the last segment is kept" + by).c_str());
        CHECK(split("abc\r\r\n", chunk) == V("abc"), ("\\r\\r\\n" + by).c_str());
        CHECK(split("abc\r\r\ndef\r\rx\n", chunk) == V("abc", "x"), ("\\r\\r then text" + by).c_str());
    }

    // 1 MB of "\r"-only progress output: one short line
    string progress;
    for (int i = 0; i < 100000; ++i) {
        progress += "frame=" + std::to_string(i) + "\r";
    }
    progress += "frame=last";
    vector<string> l = split(progress, 4096);
    CHECK(l == V("frame=last"), ("\\r-only progress output: " + show(l)).c_str());
}

static void
testBounds()
{
    std::printf("bounds:\n");
    // a long line is cut
    string longLine(200000, 'x');
    vector<string> l = split(longLine + "\nafter\n", 4096);
    CHECK(l.size() == 2 && l[0] == string(64 * 1024, 'x') + "..." && l[1] == "after", ("long line cut at 64 KB: " + show(l)).c_str());
    // a long line, then a lone \r: the line starts again, uncut
    l = split(longLine + "\rshort", 4096);
    CHECK(l == V("short"), ("long line, then \\r: " + show(l)).c_str());
    // not within a UTF-8 sequence: "é" is 2 bytes, cut after 5 bytes -> "éé" + "..."
    l = split("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\n", 1, 1000, 1024, 5);
    CHECK(l.size() == 1 && l[0] == "\xC3\xA9\xC3\xA9...", ("cut at a UTF-8 boundary: " + show(l)).c_str());
    // 4-byte sequences
    l = split("\xF0\x9F\x8E\xAC\xF0\x9F\x8E\xAC\n", 1, 1000, 1024, 6);
    CHECK(l.size() == 1 && l[0] == "\xF0\x9F\x8E\xAC...", ("cut at a UTF-8 boundary (4 bytes): " + show(l)).c_str());
    CHECK(RunScriptOutput::truncateUtf8("abc", 3) == "abc", "truncateUtf8: short string unchanged");
    CHECK(RunScriptOutput::truncateUtf8("abcd", 3) == "abc...", "truncateUtf8: cut");
    CHECK(RunScriptOutput::truncateUtf8("a\xE6\x97\xA5", 3) == "a...", "truncateUtf8: not within a UTF-8 sequence");

    // only the last lines are kept
    string many;
    for (int i = 0; i < 10000; ++i) {
        many += std::to_string(i) + "\n";
    }
    l = split(many, 4096, 100);
    CHECK(l.size() >= 100 && l.size() <= 200 && l.back() == "9999", ("last lines kept (" + std::to_string(l.size()) + ")").c_str());
    // and at most about maxBytes bytes of them
    string big;
    for (int i = 0; i < 1000; ++i) {
        big += string(1000, 'a' + i % 26) + "\n";
    }
    l = split(big, 4096, 1000, 10000);
    size_t bytes = 0;
    for (size_t i = 0; i < l.size(); ++i) {
        bytes += l[i].size();
    }
    CHECK(bytes <= 20000 && l.size() >= 10 && l.back() == string(1000, 'a' + 999 % 26),
          ("bytes bounded (" + std::to_string(l.size()) + " lines, " + std::to_string(bytes) + " bytes)").c_str());
}

#ifdef TEST_POSIX
// aborted() of runScript(): true once deadline is passed and, if readyFile is set, that file is not empty (the
// script wrote it), or 10 s after the deadline
struct AbortAt {
    std::chrono::steady_clock::time_point deadline;
    const char* readyFile;
};

static bool
abortAt(void* context)
{
    const AbortAt* at = static_cast<const AbortAt*>(context);
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (now < at->deadline) {
        return false;
    }
    struct stat st;
    return !at->readyFile || (stat(at->readyFile, &st) == 0 && st.st_size > 0) || now >= at->deadline + std::chrono::seconds(10);
}

// write a script to a temporary file, and run it as RunScript does on POSIX; abortAfterMs >= 0: abort after that
// time, once readyFile (if any) is not empty (*result is eRunAborted if the script was killed)
static bool
runScript(const string& script,
          vector<string>* lines,
          int* execError,
          int abortAfterMs = -1,
          RunScriptPosix::RunResult* result = NULL,
          const char* readyFile = NULL)
{
    char name[] = "/tmp/runscript_output_testXXXXXX";
    int fd = mkostemp(name, O_CLOEXEC); // (not inherited by the scripts of other threads: ETXTBSY)
    if (fd < 0) {
        return false;
    }
    bool ok = write(fd, script.data(), script.size()) == (ssize_t)script.size();
    close(fd);
    ok = ok && chmod(name, S_IRWXU) == 0;
    *execError = 0;
    if (ok) {
        vector<string> argv(1, name);
        RunScriptPosix::Process process(name, argv);
        if (!process.started()) {
            *execError = process.error();
        } else {
            LineCollector c(lines);
            AbortAt at;
            at.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(abortAfterMs);
            at.readyFile = readyFile;
            const RunScriptPosix::RunResult r = process.run(c, abortAfterMs >= 0 ? &abortAt : NULL, &at);
            if (result) {
                *result = r;
            }
        }
    }
    unlink(name);

    return ok;
}

static void
testPosix()
{
    std::printf("RunScriptPosix::Process (as RunScript on POSIX):\n");
    vector<string> l;
    int err = 0;
    CHECK(runScript("#!/bin/sh\necho one >&2\nprintf 'two\\r\\n' >&2\nprintf 'no newline' >&2\n", &l, &err) && err == 0
              && l == V("one", "two", "no newline"),
          ("lines: " + show(l)).c_str());

    l.clear();
    CHECK(runScript("#!/bin/sh\ni=0\nwhile [ $i -lt 2000 ]; do printf 'frame=%d\\r' $i >&2; i=$((i+1)); done\nprintf 'done\\r' >&2\nexit 1\n",
                    &l, &err)
              && l == V("done"),
          ("\\r-only progress output: " + show(l)).c_str());

    // 20 MB on one line: bounded
    l.clear();
    CHECK(runScript("#!/bin/sh\nhead -c 20000000 /dev/zero | tr '\\0' 'x' >&2\necho >&2\necho last >&2\n", &l, &err)
              && l.size() == 2 && l[0].size() == 64 * 1024 + 3 && l[1] == "last",
          ("a 20 MB line is cut: " + show(l)).c_str());

    // stdin is /dev/null: read gets the end of file at once (and does not wait for, or read, the host's stdin). The
    // host's stdin is then a pipe with a line in it, so that a script that inherited it would read it.
    // (If this test was started with fd 0 closed, dup() fails: fd 0 is closed again afterwards.)
    l.clear();
    int hostIn[2];
    const int savedStdin = dup(STDIN_FILENO);
    if (pipe(hostIn) == 0) {
        const char line[] = "host\n";
        const bool filled = write(hostIn[1], line, sizeof(line) - 1) == (ssize_t)(sizeof(line) - 1);
        close(hostIn[1]);
        if (hostIn[0] != STDIN_FILENO) { // (it is fd 0 if fd 0 was closed)
            dup2(hostIn[0], STDIN_FILENO);
            close(hostIn[0]);
        }
        CHECK(filled && runScript("#!/bin/sh\nif read x; then echo \"read: $x\" >&2; else echo eof >&2; fi\n", &l, &err) && l == V("eof"),
              ("stdin is /dev/null, not the host's: " + show(l)).c_str());
        if (savedStdin >= 0) {
            dup2(savedStdin, STDIN_FILENO);
        } else {
            close(STDIN_FILENO); // the pipe
        }
    } else {
        CHECK(false, "pipe for the host's stdin");
    }
    if (savedStdin >= 0) {
        close(savedStdin);
    }

    // The host closed some of its standard streams: the stderr pipe must not be fd 2 already (0 and 2 closed: pipe()
    // gives fds 0 and 2), and a closed stdout is not inherited (posix_spawn() would fail with EBADF on macOS).
    const int closedSets[2][2] = { { STDIN_FILENO, STDERR_FILENO }, { STDOUT_FILENO, -1 } };
    for (const auto& closed : closedSets) {
        int saved[2] = { -1, -1 };
        for (int i = 0; i < 2 && closed[i] >= 0; ++i) {
            saved[i] = fcntl(closed[i], F_DUPFD_CLOEXEC, 10);
            close(closed[i]);
        }
        l.clear();
        const bool ran = runScript("#!/bin/sh\necho captured >&2\n", &l, &err);
        for (int i = 0; i < 2 && closed[i] >= 0; ++i) {
            dup2(saved[i], closed[i]);
            close(saved[i]);
        }
        CHECK(ran && err == 0 && l == V("captured"),
              ("host's fd " + std::to_string(closed[0]) + (closed[1] >= 0 ? " and " + std::to_string(closed[1]) : string()) + " closed: " + show(l) + " (error " + std::to_string(err) + ")").c_str());
    }

    // no "#!" line: run by /bin/sh, as execvp() does (ENOEXEC from exec())
    l.clear();
    CHECK(runScript("echo \"no shebang: $#\" >&2\n", &l, &err) && err == 0 && l == V("no shebang: 0"),
          ("a script without \"#!\" is run by /bin/sh: " + show(l)).c_str());

    // exec() failure, which RunScript reports as "cannot execute ...: No such file or directory"
    l.clear();
    CHECK(runScript("#!/nonexistent/interpreter\n", &l, &err) && err == ENOENT,
          ("missing interpreter: started() is false, error() is ENOENT (" + (err ? RunScriptPosix::errorString(err) : string("no error")) + ")").c_str());

    // the message of an errno value that RunScript shows (#474: the thread-safe strerror_r(), GNU or XSI), the same as
    // the one of strerror(), on this thread only; the message of an unknown value has the value (the libc's text)
    CHECK(RunScriptPosix::errorString(ENOENT) == std::strerror(ENOENT),
          ("errorString(ENOENT) is strerror(ENOENT): \"" + RunScriptPosix::errorString(ENOENT) + "\"").c_str());
    CHECK(RunScriptPosix::errorString(-12345).find("12345") != string::npos, ("errorString(-12345): \"" + RunScriptPosix::errorString(-12345) + "\"").c_str());
}

// the file descriptors of this process that are pipes
static std::set<int>
pipeFds()
{
    std::set<int> fds;
    for (int fd = 0; fd < 256; ++fd) {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode)) {
            fds.insert(fd);
        }
    }

    return fds;
}

// The script of a concurrent render does not inherit the stderr pipe of a running script (it would hold it open, and
// that script's render would wait for it): while a script runs, another script does not have its pipe open. (This
// only catches a pipe that is not close-on-exec on Linux: on macOS, POSIX_SPAWN_CLOEXEC_DEFAULT closes it anyway.)
static void
testPipeNotInherited()
{
    std::printf("the stderr pipe of a script is not inherited by another one:\n");
    char name[] = "/tmp/runscript_output_testXXXXXX";
    const int fd = mkostemp(name, O_CLOEXEC);
    if (fd < 0) {
        CHECK(false, "mkostemp");

        return;
    }
    const string sleeps = "#!/bin/sh\nsleep 2\n";
    const bool written = write(fd, sleeps.data(), sleeps.size()) == (ssize_t)sleeps.size();
    close(fd);
    if (written && chmod(name, S_IRWXU) == 0) {
        const std::set<int> before = pipeFds();
        RunScriptPosix::Process running(name, vector<string>(1, name)); // (killed by its destructor)
        std::set<int> pipes = pipeFds();
        for (int b : before) {
            pipes.erase(b);
        }
        CHECK(running.started() && pipes.size() == 1, ("the running script's pipe: " + std::to_string(pipes.size()) + " new pipe(s)").c_str());
        if (pipes.size() == 1) {
            const string n = std::to_string(*pipes.begin());
            vector<string> l;
            int err = 0;
            CHECK(runScript("#!/bin/sh\nif [ -e /dev/fd/" + n + " ]; then echo inherited >&2; else echo closed >&2; fi\n", &l, &err)
                      && l == V("closed"),
                  ("file descriptor " + n + " in the other script: " + show(l)).c_str());
        }
    } else {
        CHECK(false, "write the script");
    }
    unlink(name);
}

// #287: an aborted script is killed, with the processes of its group, and run() returns soon after the abort
static void
testAbort()
{
    std::printf("abort (as RunScript on POSIX):\n");
    struct Case {
        const char* what;
        const char* script;
    };
    // pidfile: the PID of a process that must be killed (the script or a background child); the abort waits for it
    char pidfile[] = "/tmp/runscript_output_test_pidXXXXXX";
    const int fd = mkstemp(pidfile);
    if (fd < 0) {
        CHECK(false, "mkstemp");

        return;
    }
    close(fd);
    const string p = pidfile;
    const string sleeps = "#!/bin/sh\necho $$ > " + p + "\nsleep 60\n";
    const string floods = "#!/bin/sh\nyes >&2 &\necho $! > " + p + "\nwait\n";
    const string closed = "#!/bin/sh\nexec 2>&-\nsleep 60 &\necho $! > " + p + "\nwait\n";
    const string bg = "#!/bin/sh\nsleep 60 &\necho $! > " + p + "\nexit 0\n";
    const Case cases[] = {
        { "a script that sleeps", sleeps.c_str() },
        { "a script whose child floods stderr", floods.c_str() },
        { "a script that closed its stderr and waits for its child", closed.c_str() },
        { "a script that exited, with a child that keeps stderr open", bg.c_str() },
    };
    for (const Case& c : cases) {
        if (std::FILE* f = std::fopen(pidfile, "w")) { // empty: this case has no background child yet
            std::fclose(f);
        }
        vector<string> l;
        int err = 0;
        RunScriptPosix::RunResult r = RunScriptPosix::eRunOK;
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        const bool ran = runScript(c.script, &l, &err, 300, &r, pidfile);
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        CHECK(ran && err == 0 && r == RunScriptPosix::eRunAborted && ms < 10000,
              (string(c.what) + ": aborted after " + std::to_string(ms) + " ms").c_str());
        // the process that it wrote to pidfile was killed (the background child, with the process group)
        long pid = 0;
        if (std::FILE* f = std::fopen(pidfile, "r")) {
            if (std::fscanf(f, "%ld", &pid) != 1) {
                pid = 0;
            }
            std::fclose(f);
        }
        CHECK(pid > 0, (string(c.what) + ": it wrote a PID before the abort").c_str());
        if (pid > 0) {
            // (a background child is reaped by PID 1 once killed, since the script, its parent, was killed too: this
            // assumes that PID 1 reaps orphans, which it does not in a container run without --init)
            bool gone = false;
            for (int i = 0; i < 100 && !gone; ++i) {
                gone = ::kill((pid_t)pid, 0) != 0 && errno == ESRCH;
                if (!gone) {
                    usleep(10 * 1000);
                }
            }
            if (!gone) {
                (void)::kill((pid_t)pid, SIGKILL);
            }
            CHECK(gone, (string(c.what) + ": the process was killed").c_str());
        }
    }
    std::remove(pidfile);

    // not aborted: the script runs to its end
    vector<string> l;
    int err = 0;
    RunScriptPosix::RunResult r = RunScriptPosix::eRunAborted;
    CHECK(runScript("#!/bin/sh\nsleep 1\necho done >&2\n", &l, &err, 10000, &r) && r == RunScriptPosix::eRunOK && l == V("done"),
          ("a script that ends before the abort deadline: " + show(l)).c_str());
}
#endif

int
main()
{
    testSplit();
    testBounds();
#ifdef TEST_POSIX
    testPosix();
    testPipeNotInherited();
    testAbort();
#endif
    std::printf("Result: failures=%d\n", gFailures);

    return gFailures ? 1 : 0;
}
