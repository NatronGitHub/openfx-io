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

#include <cstdio> // snprintf, _snwprintf
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
    if (!arg.empty() && (arg.find_first_of(L" \t\n\v\"") == wstring::npos)) {
        return arg;
    }
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

bool
isBatchFile(const wstring& program)
{
    const wstring l = toLower(program);

    return endsWith(l, L".bat") || endsWith(l, L".cmd");
}

bool
buildCommandLine(const wstring& program,
                 const wstring& extraArgs,
                 const vector<wstring>& args,
                 wstring* applicationName,
                 wstring* commandLine,
                 string* error)
{
    if (program.empty() || program.find(L'"') != wstring::npos) {
        *error = "invalid program name \"" + wideToUtf8(program) + "\"";

        return false;
    }
    if (isBatchFile(program)) {
        // CreateProcess cannot run a batch file: run it with cmd.exe /c, from the system directory
        // (not from the PATH). cmd.exe removes the first and the last quote after /c, and parses the rest
        // as a command line.
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
    } else {
        // No application name: CreateProcess searches the program in the PATH, and appends .exe if needed.
        // The program name is quoted without escapes (CommandLineToArgvW does not process backslashes in it).
        applicationName->clear();
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

static void
addLines(string* pending,
         const char* data,
         size_t n,
         vector<string>* lines)
{
    pending->append(data, n);
    size_t pos;
    while ((pos = pending->find('\n')) != string::npos) {
        string line = pending->substr(0, pos);
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        lines->push_back(line);
        pending->erase(0, pos + 1);
    }
}

bool
runProcess(const wstring& applicationName,
           const wstring& commandLine,
           vector<string>* errLines,
           unsigned long* exitCode,
           string* error)
{
    *exitCode = 0;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE errRead = NULL;
    HANDLE errWrite = NULL;
    if (!CreatePipe(&errRead, &errWrite, &sa, 0)) {
        *error = "cannot create a pipe: " + systemErrorMessage(GetLastError());

        return false;
    }
    // our end of the pipe must not be inherited, or the read would never see the end of file
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    // stdin: NUL, so that a script waiting for input (e.g. "pause") does not block the render forever
    HANDLE inNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    // stdout: the host's stdout (like on POSIX) if it is redirected to a file or a pipe, else NUL: a GUI host
    // usually has none, and a console handle would not be valid in the child, which runs without a console window
    HANDLE out = NULL;
    HANDLE parentOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD parentOutType = (parentOut == NULL || parentOut == INVALID_HANDLE_VALUE) ? FILE_TYPE_UNKNOWN : GetFileType(parentOut);
    if ((parentOutType != FILE_TYPE_DISK && parentOutType != FILE_TYPE_PIPE)
        || !DuplicateHandle(GetCurrentProcess(), parentOut, GetCurrentProcess(), &out, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    }
    if (inNul == INVALID_HANDLE_VALUE || out == NULL || out == INVALID_HANDLE_VALUE) {
        *error = "cannot open NUL: " + systemErrorMessage(GetLastError());
        closeHandle(&errRead);
        closeHandle(&errWrite);
        closeHandle(&inNul);
        closeHandle(&out);

        return false;
    }

    // Only let the child inherit these three handles (and not, e.g., the pipe of a RunScript running
    // concurrently in another thread, which would keep that pipe open until our child exits).
    HANDLE inherited[3] = { inNul, out, errWrite };
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
    si.StartupInfo.hStdInput = inNul;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = errWrite;
    si.lpAttributeList = attrsOK ? attrs : NULL;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end()); // CreateProcessW may modify it
    cmdBuf.push_back(L'\0');
    DWORD flags = CREATE_NO_WINDOW | (attrsOK ? EXTENDED_STARTUPINFO_PRESENT : 0);
    BOOL created = CreateProcessW(applicationName.empty() ? NULL : applicationName.c_str(),
                                  &cmdBuf[0], NULL, NULL, TRUE, flags, NULL, NULL, &si.StartupInfo, &pi);
    DWORD createError = GetLastError();
    if (attrsOK) {
        DeleteProcThreadAttributeList(attrs);
    }
    // the child has its own copies now
    closeHandle(&errWrite);
    closeHandle(&inNul);
    closeHandle(&out);
    if (!created) {
        closeHandle(&errRead);
        *error = "cannot run " + wideToUtf8(commandLine) + ": " + systemErrorMessage(createError);

        return false;
    }
    CloseHandle(pi.hThread);

    // read stderr until the end of file (all the writers have exited or closed it)
    string pending;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(errRead, buf, sizeof(buf), &n, NULL) && n > 0) {
        addLines(&pending, buf, n, errLines);
    }
    if (!pending.empty()) {
        if (pending[pending.size() - 1] == '\r') {
            pending.erase(pending.size() - 1);
        }
        errLines->push_back(pending);
    }
    closeHandle(&errRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code)) {
        *exitCode = code;
    }
    CloseHandle(pi.hProcess);

    return true;
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

// an extension for the script file, which some interpreters require (e.g. PowerShell needs .ps1)
static const wchar_t*
scriptExtension(const string& interpreter)
{
    wstring name = toLower(utf8ToWide(interpreter));
    size_t sep = name.find_last_of(L"\\/");
    if (sep != wstring::npos) {
        name.erase(0, sep + 1);
    }
    if (endsWith(name, L".exe")) {
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

bool
runScript(const string& scriptContents,
          const vector<string>& args,
          vector<string>* errLines,
          unsigned long* exitCode,
          string* error)
{
    string script = scriptContents;
    // a UTF-8 BOM would hide the shebang, and breaks the first line of a batch file
    if (script.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        script.erase(0, 3);
    }

    string interpreter;
    string options;
    string data;
    const wchar_t* extension;
    if (script.compare(0, 2, "#!") == 0) {
        string line = trimRight(script.substr(2, script.find('\n') == string::npos ? string::npos : script.find('\n') - 2));
        splitFirstWord(line, &interpreter, &options);
        // "#!/usr/bin/env name [options]": search name in the PATH
        if ((interpreter == "env" || (!interpreter.empty() && interpreter[0] == '/' && unixBaseName(interpreter) == "env"))) {
            string rest = options;
            splitFirstWord(rest, &interpreter, &options);
            if (interpreter == "-S") {
                rest = options;
                splitFirstWord(rest, &interpreter, &options);
            }
        }
        // a Unix path ("/bin/sh", "/usr/bin/python3"): search its file name in the PATH
        if (!interpreter.empty() && interpreter[0] == '/') {
            interpreter = unixBaseName(interpreter);
        }
        if (interpreter.empty()) {
            *error = "no interpreter on the first line of the script (\"#!\" must be followed by a program name)";

            return false;
        }
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
        return false;
    }

    vector<wstring> wargs;
    wstring program;
    if (interpreter.empty()) {
        program = scriptPath;
    } else {
        program = utf8ToWide(interpreter);
        wargs.push_back(scriptPath);
    }
    for (size_t i = 0; i < args.size(); ++i) {
        wargs.push_back(utf8ToWide(args[i]));
    }

    wstring applicationName;
    wstring commandLine;
    bool ok = buildCommandLine(program, utf8ToWide(options), wargs, &applicationName, &commandLine, error)
        && runProcess(applicationName, commandLine, errLines, exitCode, error);

    // remove the script (this may fail if a process started by the script still has it open)
    DeleteFileW(scriptPath.c_str());

    return ok;
} // runScript

} // namespace RunScriptWin32

#endif // defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
