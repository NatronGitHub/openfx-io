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
std::wstring quoteArg(const std::wstring& arg);

// Quote one argument passed to a batch file through "cmd.exe /c" (same escaping as Rust's std::process
// after CVE-2024-24576): always quoted, '"' doubled, '%' neutralized, backslashes doubled before a '"'.
// Returns false if the argument cannot be passed to cmd.exe (it contains CR, LF or NUL).
bool quoteBatchArg(const std::wstring& arg, std::wstring* quoted);

// true if the program is a batch file (.bat or .cmd, case-insensitive), which must be run by cmd.exe
bool isBatchFile(const std::wstring& program);

// Build the CreateProcessW() application name (may be empty: search PATH) and command line for running
// program with args. Batch files are run with "cmd.exe /e:ON /v:OFF /d /c". extraArgs is inserted verbatim
// after the program (used for the options on a shebang line). Returns false and sets *error if an argument
// cannot be passed.
bool buildCommandLine(const std::wstring& program,
                      const std::wstring& extraArgs,
                      const std::vector<std::wstring>& args,
                      std::wstring* applicationName,
                      std::wstring* commandLine,
                      std::string* error);

// Run a command line built by buildCommandLine(), without a console window, with stdin from NUL, stdout to the
// host's stdout (or NUL if there is none), and stderr captured line by line into *errLines (like the POSIX
// version, which reads the script's stderr with pstream). Waits for the process to exit.
// Returns false and sets *error (UTF-8) if the process could not be started.
bool runProcess(const std::wstring& applicationName,
                const std::wstring& commandLine,
                std::vector<std::string>* errLines,
                unsigned long* exitCode,
                std::string* error);

// Write the script contents to a new temporary file and run it with the given UTF-8 arguments:
// - if the script starts with a shebang line ("#!interpreter [options]"), it is saved with an extension
//   matching the interpreter and run as "interpreter [options] script args...". A Unix path
//   ("#!/usr/bin/python3", "#!/bin/sh") is reduced to its file name, and "#!/usr/bin/env name" to "name",
//   which are searched for in the PATH (like the Python launcher for Windows does).
// - otherwise, it is a batch file, saved as a .bat file (with CRLF line endings) and run by cmd.exe.
// The temporary file is removed afterwards.
// Returns false and sets *error (UTF-8) if the script could not be written or started. A non-zero exit
// code is not an error (the POSIX version ignores it too).
bool runScript(const std::string& script,
               const std::vector<std::string>& args,
               std::vector<std::string>* errLines,
               unsigned long* exitCode,
               std::string* error);

} // namespace RunScriptWin32

#endif // defined(_WIN32) || defined(__WIN32__) || defined(WIN32)

#endif // IO_RunScriptWin32_h
