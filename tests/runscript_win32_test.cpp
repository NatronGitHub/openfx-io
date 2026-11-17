/*
 * runscript_win32_test — tests the MS Windows process launching of RunScript (RunScript/RunScriptWin32.cpp):
 * argument quoting (checked with CommandLineToArgvW), running a program directly, a batch file through
 * cmd.exe, and a script with a shebang line; stderr capture and exit code.
 *
 * The test runs itself as the child program: when RUNSCRIPT_TEST_OUT is set, it writes its arguments
 * (parsed from GetCommandLineW() with the MSVC runtime rules, as UTF-8, each followed by a NUL) to that file,
 * prints two lines to stderr, and exits with code 3. It also checks command-injection canaries in batch files,
 * the interpreter search (PATH only, not the current directory), "x.bat." (CVE-2024-43402), a process started
 * with "start" that keeps stderr open, and abort. When RUNSCRIPT_TEST_MODE is set, the child instead floods stderr
 * ("flood"), or prints "\r"-only progress output ("cr"), to test abort and the line splitting (#111), or prints the
 * DACL of a directory ("acl"). PowerShell scripts, wsl, CR line endings and the private temporary directory (#126).
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
#include <cwctype>
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

// RUNSCRIPT_TEST_MODE=flood: write to stderr as fast as possible, until killed
// RUNSCRIPT_TEST_MODE=cr: 100000 "\r"-terminated progress lines then "frame=last" without a line break, a 200 KB line,
// and "after"; exit code 1
static int
childModeMain(const wstring& mode)
{
    if (mode == L"flood") {
        static char buf[65536];
        for (size_t i = 0; i < sizeof(buf); ++i) {
            buf[i] = (i % 2) ? '\n' : 'y';
        }
        for (;;) {
            if (std::fwrite(buf, 1, sizeof(buf), stderr) != sizeof(buf)) {
                return 1;
            }
        }
    }
    if (mode == L"cr") {
        std::setvbuf(stderr, NULL, _IOFBF, 65536);
        std::fputs(string(200000, 'x').c_str(), stderr);
        std::fputs("\nafter\n", stderr);
        for (int i = 0; i < 100000; ++i) {
            std::fprintf(stderr, "frame=%d\r", i);
        }
        std::fputs("frame=last", stderr);
        std::fflush(stderr);

        return 1;
    }
    if (mode == L"acl") {
        // print the number of ACEs of the DACL of the directory given as argument 1, and whether it is protected
        vector<wstring> args = parseCommandLineCRT(GetCommandLineW());
        if (args.size() < 2) {
            return 2;
        }
        DWORD size = 0;
        GetFileSecurityW(args[1].c_str(), DACL_SECURITY_INFORMATION, NULL, 0, &size);
        vector<char> sd(size ? size : 1);
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL dacl = NULL;
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        if (!size || !GetFileSecurityW(args[1].c_str(), DACL_SECURITY_INFORMATION, &sd[0], size, &size)
            || !GetSecurityDescriptorDacl(&sd[0], &present, &dacl, &defaulted)
            || !GetSecurityDescriptorControl(&sd[0], &control, &revision)) {
            std::fprintf(stderr, "cannot get the DACL\n");

            return 2;
        }
        std::fprintf(stderr, "aces=%d protected=%d\n", (present && dacl) ? (int)dacl->AceCount : -1, (control & SE_DACL_PROTECTED) ? 1 : 0);

        return 0;
    }

    return 2;
}

static int gModeAbortPolls = 0;
static DWORD gAbortTick = 0;

// abort 300 ms after gAbortTick
static bool
abortAfter300ms(void*)
{
    ++gModeAbortPolls;

    return (GetTickCount() - gAbortTick) > 300;
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

// clear the error message before each run, so that a failure message does not show a previous run's error
static string&
cleared(string& s)
{
    s.clear();

    return s;
}

static int gAbortPolls = 0;

static wstring
toLowerW(const wstring& s)
{
    wstring l(s);
    for (size_t i = 0; i < l.size(); ++i) {
        l[i] = (wchar_t)towlower(l[i]);
    }

    return l;
}

static bool
abortAfterAFewPolls(void*)
{
    return ++gAbortPolls > 10;
}

int
main()
{
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"RUNSCRIPT_TEST_MODE", buf, 32768);
    if (n > 0 && n < 32768) {
        return childModeMain(wstring(buf, n));
    }
    n = GetEnvironmentVariableW(L"RUNSCRIPT_TEST_OUT", buf, 32768);
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
        bool ok = buildCommandLine(self, L"", wargs, &app, &cmd, &cleared(error)) && runProcess(app, cmd, &errLines, &code, &cleared(error)) == eRunOK;
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
        RunResult r = runScript("@echo off\n\"%RUNSCRIPT_TEST_EXE%\" %*\nexit /b %ERRORLEVEL%\n", args, &errLines, &code, &cleared(error));
        CHECK(r == eRunOK, ("started" + (r == eRunOK ? string() : ": " + error)).c_str());
        vector<string> got = readChildArgs(outFile);
        CHECK(got == args, ("arguments: " + show(got)).c_str());
        CHECK(code == 3, ("exit code 3 (got " + std::to_string(code) + ")").c_str());
        CHECK(!markerExists(), "no command injection (marker file absent)");
        CHECK(errLines.size() == 2 && errLines[0] == "child stderr line 1" && errLines[1] == "child stderr line 2",
              ("stderr: " + show(errLines)).c_str());
        vector<string> bad(1, "line\nbreak");
        CHECK(runScript("@echo off\n", bad, &errLines, &code, &cleared(error)) == eRunFailed, "a line break in a batch argument is an error");
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
                                args, &errLines, &code, &cleared(error));
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
        RunResult r = runScript("#!\"" + wideToUtf8(self) + "\" --opt\nprint('hello')\n", args, &errLines, &code, &cleared(error));
        CHECK(r == eRunOK, ("started" + (r == eRunOK ? string() : ": " + error)).c_str());
        vector<string> got = readChildArgs(outFile);
        CHECK(got.size() == args.size() + 2 && got[0] == "--opt" && got[1].size() > 4 && got[1].compare(got[1].size() - 4, 4, ".txt") == 0
                  && vector<string>(got.begin() + 2, got.end()) == args,
              ("arguments: " + show(got)).c_str());
        CHECK(code == 3, "exit code 3");
        CHECK(runScript("#!\"C:\\does\\not\\exist.exe\"\n", args, &errLines, &code, &cleared(error)) == eRunFailed, ("missing interpreter is an error: " + error).c_str());
        CHECK(runScript("#!sub\\prog.exe\n", args, &errLines, &code, &cleared(error)) == eRunFailed, ("relative interpreter is an error: " + error).c_str());
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
        RunResult r = runScript("#!rs_onpath\n", args, &errLines, &code, &cleared(error));
        vector<string> got = readChildArgs(outFile);
        CHECK(r == eRunOK && got.size() == 2 && got[1] == "arg", ("a program in the PATH is found: " + error).c_str());
        r = runScript("#!/usr/bin/env rs_onpath\n", args, &errLines, &code, &cleared(error));
        got = readChildArgs(outFile);
        CHECK(r == eRunOK && got.size() == 2 && got[1] == "arg", ("#!/usr/bin/env name: " + error).c_str());
        r = runScript("#!rs_planted\n", args, &errLines, &code, &cleared(error));
        CHECK(r == eRunFailed && readChildArgs(outFile).empty(), ("a program in the current directory is not run: " + error).c_str());

        vector<string> cargs;
        cargs.push_back(kCanaryMarker);
        cargs.push_back(kCanaryBackslash);
        r = runScript("#!\"" + wideToUtf8(dirPath) + "\\fake.bat.\"\n", cargs, &errLines, &code, &cleared(error));
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
        RunResult r = runScript("@echo off\nstart \"\" /b ping -n 8 127.0.0.1 >NUL\n>&2 echo done\n", args, &errLines, &code, &cleared(error));
        DWORD dt = GetTickCount() - t0;
        CHECK(r == eRunOK && code == 0, ("started: " + error).c_str());
        CHECK(dt < 4000, ("does not wait for a started process that keeps stderr open (" + std::to_string(dt) + " ms)").c_str());
        CHECK(!errLines.empty() && errLines[0] == "done", ("stderr: " + show(errLines)).c_str());

        errLines.clear();
        t0 = GetTickCount();
        r = runScript("@echo off\nping -n 30 127.0.0.1 >NUL\n", args, &errLines, &code, &cleared(error), &abortAfterAFewPolls, NULL);
        dt = GetTickCount() - t0;
        CHECK(r == eRunAborted, "aborted");
        CHECK(dt < 5000, ("abort is quick (" + std::to_string(dt) + " ms)").c_str());
    }

    // 8. abort while the child floods stderr, and "\r"-only progress output (#111)
    std::printf("stderr flood and progress output:\n");
    {
        wstring app, cmd;
        string error;
        vector<string> errLines;
        unsigned long code = 0;
        SetEnvironmentVariableW(L"RUNSCRIPT_TEST_MODE", L"flood");
        bool built = buildCommandLine(self, L"", vector<wstring>(), &app, &cmd, &cleared(error));
        CHECK(built, ("command line: " + error).c_str());
        gAbortTick = GetTickCount();
        RunResult r = built ? runProcess(app, cmd, &errLines, &code, &cleared(error), &abortAfter300ms, NULL) : eRunFailed;
        DWORD dt = GetTickCount() - gAbortTick;
        CHECK(r == eRunAborted, ("aborted while flooding stderr: " + error).c_str());
        CHECK(dt < 3000, ("abort is honoured within 3 s while flooding stderr (" + std::to_string(dt) + " ms, " + std::to_string(gModeAbortPolls) + " polls)").c_str());
        CHECK(!errLines.empty() && errLines.size() <= 2000 && errLines.back().size() <= 1,
              ("stderr lines kept are bounded (" + std::to_string(errLines.size()) + ")").c_str());

        errLines.clear();
        SetEnvironmentVariableW(L"RUNSCRIPT_TEST_MODE", L"cr");
        r = built ? runProcess(app, cmd, &errLines, &code, &cleared(error)) : eRunFailed;
        CHECK(r == eRunOK && code == 1, ("progress output: started: " + error).c_str());
        CHECK(errLines.size() == 3 && errLines[0] == string(64 * 1024, 'x') + "..." && errLines[1] == "after" && errLines[2] == "frame=last",
              ("long line cut, lone \\r keeps the last segment: " + std::to_string(errLines.size()) + " lines"
               + (errLines.empty() ? string() : ", last [" + errLines.back().substr(0, 40) + "]"))
                  .c_str());
        SetEnvironmentVariableW(L"RUNSCRIPT_TEST_MODE", NULL);
    }

    // 9. "#!/usr/bin/env cmd": cmd.exe from the system directory (#111), running the rest as a batch file
    std::printf("#!/usr/bin/env cmd:\n");
    {
        wchar_t sysdir[MAX_PATH + 1];
        UINT sn = GetSystemDirectoryW(sysdir, MAX_PATH + 1);
        const wstring sys(sysdir, sn);
        wstring path;
        string error;
        bool ok = resolveProgram(L"cmd", true, &path, &cleared(error));
        CHECK(ok && lstrcmpiW(path.c_str(), (sys + L"\\cmd.exe").c_str()) == 0, ("a Unix-style \"cmd\" is the system's cmd.exe: " + wideToUtf8(path) + error).c_str());
        ok = resolveProgram(L"powershell", true, &path, &cleared(error));
        CHECK(ok && lstrcmpiW(path.c_str(), (sys + L"\\WindowsPowerShell\\v1.0\\powershell.exe").c_str()) == 0,
              ("a Unix-style \"powershell\" is the system's powershell.exe: " + wideToUtf8(path) + error).c_str());
        // bash is still not taken from the system directory (WSL's bash.exe)
        ok = resolveProgram(L"bash", true, &path, &cleared(error));
        CHECK(!ok || toLowerW(path).compare(0, sys.size() + 1, toLowerW(sys + L"\\")) != 0,
              ("a Unix-style \"bash\" is not taken from the system directory: " + (ok ? wideToUtf8(path) : error)).c_str());

        // a cmd.exe planted earlier in the PATH is not used
        const wstring dirPath = tmp + L"rs_test_cmd";
        CreateDirectoryW(dirPath.c_str(), NULL);
        CopyFileW(self.c_str(), (dirPath + L"\\cmd.exe").c_str(), FALSE);
        n = GetEnvironmentVariableW(L"PATH", buf, 32768);
        const wstring oldPath(buf, n);
        SetEnvironmentVariableW(L"PATH", (dirPath + L";" + oldPath).c_str());
        ok = resolveProgram(L"cmd", true, &path, &cleared(error));
        CHECK(ok && lstrcmpiW(path.c_str(), (sys + L"\\cmd.exe").c_str()) == 0, ("not a cmd.exe earlier in the PATH: " + wideToUtf8(path) + error).c_str());

        vector<string> args = commonArgs(true);
        vector<string> errLines;
        unsigned long code = 0;
        RunResult r = runScript("#!/usr/bin/env cmd /c\n@echo off\n\"%RUNSCRIPT_TEST_EXE%\" %*\nexit /b %ERRORLEVEL%\n", args, &errLines, &code, &cleared(error));
        CHECK(r == eRunOK, ("started" + (r == eRunOK ? string() : ": " + error)).c_str());
        vector<string> got = readChildArgs(outFile);
        CHECK(got == args, ("run as a batch file, arguments: " + show(got)).c_str());
        CHECK(code == 3, ("exit code 3 (got " + std::to_string(code) + ")").c_str());
        CHECK(!markerExists(), "no command injection (marker file absent)");

        SetEnvironmentVariableW(L"PATH", oldPath.c_str());
        DeleteFileW((dirPath + L"\\cmd.exe").c_str());
        RemoveDirectoryW(dirPath.c_str());
    }

    // 10. review of #126: PowerShell runs the script with -File, wsl is rejected, CR-only line endings, cmd and
    // powershell from the system directory even if it is not in the PATH, private temporary directory
    std::printf("powershell, wsl, CR line endings, system programs, private directory:\n");
    {
        wchar_t sysdir[MAX_PATH + 1];
        UINT sn = GetSystemDirectoryW(sysdir, MAX_PATH + 1);
        const wstring sys(sysdir, sn);
        n = GetEnvironmentVariableW(L"PATH", buf, 32768);
        const wstring oldPath(buf, n);
        string error;
        wstring path;

        // the arguments are literal strings, even with $(...), ';' or spaces, and the script path has a space
        const string marker = wideToUtf8(gMarker);
        vector<string> args;
        args.push_back("plain");
        args.push_back("with space");
        args.push_back("x; Set-Content -LiteralPath '" + marker + "' -Value PWNED");
        args.push_back("$(Set-Content -LiteralPath '" + marker + "' -Value PWNED)");
        args.push_back("a'b");
        args.push_back("100% $env:PATH");
        n = GetEnvironmentVariableW(L"TMP", buf, 32768);
        const wstring oldTmp(buf, n);
        n = GetEnvironmentVariableW(L"TEMP", buf, 32768);
        const wstring oldTemp(buf, n);
        const wstring spaceTmp = tmp + L"rs test tmp";
        CreateDirectoryW(spaceTmp.c_str(), NULL);
        SetEnvironmentVariableW(L"TMP", spaceTmp.c_str());
        SetEnvironmentVariableW(L"TEMP", spaceTmp.c_str());
        const string psScript = "[IO.File]::WriteAllText($env:RUNSCRIPT_TEST_OUT, ($args -join \"`0\") + \"`0\")\nexit 3\n";
        for (int unixStyle = 0; unixStyle < 2; ++unixStyle) {
            vector<string> errLines;
            unsigned long code = 0;
            const string shebang = unixStyle ? "#!/usr/bin/env powershell -NoProfile -ExecutionPolicy Bypass\n" : "#!powershell -NoProfile -ExecutionPolicy Bypass\n";
            RunResult r = runScript(shebang + psScript, args, &errLines, &code, &cleared(error));
            vector<string> got = readChildArgs(outFile);
            CHECK(r == eRunOK && code == 3, (shebang.substr(0, shebang.size() - 1) + ": started, exit code 3 (got " + std::to_string(code) + "): " + error + show(errLines)).c_str());
            CHECK(got == args, ("arguments passed as literal strings (-File): " + show(got)).c_str());
            CHECK(!markerExists(), "no code run from the arguments (marker file absent)");
            DeleteFileW(gMarker.c_str());
        }
        SetEnvironmentVariableW(L"TMP", oldTmp.c_str());
        SetEnvironmentVariableW(L"TEMP", oldTemp.c_str());
        RemoveDirectoryW(spaceTmp.c_str());
        {
            vector<string> errLines;
            unsigned long code = 0;
            CHECK(runScript("#!powershell -Command\n", args, &errLines, &code, &cleared(error)) == eRunFailed, ("powershell -Command is an error: " + error).c_str());
            CHECK(runScript("#!/usr/bin/env powershell -EncodedCommand\n", args, &errLines, &code, &cleared(error)) == eRunFailed, ("powershell -EncodedCommand is an error: " + error).c_str());
            CHECK(runScript("#!powershell -File -NoProfile\n", args, &errLines, &code, &cleared(error)) == eRunFailed, ("-File not last is an error: " + error).c_str());
            CHECK(runScript("#!wsl\n", args, &errLines, &code, &cleared(error)) == eRunFailed, ("#!wsl is an error: " + error).c_str());
            // the dashes that PowerShell accepts
            const char* const badOptions[] = { "--command", "/Command", "\xE2\x80\x93" "Command", "\xE2\x80\x94" "c", "\xE2\x80\x95" "EncodedCommand",
                                               "\xE2\x80\x93-command", "--ec", "--file -NoProfile", "\xE2\x80\x94" "File -NoProfile" };
            for (size_t i = 0; i < sizeof(badOptions) / sizeof(badOptions[0]); ++i) {
                CHECK(runScript(string("#!powershell ") + badOptions[i] + "\n", args, &errLines, &code, &cleared(error)) == eRunFailed,
                      ("#!powershell " + string(badOptions[i]) + " is an error: " + error).c_str());
            }
        }
        // pwsh (here, a copy of this program), with any dash: -File is appended unless the last option is -File
        {
            const wstring dirPath = tmp + L"rs_test_pwsh";
            CreateDirectoryW(dirPath.c_str(), NULL);
            CopyFileW(self.c_str(), (dirPath + L"\\pwsh.exe").c_str(), FALSE);
            const string pwsh = "#!\"" + wideToUtf8(dirPath) + "\\pwsh.exe\" ";
            vector<string> pargs(1, "arg");
            vector<string> errLines;
            unsigned long code = 0;
            CHECK(runScript(pwsh + "--command\n", pargs, &errLines, &code, &cleared(error)) == eRunFailed && readChildArgs(outFile).empty(),
                  ("#!pwsh --command is an error: " + error).c_str());
            CHECK(runScript(pwsh + "\xE2\x80\x93" "Command\n", pargs, &errLines, &code, &cleared(error)) == eRunFailed && readChildArgs(outFile).empty(),
                  ("#!pwsh \xE2\x80\x93" "Command is an error: " + error).c_str());
            RunResult r = runScript(pwsh + "\xE2\x80\x93NoProfile\n", pargs, &errLines, &code, &cleared(error));
            vector<string> got = readChildArgs(outFile);
            CHECK(r == eRunOK && got.size() == 4 && got[0] == "\xE2\x80\x93NoProfile" && got[1] == "-File" && got[3] == "arg",
                  ("#!pwsh \xE2\x80\x93NoProfile: -File appended: " + show(got) + error).c_str());
            r = runScript(pwsh + "--file\n", pargs, &errLines, &code, &cleared(error));
            got = readChildArgs(outFile);
            CHECK(r == eRunOK && got.size() == 3 && got[0] == "--file" && got[2] == "arg", ("#!pwsh --file: nothing appended: " + show(got) + error).c_str());
            DeleteFileW((dirPath + L"\\pwsh.exe").c_str());
            RemoveDirectoryW(dirPath.c_str());
        }
        {
            vector<string> errLines;
            unsigned long code = 0;
            CHECK(runScript("#!/usr/bin/env wsl\n", args, &errLines, &code, &cleared(error)) == eRunFailed, ("#!/usr/bin/env wsl is an error: " + error).c_str());
        }

        // CR-only line endings (classic Mac OS)
        {
            vector<string> cargs = commonArgs(true);
            vector<string> errLines;
            unsigned long code = 0;
            RunResult r = runScript("#!/usr/bin/env cmd\r@echo off\r\"%RUNSCRIPT_TEST_EXE%\" %*\rexit /b %ERRORLEVEL%\r", cargs, &errLines, &code, &cleared(error));
            vector<string> got = readChildArgs(outFile);
            CHECK(r == eRunOK && code == 3 && got == cargs, ("#!/usr/bin/env cmd with CR line endings: " + std::to_string(code) + " " + show(got) + error).c_str());
            r = runScript("@echo off\r\"%RUNSCRIPT_TEST_EXE%\" %*\rexit /b %ERRORLEVEL%\r", cargs, &errLines, &code, &cleared(error));
            got = readChildArgs(outFile);
            CHECK(r == eRunOK && code == 3 && got == cargs, ("batch file with CR line endings: " + std::to_string(code) + " " + show(got) + error).c_str());
            CHECK(!markerExists(), "no command injection (marker file absent)");
        }

        // cmd and powershell from the system directory, even if it is not in the PATH, and not a cmd.exe in the PATH
        {
            const wstring dirPath = tmp + L"rs_test_cmd2";
            CreateDirectoryW(dirPath.c_str(), NULL);
            CopyFileW(self.c_str(), (dirPath + L"\\cmd.exe").c_str(), FALSE);
            CopyFileW(self.c_str(), (dirPath + L"\\powershell.exe").c_str(), FALSE);
            SetEnvironmentVariableW(L"PATH", dirPath.c_str());
            bool ok = resolveProgram(L"cmd", false, &path, &cleared(error));
            CHECK(ok && lstrcmpiW(path.c_str(), (sys + L"\\cmd.exe").c_str()) == 0, ("#!cmd is the system's cmd.exe: " + wideToUtf8(path) + error).c_str());
            ok = resolveProgram(L"powershell.exe", false, &path, &cleared(error));
            CHECK(ok && lstrcmpiW(path.c_str(), (sys + L"\\WindowsPowerShell\\v1.0\\powershell.exe").c_str()) == 0,
                  ("#!powershell.exe is the system's powershell.exe: " + wideToUtf8(path) + error).c_str());
            ok = resolveProgram(L"powershell.com", false, &path, &cleared(error));
            CHECK(ok && lstrcmpiW(path.c_str(), (sys + L"\\WindowsPowerShell\\v1.0\\powershell.exe").c_str()) == 0,
                  ("#!powershell.com is the system's powershell.exe: " + wideToUtf8(path) + error).c_str());
            ok = resolveProgram(L"CMD.COM", true, &path, &cleared(error));
            CHECK(ok && lstrcmpiW(path.c_str(), (sys + L"\\cmd.exe").c_str()) == 0, ("#!/usr/bin/env CMD.COM is the system's cmd.exe: " + wideToUtf8(path) + error).c_str());
            SetEnvironmentVariableW(L"PATH", oldPath.c_str());
            DeleteFileW((dirPath + L"\\cmd.exe").c_str());
            DeleteFileW((dirPath + L"\\powershell.exe").c_str());
            RemoveDirectoryW(dirPath.c_str());
        }

        // the script is in a private directory, removed afterwards
        {
            SetEnvironmentVariableW(L"RUNSCRIPT_TEST_MODE", L"acl");
            vector<string> errLines;
            unsigned long code = 0;
            RunResult r = runScript("@echo off\n>&2 echo %~dp0\n\"%RUNSCRIPT_TEST_EXE%\" \"%~dp0.\"\n", vector<string>(), &errLines, &code, &cleared(error));
            SetEnvironmentVariableW(L"RUNSCRIPT_TEST_MODE", NULL);
            CHECK(r == eRunOK && code == 0 && errLines.size() == 2, ("started: " + error + show(errLines)).c_str());
            if (errLines.size() == 2) {
                const wstring dir = utf8ToWide(errLines[0]);
                CHECK(lstrcmpiW(dir.substr(0, tmp.size()).c_str(), tmp.c_str()) == 0 && dir.size() > tmp.size() + 1,
                      ("in a subdirectory of the temporary directory: " + errLines[0]).c_str());
                CHECK(errLines[1] == "aces=2 protected=1", ("private DACL (the user and SYSTEM, not inherited): " + errLines[1]).c_str());
                CHECK(GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES, "the directory is removed afterwards");
            }
        }
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
