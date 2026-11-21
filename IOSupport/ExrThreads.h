/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
 * (C) 2018-2023 The Natron Developers
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

#ifndef IO_EXR_THREADS_H
#define IO_EXR_THREADS_H

// A member of each instance of the plug-ins that use OpenEXR's global IlmThread pool: ReadEXR and WriteEXR, and,
// through OpenImageIO's OpenEXR reader and writer, the OIIO plug-ins (held by their OIIOThreadsUser, OIIOGlobal.h).
// The live users are counted (#250, #132):
// - the first one starts the pool if it has no thread (with the number of threads it had when it was last stopped,
//   else std::thread::hardware_concurrency()), on every platform: ReadEXR and WriteEXR never size it themselves;
// - on MS Windows, the destruction of the last one stops it (setNumThreads(0)), so that its threads are not left to
//   OpenEXR's static destructors, which may dead-lock at exit (a host may exit without kOfxActionUnload). Elsewhere
//   it keeps its threads, as before.
// Thread-safe. A no-op if built without OpenEXR (OFX_IO_NO_ILMTHREAD).
class ExrThreadsUser
{
public:
    ExrThreadsUser();
    ~ExrThreadsUser();

private:
    ExrThreadsUser(const ExrThreadsUser&); // not copyable
    ExrThreadsUser& operator=(const ExrThreadsUser&);
};

// The number of live ExrThreadsUser objects, and of threads of OpenEXR's global pool (-1 without OpenEXR), for the tests
int liveExrThreadsUsers();
int exrPoolThreads();

#endif /* IO_EXR_THREADS_H */
