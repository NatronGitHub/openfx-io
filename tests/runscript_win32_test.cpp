/*
 * runscript_win32_test — tests the MS Windows process launching of RunScript (RunScript/RunScriptWin32.cpp):
 * argument quoting (checked with CommandLineToArgvW), running a program directly, a batch file through
 * cmd.exe, and a script with a shebang line; stderr capture and exit code.
 *
 * The test runs itself as the child program: when RUNSCRIPT_TEST_OUT is set, it writes its arguments
 * (from CommandLineToArgvW(GetCommandLineW()), as UTF-8, each followed by a NUL) to that file, prints a line
 * to stderr, and exits with code 3.
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
    vector<wstring> args = parseCommandLine(GetCommandLineW());
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
    a.push_back("\xC3\xA9\xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\xAC"); // é日本 🎬
    if (!batch) {
        // '"' and line breaks cannot be passed reliably through cmd.exe
        a.push_back("quote\"inside");
        a.push_back("\\\"backslash quote\\\"");
        a.push_back("line\nbreak");
    }

    return a;
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
    const wstring outFile = wstring(buf, n) + L"runscript_win32_test_out.bin";
    SetEnvironmentVariableW(L"RUNSCRIPT_TEST_EXE", self.c_str());

    // 1. quoteArg() round-trips through CommandLineToArgvW
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
        wstring q;
        CHECK(!quoteBatchArg(L"a\nb", &q), "quoteBatchArg rejects a line break");
        CHECK(isBatchFile(L"C:\\x\\a.BAT") && isBatchFile(L"b.cmd") && !isBatchFile(L"c.exe"), "isBatchFile");
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
        bool ok = buildCommandLine(self, L"", wargs, &app, &cmd, &error) && runProcess(app, cmd, &errLines, &code, &error);
        CHECK(ok, ("started" + (ok ? string() : ": " + error)).c_str());
        vector<string> got = readChildArgs(outFile);
        CHECK(got == args, ("arguments: " + show(got)).c_str());
        CHECK(code == 3, "exit code 3");
        CHECK(errLines.size() == 2 && errLines[0] == "child stderr line 1" && errLines[1] == "child stderr line 2",
              ("stderr: " + show(errLines)).c_str());
    }

    // 3. a batch script, run by cmd.exe
    std::printf("batch:\n");
    {
        vector<string> args = commonArgs(true);
        string error;
        vector<string> errLines;
        unsigned long code = 0;
        bool ok = runScript("@echo off\n\"%RUNSCRIPT_TEST_EXE%\" %*\nexit /b %ERRORLEVEL%\n", args, &errLines, &code, &error);
        CHECK(ok, ("started" + (ok ? string() : ": " + error)).c_str());
        vector<string> got = readChildArgs(outFile);
        CHECK(got == args, ("arguments: " + show(got)).c_str());
        CHECK(code == 3, "exit code 3");
        CHECK(errLines.size() == 2 && errLines[0] == "child stderr line 1" && errLines[1] == "child stderr line 2",
              ("stderr: " + show(errLines)).c_str());
        vector<string> bad(1, "line\nbreak");
        CHECK(!runScript("@echo off\n", bad, &errLines, &code, &error), "a line break in a batch argument is an error");
    }

    // 4. a script with a shebang line: run by the interpreter (here, this program)
    std::printf("shebang:\n");
    {
        vector<string> args = commonArgs(false);
        string error;
        vector<string> errLines;
        unsigned long code = 0;
        bool ok = runScript("#!\"" + wideToUtf8(self) + "\" --opt\nprint('hello')\n", args, &errLines, &code, &error);
        CHECK(ok, ("started" + (ok ? string() : ": " + error)).c_str());
        vector<string> got = readChildArgs(outFile);
        CHECK(got.size() == args.size() + 2 && got[0] == "--opt" && got[1].size() > 4 && got[1].compare(got[1].size() - 4, 4, ".txt") == 0
                  && vector<string>(got.begin() + 2, got.end()) == args,
              ("arguments: " + show(got)).c_str());
        CHECK(code == 3, "exit code 3");
        CHECK(!runScript("#!\"C:\\does\\not\\exist.exe\"\n", args, &errLines, &code, &error), ("missing interpreter is an error: " + error).c_str());
    }

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
