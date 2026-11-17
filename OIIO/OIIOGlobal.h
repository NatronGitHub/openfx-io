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

// Set OpenImageIO's threading attributes (and start its thread pool): called by the OIIO plug-ins' instances.
void initOIIOThreads();

// The OIIO plug-ins of this binary (ReadOIIO, WriteOIIO, OIIOText, OIIOResize) share OpenImageIO's thread pool
// (and, on MS Windows, OpenEXR's IlmThread pool), which must be stopped before the binary is unloaded. Each
// plug-in calls OIIOPluginLoaded() at the end of its load action, with its unique identifier (getUID(): one per
// plug-in version), and OIIOPluginUnloaded() in its unload action: the pools are stopped by the unload of the
// last loaded OIIO plug-in, not by the first one, while the others may still use them (#118).
// Thread-safe. A plug-in loaded twice is counted once, and the unload of a plug-in that is not loaded (e.g. after
// its load failed) does not stop the pools while other OIIO plug-ins are loaded.
void OIIOPluginLoaded(const std::string& pluginUID);
void OIIOPluginUnloaded(const std::string& pluginUID);

#endif /* IO_GLOBAL_OIIO_H*/
