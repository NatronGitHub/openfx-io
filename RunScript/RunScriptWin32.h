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

#ifndef IO_RunScriptWin32_h
#define IO_RunScriptWin32_h

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)

#include <string>
#include <vector>

namespace RunScriptWin32 {

// UTF-8 <-> UTF-16 (OFX strings are UTF-8, the Win32 "W" API is UTF-16)
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);

// Quote one argument so that CommandLineToArgvW() (and the MSVC/UCRT runtime) gives it back unchanged.
// It is always quoted, so that MSYS2/Cygwin programs do not expand wildcards (*.exr, a[1].png) in it.
std::wstring quoteArg(const std::wstring& arg);

// Quote one argument passed to a batch file through "cmd.exe /c" (same escaping as Rust's std::process
// after CVE-2024-24576): always quoted, '"' doubled, '%' neutralized, backslashes doubled before a '"'.
// Returns false if the argument cannot be passed to cmd.exe (it contains CR, LF or NUL).
bool quoteBatchArg(const std::wstring& arg, std::wstring* quoted);

// Windows ignores trailing dots and spaces in file names ("x.bat. " opens "x.bat")
std::wstring stripTrailingDotsAndSpaces(const std::wstring& path);

// true if the program is a batch file (.bat or .cmd, case-insensitive, ignoring trailing dots and spaces),
// which must be run by cmd.exe (CVE-2024-43402)
bool isBatchFile(const std::wstring& program);

// Find the full path of a program:
// - an absolute path (C:\dir\prog, \\server\share\prog) is used as is, ".exe" or ".com" being appended if
//   it has no executable extension;
// - a relative path with a directory (dir\prog) is rejected;
// - a file name is searched for in the directories of %PATH% only (not in the host's directory or the current
//   directory, unlike CreateProcess), with ".exe" or ".com" appended if it has no executable extension.
//   If fromUnixPath (it comes from "#!/bin/bash" or "#!/usr/bin/env python3"), the system directory is skipped,
//   so that "bash" is not WSL's System32\bash.exe.
// - cmd and powershell (with or without .exe, Unix-style or not) are always taken from the system directory, never
//   from %PATH%: they have no Unix counterpart, and a program of the same name earlier in %PATH% cannot replace them.
// Returns false and sets *error if it is not found.
bool resolveProgram(const std::wstring& name, bool fromUnixPath, std::wstring* fullPath, std::string* error);

// Build the CreateProcessW() application name and command line for running program (a full path, see
// resolveProgram()) with args. Batch files are run with "cmd.exe /e:ON /v:OFF /d /c", other programs must be
// .exe or .com. extraArgs is inserted verbatim after the program (used for the options on a shebang line).
// Returns false and sets *error if an argument cannot be passed.
bool buildCommandLine(const std::wstring& program,
                      const std::wstring& extraArgs,
                      const std::vector<std::wstring>& args,
                      std::wstring* applicationName,
                      std::wstring* commandLine,
                      std::string* error);

// polled while the process runs: return true to kill it
typedef bool (*AbortCallback)(void* context);

enum RunResult {
    eRunOK = 0, // the process ran (its exit code may be non-zero)
    eRunFailed, // it could not be started (*error is set)
    eRunAborted // it was killed because the abort callback returned true
};

// Run a command line built by buildCommandLine(), in a job object, without a console window, with stdin from NUL,
// stdout to the host's stdout if it is a file or a pipe (else NUL), and stderr captured line by line into
// *errLines (like the POSIX version, which reads the script's stderr with pstream), split by
// RunScriptOutput::LineCollector (a lone '\r' starts the line again; long lines are cut, and only the last lines are
// kept). Waits for the process to exit, polling aborted(abortContext) if aborted is not NULL (at least every 64 KB
// of stderr). Processes that it started
// and which are still running are left running (e.g. a viewer started with "start"), unless it is aborted.
RunResult runProcess(const std::wstring& applicationName,
                     const std::wstring& commandLine,
                     std::vector<std::string>* errLines,
                     unsigned long* exitCode,
                     std::string* error,
                     AbortCallback aborted = NULL,
                     void* abortContext = NULL);

// Write the script contents to a new temporary file and run it with the given UTF-8 arguments:
// - if the script starts with a shebang line ("#!interpreter [options]"), it is saved with an extension
//   matching the interpreter and run as "interpreter [options] script args...". A Unix path
//   ("#!/usr/bin/python3", "#!/bin/sh") is reduced to its file name, and "#!/usr/bin/env name" to "name";
//   the interpreter is found with resolveProgram(). If it is cmd.exe ("#!cmd", "#!/usr/bin/env cmd"), the rest of
//   the script is run as a batch file (see below), and the options are ignored. If it is powershell.exe or
//   pwsh.exe, "-File" is appended to the options unless they already end with it (Windows PowerShell would
//   otherwise run the script path and the arguments as a command), and -Command or -EncodedCommand is an error
//   (with any dash that PowerShell accepts: '-', '/', "--", en dash, em dash, horizontal bar). With -File, Windows
//   PowerShell 5.1 may bind an argument that begins with '-' to a parameter of the script.
//   wsl.exe is rejected (it passes the command line to the Linux shell). The first line may end with LF, CRLF or CR.
// - otherwise, it is a batch file, saved as a .bat file (with CRLF line endings) and run by cmd.exe.
// The file is written in a new directory in the temporary directory, which only the current user (and SYSTEM) can
// access, so that another user cannot replace it before it runs; both are removed afterwards.
RunResult runScript(const std::string& script,
                    const std::vector<std::string>& args,
                    std::vector<std::string>* errLines,
                    unsigned long* exitCode,
                    std::string* error,
                    AbortCallback aborted = NULL,
                    void* abortContext = NULL);

} // namespace RunScriptWin32

#endif // defined(_WIN32) || defined(__WIN32__) || defined(WIN32)

#endif // IO_RunScriptWin32_h
