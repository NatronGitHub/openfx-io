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

#ifndef IO_GLOBAL_OIIO_H
#define IO_GLOBAL_OIIO_H

#include <string>

#include "ExrThreads.h"

// A member of each instance of the OIIO plug-ins (ReadOIIO, WriteOIIO, OIIOText, OIIOResize), which share
// OpenImageIO's thread pool. The live instances are counted: the first one sets OpenImageIO's threading attributes,
// which starts the pool, and the destruction of the last one stops it (OIIO::shutdown()), so that a host that exits
// without kOfxActionUnload (the spec allows it if it never unloads the binary) doesn't leave its threads to
// OpenImageIO's static destructors, which may dead-lock at exit on MS Windows (#250). The next instance starts it
// again. Thread-safe. It also holds an ExrThreadsUser: OpenImageIO's OpenEXR reader and writer use OpenEXR's pool,
// which is counted with ReadEXR's and WriteEXR's instances (ExrThreads.h).
class OIIOThreadsUser
{
public:
    OIIOThreadsUser();
    ~OIIOThreadsUser();

private:
    ExrThreadsUser _exrThreads; // (constructed before, and destroyed after, the body of the constructor and destructor)

    OIIOThreadsUser(const OIIOThreadsUser&); // not copyable
    OIIOThreadsUser& operator=(const OIIOThreadsUser&);
};

// The number of live OIIOThreadsUser objects (for the tests)
int liveOIIOInstances();

// The OIIO plug-ins of this binary (ReadOIIO, WriteOIIO, OIIOText, OIIOResize) share OpenImageIO's thread pool
// (and, on MS Windows, OpenEXR's IlmThread pool), which must be stopped before the binary is unloaded. Each
// plug-in calls OIIOPluginLoaded() at the end of its load action, with its unique identifier (getUID(): one per
// plug-in version), and OIIOPluginUnloaded() in its unload action: the pools are also stopped by the unload of the
// last loaded OIIO plug-in (not by the first one, while the others may still use them, #118), in case an instance
// is still alive then.
// Thread-safe. A plug-in loaded twice is counted once, and the unload of a plug-in that is not loaded (e.g. after
// its load failed) does not stop the pools while other OIIO plug-ins are loaded.
void OIIOPluginLoaded(const std::string& pluginUID);
void OIIOPluginUnloaded(const std::string& pluginUID);

// Keep the OpenMP runtime loaded until the process exits, if one is loaded (#239). LibRaw, which OpenImageIO uses for
// RAW files, uses OpenMP (Linux: libgomp; MS Windows, MSYS2: libgomp-1.dll; not MacPorts' LibRaw), and its idle
// worker threads outlive the render: if unloading IO.ofx (dlclose(), FreeLibrary()) also unloaded the OpenMP
// runtime, they would run unmapped code. Only the runtime is pinned (as openfx-gmic pins vcomp140.dll): IO.ofx itself
// still unloads. Called by ReadOIIO's load action, when the runtime is usually already loaded as a dependency of
// IO.ofx (through OpenImageIO and LibRaw), and after each decode: an OpenImageIO built with non-embedded plug-ins
// dlopen()s its raw reader, and with it LibRaw and OpenMP, on the first RAW read. Thread-safe. Once a runtime was
// pinned it does nothing more (an atomic check); until then it tries again, which is cheap. Nothing is done if no
// OpenMP runtime is loaded (e.g. MacPorts' LibRaw).
void pinOpenMPRuntime();

#endif /* IO_GLOBAL_OIIO_H*/
