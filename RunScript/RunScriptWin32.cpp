/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
 * (C) 2018-2021 The Natron Developers
 * (C) 2013-2018 INRIA
 *
 * openfx-io is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-io is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-io.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

/*
 * OFX RunScript plugin: running the script on MS Windows (the POSIX version uses pstream).
 */

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)

// PROC_THREAD_ATTRIBUTE_HANDLE_LIST needs Windows Vista
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "RunScriptWin32.h"
#include "RunScriptOutput.h"

#include <cstdio> // snprintf, _snwprintf
#include <cwchar> // wcslen
#include <cwctype>
#include <string>
#include <vector>

using std::string;
using std::vector;
using std::wstring;

namespace RunScriptWin32 {

wstring
utf8ToWide(const string& s)
{
    if (s.empty()) {
        return wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    if (n <= 0) {
        return wstring();
    }
    wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);

    return w;
}

string
wideToUtf8(const wstring& w)
{
    if (w.empty()) {
        return string();
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) {
        return string();
    }
    string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);

    return s;
}

// the system error message for an error code, in UTF-8
static string
systemErrorMessage(DWORD err)
{
    wchar_t* buf = NULL;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, err, 0, (LPWSTR)&buf, 0, NULL);
    wstring msg;
    if (n && buf) {
        msg.assign(buf, n);
        while (!msg.empty() && (msg[msg.size() - 1] == L'\r' || msg[msg.size() - 1] == L'\n' || msg[msg.size() - 1] == L' ' || msg[msg.size() - 1] == L'.')) {
            msg.erase(msg.size() - 1);
        }
    }
    if (buf) {
        LocalFree(buf);
    }
    char code[32];
    snprintf(code, sizeof(code), "error %lu", (unsigned long)err);

    return msg.empty() ? string(code) : (wideToUtf8(msg) + " (" + code + ")");
}

wstring
quoteArg(const wstring& arg)
{
    // See "Everyone quotes command line arguments the wrong way" (Daniel Colascione) and the
    // "Parsing C++ command-line arguments" page of the Microsoft C++ documentation.
    // Always quoted: MSYS2/Cygwin programs expand wildcards in unquoted arguments.
    wstring quoted(1, L'"');
    for (wstring::const_iterator it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            // double the backslashes before the closing quote
            quoted.append(backslashes * 2, L'\\');
            break;
        } else if (*it == L'"') {
            // double the backslashes, and escape the quote
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            // backslashes are literal when not followed by a quote
            quoted.append(backslashes, L'\\');
            quoted.push_back(*it);
        }
    }
    quoted.push_back(L'"');

    return quoted;
}

// '%' cannot be escaped in a cmd.exe command line (^% does not work within quotes), but "%%cd:~,%" expands
// to a single '%' which cannot start a variable reference with a following '%' (this needs command extensions,
// /e:ON). This is the escape used by Rust's std::process for batch files (CVE-2024-24576).
static void
appendBatchChar(wstring* out,
                wchar_t c)
{
    if (c == L'%') {
        out->append(L"%%cd:~,%");
    } else {
        out->push_back(c);
    }
}

bool
quoteBatchArg(const wstring& arg,
              wstring* quoted)
{
    if (arg.find_first_of(wstring(L"\r\n\0", 3)) != wstring::npos) {
        return false;
    }
    // Always quote: within quotes, cmd.exe does not interpret & | < > ^ ( ) and the separators , ; = space,
    // and with /v:OFF '!' is literal. A '"' is doubled, so that cmd.exe's quote state is unchanged, and the
    // backslashes before a '"' (including the closing one) are doubled, so that a program that receives
    // "%~1" or %* from the batch file decodes the original argument.
    quoted->assign(1, L'"');
    size_t backslashes = 0;
    for (wstring::const_iterator it = arg.begin(); it != arg.end(); ++it) {
        if (*it == L'\\') {
            ++backslashes;
            quoted->push_back(L'\\');
            continue;
        }
        if (*it == L'"') {
            quoted->append(backslashes, L'\\');
            quoted->append(L"\"\"");
        } else {
            appendBatchChar(quoted, *it);
        }
        backslashes = 0;
    }
    quoted->append(backslashes, L'\\');
    quoted->push_back(L'"');

    return true;
}

static wstring
toLower(const wstring& s)
{
    wstring l(s);
    for (size_t i = 0; i < l.size(); ++i) {
        l[i] = (wchar_t)std::towlower(l[i]);
    }

    return l;
}

static bool
endsWith(const wstring& s,
         const wchar_t* suffix)
{
    const wstring suf(suffix);

    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

wstring
stripTrailingDotsAndSpaces(const wstring& path)
{
    size_t n = path.size();
    while (n > 0 && (path[n - 1] == L'.' || path[n - 1] == L' ')) {
        --n;
    }

    return path.substr(0, n);
}

bool
isBatchFile(const wstring& program)
{
    const wstring l = toLower(stripTrailingDotsAndSpaces(program));

    return endsWith(l, L".bat") || endsWith(l, L".cmd");
}

static bool
isExeFile(const wstring& program)
{
    const wstring l = toLower(stripTrailingDotsAndSpaces(program));

    return endsWith(l, L".exe") || endsWith(l, L".com");
}

static bool
isAbsolutePath(const wstring& p)
{
    // C:\... or \\server\share\...
    return (p.size() >= 3 && p[1] == L':' && p[2] == L'\\' && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')))
        || (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\');
}

static bool
isFile(const wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());

    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// the file names to look for: name itself if it has an executable extension, else name.exe and name.com
static vector<wstring>
programCandidates(const wstring& name)
{
    vector<wstring> c;
    if (isExeFile(name) || isBatchFile(name)) {
        c.push_back(name);
    } else {
        c.push_back(name + L".exe");
        c.push_back(name + L".com");
    }

    return c;
}

static wstring
withoutTrailingBackslash(const wstring& dir)
{
    wstring d = dir;
    while (d.size() > 3 && d[d.size() - 1] == L'\\') {
        d.erase(d.size() - 1);
    }

    return d;
}

// The Windows system programs that a Unix-style name ("#!/usr/bin/env cmd") may designate, and their path in the
// system directory, or NULL. Only these are taken from the system directory, which is otherwise skipped for Unix
// names (see resolveProgram()): they have no Unix counterpart that the name could mean instead (unlike bash.exe,
// which is WSL's), and they are only taken from the system directory, never from the PATH.
static const wchar_t*
unixNameSystemProgram(const wstring& name)
{
    wstring l = toLower(name);
    if (endsWith(l, L".exe")) {
        l.erase(l.size() - 4);
    }
    if (l == L"cmd") {
        return L"cmd.exe";
    }
    if (l == L"powershell") {
        return L"WindowsPowerShell\\v1.0\\powershell.exe";
    }
    if (l == L"wsl") {
        return L"wsl.exe";
    }

    return NULL;
}

bool
resolveProgram(const wstring& programName,
               bool fromUnixPath,
               wstring* fullPath,
               string* error)
{
    wstring name = stripTrailingDotsAndSpaces(programName);
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == L'/') {
            name[i] = L'\\';
        }
    }
    if (name.empty() || name.find_first_of(L"\"*?<>|") != wstring::npos) {
        *error = "invalid program name \"" + wideToUtf8(programName) + "\"";

        return false;
    }
    // a ':' is only allowed after the drive letter (not "prog:stream", not "C:prog")
    size_t colon = name.find(L':');
    if (colon != wstring::npos && (colon != 1 || name.find(L':', 2) != wstring::npos || !isAbsolutePath(name))) {
        *error = "invalid program name \"" + wideToUtf8(programName) + "\" (use a full path, e.g. C:\\dir\\prog.exe)";

        return false;
    }
    vector<wstring> candidates = programCandidates(name);
    if (isAbsolutePath(name)) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (isFile(candidates[i])) {
                *fullPath = candidates[i];

                return true;
            }
        }
        *error = "cannot find " + wideToUtf8(name);

        return false;
    }
    if (name.find(L'\\') != wstring::npos) {
        *error = "the program \"" + wideToUtf8(programName) + "\" must be a file name, found in the PATH, or a full path";

        return false;
    }

    // search %PATH% only: CreateProcess and SearchPath (with the default search path) would look in the host's
    // directory and in the current directory (often the project directory) first
    wstring systemDir; // lowercase, for comparisons
    wstring systemDirPath;
    {
        wchar_t sysdir[MAX_PATH + 1];
        UINT n = GetSystemDirectoryW(sysdir, MAX_PATH + 1);
        if (n > 0 && n <= MAX_PATH) {
            systemDirPath = withoutTrailingBackslash(wstring(sysdir, n));
            systemDir = toLower(systemDirPath);
        }
    }
    if (fromUnixPath) {
        const wchar_t* sysProgram = unixNameSystemProgram(name);
        if (sysProgram) {
            if (systemDirPath.empty()) {
                *error = "cannot find the system directory";

                return false;
            }
            const wstring p = systemDirPath + L"\\" + sysProgram;
            if (!isFile(p)) {
                *error = "cannot find " + wideToUtf8(p);

                return false;
            }
            *fullPath = p;

            return true;
        }
    }
    wstring path;
    DWORD n = GetEnvironmentVariableW(L"PATH", NULL, 0);
    if (n > 0) {
        vector<wchar_t> buf(n + 1);
        n = GetEnvironmentVariableW(L"PATH", &buf[0], (DWORD)buf.size());
        if (n > 0 && n < buf.size()) {
            path.assign(&buf[0], n);
        }
    }
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(L';', start);
        if (end == wstring::npos) {
            end = path.size();
        }
        wstring dir = path.substr(start, end - start);
        start = end + 1;
        if (dir.size() >= 2 && dir[0] == L'"' && dir[dir.size() - 1] == L'"') {
            dir = dir.substr(1, dir.size() - 2);
        }
        dir = withoutTrailingBackslash(dir);
        // skip relative directories (relative to the current directory), and the system directory for Unix names
        if (!isAbsolutePath(dir) || (fromUnixPath && toLower(dir) == systemDir)) {
            continue;
        }
        for (size_t i = 0; i < candidates.size(); ++i) {
            wstring p = dir + L"\\" + candidates[i];
            if (isFile(p)) {
                *fullPath = p;

                return true;
            }
        }
    }
    *error = "cannot find " + wideToUtf8(name) + " in the PATH";

    return false;
} // resolveProgram

bool
buildCommandLine(const wstring& programPath,
                 const wstring& extraArgs,
                 const vector<wstring>& args,
                 wstring* applicationName,
                 wstring* commandLine,
                 string* error)
{
    const wstring program = stripTrailingDotsAndSpaces(programPath);
    if (program.empty() || program.find(L'"') != wstring::npos || !isAbsolutePath(program)) {
        *error = "invalid program name \"" + wideToUtf8(programPath) + "\"";

        return false;
    }
    if (isBatchFile(program)) {
        // CreateProcess would run a batch file with cmd.exe, but with the quoting of the arguments for
        // CommandLineToArgvW, which is not safe for cmd.exe (CVE-2024-24576, CVE-2024-43402): run cmd.exe /c
        // explicitly, from the system directory. cmd.exe removes the first and the last quote after /c, and
        // parses the rest as a command line.
        wchar_t sysdir[MAX_PATH + 1];
        UINT n = GetSystemDirectoryW(sysdir, MAX_PATH + 1);
        if (n == 0 || n > MAX_PATH) {
            *error = "cannot find cmd.exe: " + systemErrorMessage(GetLastError());

            return false;
        }
        *applicationName = wstring(sysdir, n) + L"\\cmd.exe";
        wstring cmd = L"cmd.exe /e:ON /v:OFF /d /c \"\"";
        for (size_t i = 0; i < program.size(); ++i) {
            appendBatchChar(&cmd, program[i]);
        }
        cmd.push_back(L'"');
        if (!extraArgs.empty()) {
            cmd.push_back(L' ');
            cmd.append(extraArgs);
        }
        for (size_t i = 0; i < args.size(); ++i) {
            wstring q;
            if (!quoteBatchArg(args[i], &q)) {
                *error = "argument " + std::to_string(i + 1) + " contains a line break, which cannot be passed to a batch file";

                return false;
            }
            cmd.push_back(L' ');
            cmd.append(q);
        }
        cmd.push_back(L'"');
        *commandLine = cmd;
    } else if (isExeFile(program)) {
        // The program name is quoted without escapes (CommandLineToArgvW does not process backslashes in it).
        *applicationName = program;
        wstring cmd = L"\"" + program + L"\"";
        if (!extraArgs.empty()) {
            cmd.push_back(L' ');
            cmd.append(extraArgs);
        }
        for (size_t i = 0; i < args.size(); ++i) {
            cmd.push_back(L' ');
            cmd.append(quoteArg(args[i]));
        }
        *commandLine = cmd;
    } else {
        *error = "cannot run " + wideToUtf8(program) + ": not a .exe, .com, .bat or .cmd file";

        return false;
    }

    return true;
} // buildCommandLine

static void
closeHandle(HANDLE* h)
{
    if (*h != NULL && *h != INVALID_HANDLE_VALUE) {
        CloseHandle(*h);
    }
    *h = NULL;
}

enum ReadStatus {
    eReadEOF = 0, // end of file (or error)
    eReadDrained, // the pipe is empty
    eReadMore // the pipe may contain more: call again soon
};

// Read what is available in the pipe without blocking. At most kMaxReads reads of 4 KB are done, so that the caller
// checks for abort regularly, even if the process writes faster than we read (e.g. "yes >&2").
static ReadStatus
readAvailable(HANDLE pipe,
              RunScriptOutput::LineCollector* lines)
{
    const int kMaxReads = 16;
    for (int i = 0; i < kMaxReads; ++i) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) {
            return eReadEOF; // ERROR_BROKEN_PIPE: all the writers have closed it
        }
        if (avail == 0) {
            return eReadDrained;
        }
        char buf[4096];
        DWORD n = 0;
        if (!ReadFile(pipe, buf, avail < sizeof(buf) ? avail : (DWORD)sizeof(buf), &n, NULL) || n == 0) {
            return eReadEOF;
        }
        lines->add(buf, n);
    }

    return eReadMore;
}

// an inheritable copy of h
static HANDLE
inheritableCopy(HANDLE h)
{
    HANDLE dup = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        return NULL;
    }

    return dup;
}

// the environment of the host, with NoDefaultCurrentDirectoryInExePath set, so that cmd.exe (when it runs a batch
// file) does not search the programs in the current directory first; empty if the host's environment cannot be
// read (the child then inherits it unchanged, rather than getting an environment without PATH or SystemRoot)
static vector<wchar_t>
childEnvironment()
{
    vector<wchar_t> env;
    const wstring var = L"NoDefaultCurrentDirectoryInExePath=";
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) {
        return env;
    }
    {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            const wstring entry(p);
            if (toLower(entry.substr(0, var.size())) == toLower(var)) {
                continue;
            }
            env.insert(env.end(), entry.begin(), entry.end());
            env.push_back(L'\0');
        }
        FreeEnvironmentStringsW(block);
    }
    const wstring mine = var + L"1";
    env.insert(env.end(), mine.begin(), mine.end());
    env.push_back(L'\0');
    env.push_back(L'\0');

    return env;
}

RunResult
runProcess(const wstring& applicationName,
           const wstring& commandLine,
           vector<string>* errLines,
           unsigned long* exitCode,
           string* error,
           AbortCallback aborted,
           void* abortContext)
{
    *exitCode = 0;
    const string programName = wideToUtf8(applicationName);

    // All our handles are created non-inheritable, and only the copies given to the child are inheritable,
    // for the shortest possible time: another thread of the host that starts a process without a handle list
    // could inherit them (and if it inherited the write end of the pipe, we would never see its end of file).
    HANDLE errRead = NULL;
    HANDLE errWrite = NULL;
    // a 64 KB pipe buffer (the default is about 4 KB), so that a script that writes a lot to stderr is not slowed
    // down by our polling
    if (!CreatePipe(&errRead, &errWrite, NULL, 65536)) {
        *error = "cannot create a pipe: " + systemErrorMessage(GetLastError());

        return eRunFailed;
    }
    // stdin: NUL, so that a script waiting for input (e.g. "pause") does not block the render forever
    HANDLE inNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    // stdout: the host's stdout (like on POSIX) if it is redirected to a file or a pipe, else NUL: a GUI host
    // usually has none, and a console handle would not be valid in the child, which runs without a console window
    HANDLE outNul = NULL;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD outType = (out == NULL || out == INVALID_HANDLE_VALUE) ? FILE_TYPE_UNKNOWN : GetFileType(out);
    if (outType != FILE_TYPE_DISK && outType != FILE_TYPE_PIPE) {
        out = outNul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
    if (inNul == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE) {
        *error = "cannot open NUL: " + systemErrorMessage(GetLastError());
        closeHandle(&errRead);
        closeHandle(&errWrite);
        closeHandle(&inNul);
        closeHandle(&outNul);

        return eRunFailed;
    }

    // a job object, so that the process and its children can be killed on abort, and are killed if the host
    // dies while waiting (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
        ZeroMemory(&info, sizeof(info));
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
            closeHandle(&job);
        }
    }

    vector<wchar_t> env = childEnvironment();
    vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end()); // CreateProcessW may modify it
    cmdBuf.push_back(L'\0');
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL created = FALSE;
    DWORD createError = 0;
    {
        HANDLE childIn = inheritableCopy(inNul);
        HANDLE childOut = inheritableCopy(out);
        HANDLE childErr = inheritableCopy(errWrite);
        closeHandle(&errWrite); // the child's copy is the only write end
        if (childIn && childOut && childErr) {
            // Only let the child inherit these three handles
            HANDLE inherited[3] = { childIn, childOut, childErr };
            SIZE_T attrSize = 0;
            InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
            vector<char> attrBuf(attrSize ? attrSize : 1);
            LPPROC_THREAD_ATTRIBUTE_LIST attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(&attrBuf[0]);
            bool attrsOK = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) != 0;
            if (attrsOK) {
                attrsOK = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), NULL, NULL) != 0;
                if (!attrsOK) {
                    DeleteProcThreadAttributeList(attrs);
                }
            }

            STARTUPINFOEXW si;
            ZeroMemory(&si, sizeof(si));
            si.StartupInfo.cb = attrsOK ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = childIn;
            si.StartupInfo.hStdOutput = childOut;
            si.StartupInfo.hStdError = childErr;
            si.lpAttributeList = attrsOK ? attrs : NULL;
            // suspended until it is in the job, so that its children are in the job too
            DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT) | (attrsOK ? EXTENDED_STARTUPINFO_PRESENT : 0);
            created = CreateProcessW(applicationName.c_str(), &cmdBuf[0], NULL, NULL, TRUE, flags, env.empty() ? NULL : &env[0], NULL, &si.StartupInfo, &pi);
            createError = GetLastError();
            if (attrsOK) {
                DeleteProcThreadAttributeList(attrs);
            }
        } else {
            createError = GetLastError();
        }
        closeHandle(&childIn);
        closeHandle(&childOut);
        closeHandle(&childErr);
    }
    closeHandle(&inNul);
    closeHandle(&outNul);
    if (!created) {
        closeHandle(&errRead);
        closeHandle(&job);
        *error = "cannot run " + programName + ": " + systemErrorMessage(createError);

        return eRunFailed;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        // e.g. the host is in a job that does not allow nested jobs (before Windows 8): run without
        closeHandle(&job);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Read stderr while waiting for the process, and poll for abort. After the process exits, read what remains
    // for a short time only: a process that it started (e.g. "start viewer") may keep the pipe open.
    RunResult result = eRunOK;
    RunScriptOutput::LineCollector lines(errLines);
    bool eof = false;
    bool exited = false;
    DWORD exitTick = 0;
    for (;;) {
        bool more = false; // the pipe was not drained: do not wait before reading again
        if (!eof) {
            const ReadStatus rs = readAvailable(errRead, &lines);
            eof = rs == eReadEOF;
            more = rs == eReadMore;
        }
        if (exited && (eof || (GetTickCount() - exitTick) > 200)) {
            break;
        }
        if (!exited && aborted && aborted(abortContext)) {
            if (job) {
                TerminateJobObject(job, 1);
            } else {
                TerminateProcess(pi.hProcess, 1);
            }
            WaitForSingleObject(pi.hProcess, 5000);
            result = eRunAborted;
            *error = "aborted";
            break;
        }
        if (!exited) {
            if (WaitForSingleObject(pi.hProcess, more ? 0 : 20) == WAIT_OBJECT_0) {
                exited = true;
                exitTick = GetTickCount();
            }
        } else if (!more) {
            Sleep(10);
        }
    }
    lines.finish();
    closeHandle(&errRead);

    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code)) {
        *exitCode = code;
    }
    CloseHandle(pi.hProcess);
    if (job) {
        if (result == eRunOK) {
            // let the processes started by the script (e.g. a viewer) run, like on POSIX
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
            ZeroMemory(&info, sizeof(info));
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        closeHandle(&job);
    }

    return result;
} // runProcess

// the file name of a Unix path
static string
unixBaseName(const string& path)
{
    size_t slash = path.find_last_of('/');

    return slash == string::npos ? path : path.substr(slash + 1);
}

static string
trimLeft(const string& s)
{
    size_t i = s.find_first_not_of(" \t");

    return i == string::npos ? string() : s.substr(i);
}

static string
trimRight(const string& s)
{
    size_t i = s.find_last_not_of(" \t\r");

    return i == string::npos ? string() : s.substr(0, i + 1);
}

// split s into the first word (possibly within double quotes) and the rest
static void
splitFirstWord(const string& s,
               string* first,
               string* rest)
{
    const string t = trimLeft(s);
    if (!t.empty() && t[0] == '"') {
        size_t q = t.find('"', 1);
        if (q == string::npos) {
            *first = t.substr(1);
            rest->clear();
        } else {
            *first = t.substr(1, q - 1);
            *rest = trimLeft(t.substr(q + 1));
        }
    } else {
        size_t sp = t.find_first_of(" \t");
        *first = t.substr(0, sp);
        *rest = (sp == string::npos) ? string() : trimLeft(t.substr(sp));
    }
}

// true if program is cmd.exe (in any directory)
static bool
isCmdExe(const wstring& program)
{
    wstring name = toLower(stripTrailingDotsAndSpaces(program));
    size_t sep = name.find_last_of(L"\\/");
    if (sep != wstring::npos) {
        name.erase(0, sep + 1);
    }

    return name == L"cmd.exe" || name == L"cmd.com";
}

// an extension for the script file, which some interpreters require (e.g. PowerShell needs .ps1)
static const wchar_t*
scriptExtension(const wstring& interpreterPath)
{
    wstring name = toLower(interpreterPath);
    size_t sep = name.find_last_of(L"\\/");
    if (sep != wstring::npos) {
        name.erase(0, sep + 1);
    }
    if (endsWith(name, L".exe") || endsWith(name, L".com")) {
        name.erase(name.size() - 4);
    }
    if (name == L"powershell" || name == L"pwsh") {
        return L".ps1";
    }
    if (name.compare(0, 6, L"python") == 0 || name == L"py" || name == L"pyw") {
        return L".py";
    }
    if (name == L"sh" || name == L"bash" || name == L"dash" || name == L"ksh" || name == L"zsh") {
        return L".sh";
    }
    if (name == L"perl") {
        return L".pl";
    }
    if (name == L"ruby") {
        return L".rb";
    }
    if (name == L"node") {
        return L".js";
    }

    return L".txt";
}

// create a new file with a unique name in the temporary directory, and write data to it
static bool
writeTempFile(const wchar_t* extension,
              const string& data,
              wstring* path,
              string* error)
{
    DWORD n = GetTempPathW(0, NULL);
    if (n == 0) {
        *error = "cannot get the temporary directory: " + systemErrorMessage(GetLastError());

        return false;
    }
    vector<wchar_t> dir(n + 1);
    n = GetTempPathW((DWORD)dir.size(), &dir[0]);
    if (n == 0 || n >= dir.size()) {
        *error = "cannot get the temporary directory: " + systemErrorMessage(GetLastError());

        return false;
    }
    wstring tmpdir(&dir[0], n);
    if (!tmpdir.empty() && tmpdir[tmpdir.size() - 1] != L'\\') {
        tmpdir.push_back(L'\\');
    }

    static volatile LONG counter = 0;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 100; ++attempt) {
        wchar_t name[96];
        _snwprintf(name, sizeof(name) / sizeof(name[0]) - 1, L"runscript%lu_%ld_%lu",
                   (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&counter), (unsigned long)GetTickCount());
        name[sizeof(name) / sizeof(name[0]) - 1] = L'\0';
        *path = tmpdir + name + extension;
        // CREATE_NEW fails if the file exists, so we never write to a file created by someone else
        h = CreateFileW(path->c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            break;
        }
        DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) {
            *error = "cannot create " + wideToUtf8(*path) + ": " + systemErrorMessage(err);

            return false;
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        *error = "cannot create a temporary file in " + wideToUtf8(tmpdir);

        return false;
    }
    size_t written = 0;
    while (written < data.size()) {
        DWORD w = 0;
        DWORD chunk = (DWORD)((data.size() - written) > 0x10000000 ? 0x10000000 : (data.size() - written));
        if (!WriteFile(h, data.data() + written, chunk, &w, NULL) || w == 0) {
            *error = "cannot write " + wideToUtf8(*path) + ": " + systemErrorMessage(GetLastError());
            CloseHandle(h);
            DeleteFileW(path->c_str());

            return false;
        }
        written += w;
    }
    CloseHandle(h);

    return true;
} // writeTempFile

RunResult
runScript(const string& scriptContents,
          const vector<string>& args,
          vector<string>* errLines,
          unsigned long* exitCode,
          string* error,
          AbortCallback aborted,
          void* abortContext)
{
    string script = scriptContents;
    // a UTF-8 BOM would hide the shebang, and breaks the first line of a batch file
    if (script.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        script.erase(0, 3);
    }

    wstring interpreter; // full path
    string options;
    string data;
    const wchar_t* extension;
    if (script.compare(0, 2, "#!") == 0) {
        string name;
        string line = trimRight(script.substr(2, script.find('\n') == string::npos ? string::npos : script.find('\n') - 2));
        splitFirstWord(line, &name, &options);
        bool fromUnixPath = false;
        // "#!/usr/bin/env name [options]": search name in the PATH
        if ((name == "env" || (!name.empty() && name[0] == '/' && unixBaseName(name) == "env"))) {
            fromUnixPath = name[0] == '/';
            string rest = options;
            splitFirstWord(rest, &name, &options);
            if (name == "-S") {
                rest = options;
                splitFirstWord(rest, &name, &options);
            }
        }
        // a Unix path ("/bin/sh", "/usr/bin/python3"): search its file name in the PATH
        if (!name.empty() && name[0] == '/') {
            name = unixBaseName(name);
            fromUnixPath = true;
        }
        if (name.empty()) {
            *error = "no interpreter on the first line of the script (\"#!\" must be followed by a program name)";

            return eRunFailed;
        }
        if (!resolveProgram(utf8ToWide(name), fromUnixPath, &interpreter, error)) {
            *error = "interpreter on the first line of the script: " + *error;

            return eRunFailed;
        }
        if (isCmdExe(interpreter)) {
            // "#!cmd" or "#!/usr/bin/env cmd": the rest of the script is a batch file, run as such (by the system's
            // cmd.exe, with the quoting of the arguments for batch files): "cmd.exe script args" would not run it,
            // and "cmd.exe /c script args" would quote the arguments for CommandLineToArgvW, which allows command
            // injection. The options are ignored, and the shebang line is replaced with an empty line, so that the
            // line numbers are unchanged.
            interpreter.clear();
            options.clear();
            const size_t eol = script.find('\n');
            script = (eol == string::npos) ? string() : script.substr(eol);
        }
    }
    if (!interpreter.empty()) {
        extension = scriptExtension(interpreter);
        data = script;
    } else {
        // a batch file: convert the line endings to CRLF (cmd.exe misparses labels and GOTO with LF only)
        extension = L".bat";
        data.reserve(script.size() + script.size() / 16);
        for (size_t i = 0; i < script.size(); ++i) {
            if (script[i] == '\n' && (i == 0 || script[i - 1] != '\r')) {
                data.push_back('\r');
            }
            data.push_back(script[i]);
        }
    }

    wstring scriptPath;
    if (!writeTempFile(extension, data, &scriptPath, error)) {
        return eRunFailed;
    }

    vector<wstring> wargs;
    wstring program;
    if (interpreter.empty()) {
        program = scriptPath;
    } else {
        program = interpreter;
        wargs.push_back(scriptPath);
    }
    for (size_t i = 0; i < args.size(); ++i) {
        wargs.push_back(utf8ToWide(args[i]));
    }

    wstring applicationName;
    wstring commandLine;
    RunResult result = eRunFailed;
    if (buildCommandLine(program, utf8ToWide(options), wargs, &applicationName, &commandLine, error)) {
        result = runProcess(applicationName, commandLine, errLines, exitCode, error, aborted, abortContext);
    }

    // remove the script (this may fail if a process started by the script still has it open)
    DeleteFileW(scriptPath.c_str());

    return result;
} // runScript

} // namespace RunScriptWin32

#endif // defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
