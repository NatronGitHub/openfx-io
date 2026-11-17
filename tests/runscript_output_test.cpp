/*
 * runscript_output_test — tests how RunScript splits the standard error of the script into lines
 * (RunScript/RunScriptOutput.h, used on all systems), with bounded memory:
 * - "\n" and "\r\n" end a line, a lone "\r" (progress output) starts it again, so the last segment is kept;
 * - long lines are cut (at a UTF-8 sequence boundary), and only the last lines are kept;
 * - on POSIX, the same through pstream, as RunScript runs a script there: "\r"-only output, a script that floods
 *   stderr with one huge line, and the detection of an exec() failure (a missing interpreter).
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

#include "pstream.h"
#define TEST_PSTREAM
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

#ifdef TEST_PSTREAM
// write a script to a temporary file, and run it as RunScript does on POSIX
static bool
runScript(const string& script,
          vector<string>* lines,
          int* execError)
{
    char name[] = "/tmp/runscript_output_testXXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) {
        return false;
    }
    bool ok = write(fd, script.data(), script.size()) == (ssize_t)script.size();
    close(fd);
    ok = ok && chmod(name, S_IRWXU) == 0;
    *execError = 0;
    if (ok) {
        vector<string> argv(1, name);
        redi::ipstream in(name, argv, redi::pstreambuf::pstderr);
        if (!in.rdbuf()->is_open()) {
            *execError = in.rdbuf()->error();
        } else {
            LineCollector c(lines);
            RunScriptOutput::readAll(in.rdbuf(), &c);
            in.close();
        }
    }
    unlink(name);

    return ok;
}

static void
testPstream()
{
    std::printf("pstream (as RunScript on POSIX):\n");
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

    // exec() failure, which RunScript reports as "cannot execute ...: No such file or directory"
    l.clear();
    CHECK(runScript("#!/nonexistent/interpreter\n", &l, &err) && err == ENOENT,
          ("missing interpreter: is_open() is false, error() is ENOENT (" + string(err ? std::strerror(err) : "no error") + ")").c_str());
}
#endif

int
main()
{
    testSplit();
    testBounds();
#ifdef TEST_PSTREAM
    testPstream();
#endif
    std::printf("Result: failures=%d\n", gFailures);

    return gFailures ? 1 : 0;
}
