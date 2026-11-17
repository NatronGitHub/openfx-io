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
#include "OIIOGlobal.h"

#include "ofxsMacros.h"
#include <iostream>
#include <mutex>
#include <set>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <IlmThreadPool.h>
#endif

// clang-format off
GCC_DIAG_OFF(unused-parameter)
#include <OpenImageIO/imagecache.h>
#include <OpenImageIO/imageio.h>
GCC_DIAG_ON(unused-parameter)
// clang-format on

OIIO_NAMESPACE_USING

void
initOIIOThreads()
{
    // See https://github.com/lgritz/oiio/commit/7f7934fafc127a9f3bc51b6aa5e2e77b1b8a26db
    // We want OpenEXR to use all threads, while we do not want OIIO to use all threads for its image
    // processing functionalities w/o letting the host know about it.
    if (!attribute("exr_threads", 0)) {
        // This version of OIIO does not have the exr_threads attribute, fallback on the "threads" attribute...
        if (!attribute("threads", 0)) {
#ifdef DEBUG
            std::cerr << "OIIO: Failed to set exr_threads and threads attribute" << std::endl;
#endif
        }
#ifdef DEBUG
        else {
            std::cout << "Failed to set exr_threads to 0 fallback to OIIO threads=0" << std::endl;
        }
#endif
    } else {
        // This version of OIIO has the exr_threads attribute. Set the "threads" attribute to limit image processing functions
        static const int oiio_threads = 0;
#ifdef DEBUG
        std::cout << "Successfully set exr_threads to 0, setting OIIO threads=" << oiio_threads << std::endl;
#endif
        if (!attribute("threads", oiio_threads)) {
#ifdef DEBUG
            std::cerr << "Failed to set the threads attribute for OIIO" << std::endl;
#endif
        }
    }
}

// Stop OpenImageIO's thread pool (and, on MS Windows, OpenEXR's): only called by OIIOPluginUnloaded(), once the
// last OIIO plug-in is unloaded.
static void
shutdownOIIOThreads()
{
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    // Kill all threads otherwise when the static global thread pool joins it threads there is a deadlock on Mingw
    IlmThread::ThreadPool::globalThreadPool().setNumThreads(0);

    // TODO: Remove the code in the following #if block once all platforms have been updated to an OpenImageIO
    // version that defines default_thread_pool_shutdown().
#if NATRON_OIIO_HAS_DEFAULT_THREAD_POOL_SHUTDOWN != 1 && OIIO_VERSION < OIIO_MAKE_VERSION(2,5,3)
    // Workaround to a bug: https://github.com/OpenImageIO/oiio/issues/1795
    // see also https://github.com/LuxCoreRender/LuxCore/commit/607bfc9bff519ecc32c02ff3203b7ec71d201fde
    OIIO::attribute("threads", 1);
#endif
#endif

// TODO: Remove #if once all platforms have been updated to an OpenImageIO version that defines
// default_thread_pool_shutdown().
#if NATRON_OIIO_HAS_DEFAULT_THREAD_POOL_SHUTDOWN == 1 || OIIO_VERSION >= OIIO_MAKE_VERSION(2,5,3)
    default_thread_pool_shutdown();
#endif
}

namespace {
// The unique identifiers of the loaded OIIO plug-ins. Function-local statics: constructed on first use, so that a
// plug-in loaded during static initialization finds them, and a set rather than a counter, so that the count can
// neither go negative (an unload without load) nor leak (a plug-in loaded twice).
std::mutex&
loadedOIIOPluginsMutex()
{
    // never destroyed: a host may unload the plug-ins from its own static destructors or atexit handlers
    static std::mutex* m = new std::mutex;
    return *m;
}

std::set<std::string>&
loadedOIIOPlugins()
{
    static std::set<std::string>* s = new std::set<std::string>;
    return *s;
}
} // namespace

void
OIIOPluginLoaded(const std::string& pluginUID)
{
    std::lock_guard<std::mutex> lock(loadedOIIOPluginsMutex());
    loadedOIIOPlugins().insert(pluginUID);
}

void
OIIOPluginUnloaded(const std::string& pluginUID)
{
    std::lock_guard<std::mutex> lock(loadedOIIOPluginsMutex());
    std::set<std::string>& loaded = loadedOIIOPlugins();
    loaded.erase(pluginUID);
    if (loaded.empty()) {
        // the last OIIO plug-in of this binary is unloaded (kOfxActionUnload: "the last action passed to the plug-in
        // before the binary containing the plug-in is unloaded", after "all instances of a plugin have been destroyed"):
        // stop the pools now, their threads would deadlock if they were joined while the binary is unloaded (MS Windows)
        shutdownOIIOThreads();
    }
}
