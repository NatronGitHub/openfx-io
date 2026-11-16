/*
 * runscript_win32_test — tests the MS Windows process launching of RunScript (RunScript/RunScriptWin32.cpp):
 * argument quoting (checked with CommandLineToArgvW), running a program directly, a batch file through
 * cmd.exe, and a script with a shebang line; stderr capture and exit code.
 *
 * The test runs itself as the child program: when RUNSCRIPT_TEST_OUT is set, it writes its arguments
 * (parsed from GetCommandLineW() with the MSVC runtime rules, as UTF-8, each followed by a NUL) to that file,
 * prints two lines to stderr, and exits with code 3. It also checks command-injection canaries in batch files,
 * the interpreter search (PATH only, not the current directory), "x.bat." (CVE-2024-43402), a process started
 * with "start" that keeps stderr open, and abort.
 *
 * On other systems, it does nothing (RunScript uses pstream there).
 */

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// shellapi.h (CommandLineToArgvW) needs windows.h first
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "RunScriptWin32.h"

using namespace RunScriptWin32;
using std::string;
using std::vector;
using std::wstring;

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

static vector<wstring>
parseCommandLine(const wstring& cmd)
{
    vector<wstring> out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmd.c_str(), &argc);
    if (argv) {
        for (int i = 0; i < argc; ++i) {
            out.push_back(argv[i]);
        }
        LocalFree(argv);
    }

    return out;
}

// The command-line parsing of the MSVC runtime (UCRT, and msvcrt since 2008), which, unlike CommandLineToArgvW,
// documents that "" within a quoted argument is a literal ". Batch files pass a '"' in an argument as "".
static vector<wstring>
parseCommandLineCRT(const wstring& cmd)
{
    vector<wstring> out;
    const wchar_t* p = cmd.c_str();
    // program name: no escapes
    wstring prog;
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') {
            prog.push_back(*p++);
        }
        if (*p) {
            ++p;
        }
    } else {
        while (*p && *p != L' ' && *p != L'\t') {
            prog.push_back(*p++);
        }
    }
    out.push_back(prog);
    for (;;) {
        while (*p == L' ' || *p == L'\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        wstring arg;
        bool inquote = false;
        for (;;) {
            bool copy = true;
            size_t slashes = 0;
            while (*p == L'\\') {
                ++p;
                ++slashes;
            }
            if (*p == L'"') {
                if (slashes % 2 == 0) {
                    if (inquote && p[1] == L'"') {
                        ++p; // "" within quotes: a literal "
                    } else {
                        copy = false;
                        inquote = !inquote;
                    }
                }
                slashes /= 2;
            }
            arg.append(slashes, L'\\');
            if (!*p || (!inquote && (*p == L' ' || *p == L'\t'))) {
                break;
            }
            if (copy) {
                arg.push_back(*p);
            }
            ++p;
        }
        out.push_back(arg);
    }

    return out;
}

static string
show(const vector<string>& v)
{
    string s;
    for (size_t i = 0; i < v.size(); ++i) {
        s += (i ? ", [" : "[") + v[i] + "]";
    }

    return s;
}

static int
childMain(const wstring& outFile)
{
    vector<wstring> args = parseCommandLineCRT(GetCommandLineW());
    FILE* f = _wfopen(outFile.c_str(), L"wb");
    if (!f) {
        return 2;
    }
    for (size_t i = 1; i < args.size(); ++i) {
        string a = wideToUtf8(args[i]);
        std::fwrite(a.data(), 1, a.size(), f);
        std::fputc('\0', f);
    }
    std::fclose(f);
    std::fprintf(stderr, "child stderr line 1\nchild stderr line 2\n");
    std::fflush(stderr);

    return 3;
}

static vector<string>
readChildArgs(const wstring& outFile)
{
    vector<string> out;
    string data;
    FILE* f = _wfopen(outFile.c_str(), L"rb");
    if (f) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            data.append(buf, n);
        }
        std::fclose(f);
    }
    size_t start = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\0') {
            out.push_back(data.substr(start, i - start));
            start = i + 1;
        }
    }
    DeleteFileW(outFile.c_str());

    return out;
}

// command injection canaries (CVE-2024-24576): if one is interpreted by cmd.exe, the marker file is created or
// the exit code is 9
static const char* kCanaryMarker = "x\" & echo PWNED> \"%TEMP%\\rs_marker\" & \"";
static const char* kCanaryExit = "\" & exit /b 9 & \"";
static const char* kCanaryBackslash = "a\\\"&echo PWNED>\"%TEMP%\\rs_marker\"&\\";

static vector<string>
commonArgs(bool batch)
{
    vector<string> a;
    a.push_back("plain");
    a.push_back("with space");
    a.push_back("");
    a.push_back("a&b|c<d>e^f");
    a.push_back("100%");
    a.push_back("%PATH%");
    a.push_back("%%cd%%");
    a.push_back("!x!");
    a.push_back("(paren) [b] {c}");
    a.push_back("semi;colon,comma=eq");
    a.push_back("C:\\dir\\");
    a.push_back("back\\\\slashes\\\\");
    a.push_back("tab\tchar");
    a.push_back("*.exr");
    a.push_back("a[1].png");
    a.push_back("@args.txt");
    a.push_back("\xC3\xA9\xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xAC"); // é日本 🎬
    a.push_back("quote\"inside");
    a.push_back("\\\"backslash quote\\\"");
    a.push_back(kCanaryMarker);
    a.push_back(kCanaryExit);
    a.push_back(kCanaryBackslash);
    if (!batch) {
        a.push_back("line\nbreak"); // cannot be passed to a batch file
    }

    return a;
}

static wstring gMarker;

static bool
markerExists()
{
    return GetFileAttributesW(gMarker.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static int gAbortPolls = 0;

static bool
abortAfterAFewPolls(void*)
{
    return ++gAbortPolls > 10;
}

int
main()
{
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"RUNSCRIPT_TEST_OUT", buf, 32768);
    if (n > 0 && n < 32768) {
        return childMain(wstring(buf, n));
    }

    n = GetModuleFileNameW(NULL, buf, 32768);
    const wstring self(buf, n);
    n = GetTempPathW(32768, buf);
    const wstring tmp(buf, n);
    const wstring outFile = tmp + L"runscript_win32_test_out.bin";
    n = GetEnvironmentVariableW(L"TEMP", buf, 32768);
    gMarker = wstring(buf, n) + L"\\rs_marker";
    DeleteFileW(gMarker.c_str());
    SetEnvironmentVariableW(L"RUNSCRIPT_TEST_EXE", self.c_str());

    // 1. quoting, without running anything
    std::printf("quoteArg/CommandLineToArgvW:\n");
    {
        vector<string> args = commonArgs(false);
        wstring cmd = L"\"C:\\Program Files\\x\\prog.exe\"";
        for (size_t i = 0; i < args.size(); ++i) {
            cmd += L" " + quoteArg(utf8ToWide(args[i]));
        }
        vector<wstring> parsed = parseCommandLine(cmd);
        vector<string> got;
        for (size_t i = 1; i < parsed.size(); ++i) {
            got.push_back(wideToUtf8(parsed[i]));
        }
        CHECK(!parsed.empty() && parsed[0] == L"C:\\Program Files\\x\\prog.exe", "program name");
        CHECK(got == args, ("arguments: " + show(got)).c_str());
        CHECK(parseCommandLineCRT(cmd) == parsed, "same parsing by the MSVC runtime rules");
        CHECK(quoteArg(L"plain") == L"\"plain\"", "always quoted");
        wstring q;
        CHECK(!quoteBatchArg(L"a\nb", &q), "quoteBatchArg rejects a line break");
        CHECK(isBatchFile(L"C:\\x\\a.BAT") && isBatchFile(L"b.cmd") && !isBatchFile(L"c.exe"), "isBatchFile");
        CHECK(isBatchFile(L"x.bat.") && isBatchFile(L"x.bat ") && isBatchFile(L"x.CMD. .") && !isBatchFile(L"x.bat.exe"),
              "isBatchFile ignores trailing dots and spaces (CVE-2024-43402)");
        wstring path;
        string error;
        CHECK(!resolveProgram(L"sub\\prog.exe", false, &path, &error), ("relative path with a directory is rejected: " + error).c_str());
        CHECK(!resolveProgram(L"prog.exe:stream", false, &path, &error), ("':' is rejected: " + error).c_str());
        CHECK(resolveProgram(self + L". ", false, &path, &error) && path == self, "full path with trailing dots and spaces");
    }

    SetEnvironmentVariableW(L"RUNSCRIPT_TEST_OUT", outFile.c_str());

    // 2. run a program directly
    std::printf("direct:\n");
    {
        vector<string> args = commonArgs(false);
        vector<wstring> wargs;
        for (size_t i = 0; i < args.size(); ++i) {
            wargs.push_back(utf8ToWide(args[i]));
        }
        wstring app, cmd;
        string error;
        vector<string> errLines;
        unsigned long code = 0;
        bool ok = buildCommandLine(self, L"", wargs, &app, &cmd, &error) && runProcess(app, cmd, &errLines, &code, &error) == eRunOK;
        CHECK(ok, ("started" + (ok ? string() : ": " + error)).c_str());
        vector<string> got = readChildArgs(outFile);
        CHECK(got == args, ("arguments: " + show(got)).c_str());
        CHECK(code == 3, "exit code 3");
        CHECK(errLines.size() == 2 && errLines[0] == "child stderr line 1" && errLines[1] == "child stderr line 2",
              ("stderr: " + show(errLines)).c_str());
    }

    // 3. a batch script, run by cmd.exe, passing %* to a program
    std::printf("batch, %%*:\n");
    {
        vector<string> args = commonArgs(true);
        string error;
        vector<string> errLines;
        unsigned long code = 0;
        RunResult r = runScript("@echo off\n\"%RUNSCRIPT_TEST_EXE%\" %*\nexit /b %ERRORLEVEL%\n", args, &errLines, &code, &error);
        CHECK(r == eRunOK, ("started" + (r == eRunOK ? string() : ": " + error)).c_str());
        vector<string> got = readChildArgs(outFile);
        CHECK(got == args, ("arguments: " + show(got)).c_str());
        CHECK(code == 3, ("exit code 3 (got " + std::to_string(code) + ")").c_str());
        CHECK(!markerExists(), "no command injection (marker file absent)");
        CHECK(errLines.size() == 2 && errLines[0] == "child stderr line 1" && errLines[1] == "child stderr line 2",
              ("stderr: " + show(errLines)).c_str());
        vector<string> bad(1, "line\nbreak");
        CHECK(runScript("@echo off\n", bad, &errLines, &code, &error) == eRunFailed, "a line break in a batch argument is an error");
    }

    // 4. a batch script using the documented "%~1" form
    std::printf("batch, \"%%~1\":\n");
    {
        vector<string> args;
        args.push_back("with space");
        args.push_back("a&b|c<d>e^f");
        args.push_back("100% %PATH%");
        args.push_back("C:\\dir\\");
        args.push_back("quote\"inside");
        args.push_back(kCanaryMarker);
        args.push_back(kCanaryExit);
        args.push_back(kCanaryBackslash);
        args.push_back("\xC3\xA9\xE6\x97\xA5\xE6\x9C\xAC");
        string error;
        vector<string> errLines;
        unsigned long code = 0;
        RunResult r = runScript("@echo off\n\"%RUNSCRIPT_TEST_EXE%\" \"%~1\" \"%~2\" \"%~3\" \"%~4\" \"%~5\" \"%~6\" \"%~7\" \"%~8\" \"%~9\"\n"
                                "exit /b %ERRORLEVEL%\n",
                                args, &errLines, &code, &error);
        CHECK(r == eRunOK, ("started" + (r == eRunOK ? string() : ": " + error)).c_str());
        vector<string> got = readChildArgs(outFile);
        CHECK(got == args, ("arguments: " + show(got)).c_str());
        CHECK(code == 3, ("exit code 3 (got " + std::to_string(code) + ")").c_str());
        CHECK(!markerExists(), "no command injection (marker file absent)");
    }

    // 5. a script with a shebang line: run by the interpreter (here, this program)
    std::printf("shebang:\n");
    {
        vector<string> args = commonArgs(false);
        string error;
        vector<string> errLines;
        unsigned long code = 0;
        RunResult r = runScript("#!\"" + wideToUtf8(self) + "\" --opt\nprint('hello')\n", args, &errLines, &code, &error);
        CHECK(r == eRunOK, ("started" + (r == eRunOK ? string() : ": " + error)).c_str());
        vector<string> got = readChildArgs(outFile);
        CHECK(got.size() == args.size() + 2 && got[0] == "--opt" && got[1].size() > 4 && got[1].compare(got[1].size() - 4, 4, ".txt") == 0
                  && vector<string>(got.begin() + 2, got.end()) == args,
              ("arguments: " + show(got)).c_str());
        CHECK(code == 3, "exit code 3");
        CHECK(runScript("#!\"C:\\does\\not\\exist.exe\"\n", args, &errLines, &code, &error) == eRunFailed, ("missing interpreter is an error: " + error).c_str());
        CHECK(runScript("#!sub\\prog.exe\n", args, &errLines, &code, &error) == eRunFailed, ("relative interpreter is an error: " + error).c_str());
    }

    // 6. interpreter search: in %PATH% only, not in the current directory
    std::printf("interpreter search:\n");
    {
        const wstring dirPath = tmp + L"rs_test_path";
        const wstring dirCwd = tmp + L"rs_test_cwd";
        CreateDirectoryW(dirPath.c_str(), NULL);
        CreateDirectoryW(dirCwd.c_str(), NULL);
        CopyFileW(self.c_str(), (dirPath + L"\\rs_onpath.exe").c_str(), FALSE);
        CopyFileW(self.c_str(), (dirCwd + L"\\rs_planted.exe").c_str(), FALSE);
        // a batch file that the trailing dot of "fake.bat." must not hide (CVE-2024-43402)
        {
            FILE* f = _wfopen((dirPath + L"\\fake.bat").c_str(), L"wb");
            std::fputs("@echo off\r\n\"%RUNSCRIPT_TEST_EXE%\" %*\r\nexit /b %ERRORLEVEL%\r\n", f);
            std::fclose(f);
        }
        n = GetEnvironmentVariableW(L"PATH", buf, 32768);
        const wstring oldPath(buf, n);
        SetEnvironmentVariableW(L"PATH", (dirPath + L";" + oldPath).c_str());
        GetCurrentDirectoryW(32768, buf);
        const wstring oldCwd(buf);
        SetCurrentDirectoryW(dirCwd.c_str());

        vector<string> args(1, "arg");
        string error;
        vector<string> errLines;
        unsigned long code = 0;
        RunResult r = runScript("#!rs_onpath\n", args, &errLines, &code, &error);
        vector<string> got = readChildArgs(outFile);
        CHECK(r == eRunOK && got.size() == 2 && got[1] == "arg", ("a program in the PATH is found: " + error).c_str());
        r = runScript("#!/usr/bin/env rs_onpath\n", args, &errLines, &code, &error);
        got = readChildArgs(outFile);
        CHECK(r == eRunOK && got.size() == 2 && got[1] == "arg", ("#!/usr/bin/env name: " + error).c_str());
        r = runScript("#!rs_planted\n", args, &errLines, &code, &error);
        CHECK(r == eRunFailed && readChildArgs(outFile).empty(), ("a program in the current directory is not run: " + error).c_str());

        vector<string> cargs;
        cargs.push_back(kCanaryMarker);
        cargs.push_back(kCanaryBackslash);
        r = runScript("#!\"" + wideToUtf8(dirPath) + "\\fake.bat.\"\n", cargs, &errLines, &code, &error);
        got = readChildArgs(outFile);
        CHECK(r == eRunOK && got.size() == 3 && got[1] == kCanaryMarker && got[2] == kCanaryBackslash, ("\"x.bat.\" is run as a batch file: " + show(got) + error).c_str());
        CHECK(code == 3 && !markerExists(), "no command injection through \"x.bat.\"");

        SetCurrentDirectoryW(oldCwd.c_str());
        SetEnvironmentVariableW(L"PATH", oldPath.c_str());
        DeleteFileW((dirPath + L"\\rs_onpath.exe").c_str());
        DeleteFileW((dirPath + L"\\fake.bat").c_str());
        DeleteFileW((dirCwd + L"\\rs_planted.exe").c_str());
        RemoveDirectoryW(dirPath.c_str());
        RemoveDirectoryW(dirCwd.c_str());
    }

    // 7. no hang when a process started by the script keeps stderr open, and abort
    std::printf("start and abort:\n");
    {
        vector<string> args;
        string error;
        vector<string> errLines;
        unsigned long code = 0;
        DWORD t0 = GetTickCount();
        RunResult r = runScript("@echo off\nstart \"\" /b ping -n 8 127.0.0.1 >NUL\necho done 1>&2\n", args, &errLines, &code, &error);
        DWORD dt = GetTickCount() - t0;
        CHECK(r == eRunOK && code == 0, ("started: " + error).c_str());
        CHECK(dt < 4000, ("does not wait for a started process that keeps stderr open (" + std::to_string(dt) + " ms)").c_str());
        CHECK(!errLines.empty() && errLines[0] == "done", ("stderr: " + show(errLines)).c_str());

        errLines.clear();
        t0 = GetTickCount();
        r = runScript("@echo off\nping -n 30 127.0.0.1 >NUL\n", args, &errLines, &code, &error, &abortAfterAFewPolls, NULL);
        dt = GetTickCount() - t0;
        CHECK(r == eRunAborted, "aborted");
        CHECK(dt < 5000, ("abort is quick (" + std::to_string(dt) + " ms)").c_str());
    }

    DeleteFileW(gMarker.c_str());
    std::printf("Result: failures=%d\n", gFailures);

    return gFailures ? 1 : 0;
} // main

#else // !_WIN32

#include <cstdio>

int
main()
{
    std::printf("runscript_win32_test: skipped (MS Windows only)\n");

    return 0;
}

#endif
