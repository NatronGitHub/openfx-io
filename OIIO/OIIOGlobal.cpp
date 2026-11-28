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
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <link.h>
#endif
#endif

// clang-format off
GCC_DIAG_OFF(unused-parameter)
#include <OpenImageIO/imageio.h>
GCC_DIAG_ON(unused-parameter)
// clang-format on

// (also checked by CMakeLists.txt; this file is in every build of the OIIO plug-ins)
#if OIIO_VERSION < 20400
#error "The OpenImageIO plug-ins (ReadOIIO, WriteOIIO, OIIOText, OIIOResize) require OpenImageIO 2.4 or later"
#endif

OIIO_NAMESPACE_USING

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

// The number of live instances of the OIIO plug-ins (OIIOThreadsUser objects). Locked by loadedOIIOPluginsMutex().
int gLiveOIIOInstances = 0;
} // namespace

// Set OpenImageIO's threading attributes, which starts its thread pool: for the first live instance, with
// loadedOIIOPluginsMutex() locked. (OpenEXR's pool, which OpenImageIO's OpenEXR reader and writer use, is counted
// separately, by ExrThreadsUser, with ReadEXR and WriteEXR.)
static void
startOIIOThreads()
{
    // See https://github.com/lgritz/oiio/commit/7f7934fafc127a9f3bc51b6aa5e2e77b1b8a26db
    // OpenEXR uses all threads (exr_threads 0), and so do OIIO's image processing functions (threads 0). Both
    // attributes exist in every supported OpenImageIO (2.4 or later).
    attribute("exr_threads", 0);
    attribute("threads", 0);
}

// Stop OpenImageIO's thread pool, with loadedOIIOPluginsMutex() locked: when the
// last live instance of the OIIO plug-ins is destroyed (OIIOThreadsUser), and, as a fallback, when the last OIIO
// plug-in is unloaded (OIIOPluginUnloaded()). Stopping a stopped pool does nothing. The next instance starts the
// pools again (startOIIOThreads()).
static void
shutdownOIIOThreads()
{
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    // (OpenEXR's pool is stopped by the last ExrThreadsUser, and by OIIOPluginUnloaded() if none is alive,
    // ExrThreads.h: not here, while ReadEXR or WriteEXR instances may still use it, #132)

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
        // and OpenEXR's, unless ReadEXR or WriteEXR instances use it (#132): OpenImageIO may have started it outside
        // of any instance
        stopUnusedExrThreads();
    }
}

OIIOThreadsUser::OIIOThreadsUser()
{
    std::lock_guard<std::mutex> lock(loadedOIIOPluginsMutex());
    if (++gLiveOIIOInstances == 1) {
        startOIIOThreads();
    }
}

OIIOThreadsUser::~OIIOThreadsUser()
{
    std::lock_guard<std::mutex> lock(loadedOIIOPluginsMutex());
    if (--gLiveOIIOInstances == 0) {
        // the last live instance: stop the pools now, while the host still runs normally. A host may exit without
        // sending kOfxActionUnload (it never unloads the binary), and OpenImageIO requires its pools to be shut down
        // before exit: "Failing to call `OIIO::shutdown` could lead to a sporadic dead-lock during application
        // shutdown on certain platforms such as Windows" (imageio.h, OpenImageIO 3.2.2) (#250)
        shutdownOIIOThreads();
    }
}

int
liveOIIOInstances()
{
    std::lock_guard<std::mutex> lock(loadedOIIOPluginsMutex());

    return gLiveOIIOInstances;
}

#if !defined(_WIN32) && !defined(__WIN32__) && !defined(WIN32)
namespace {
// Is this the path of an OpenMP runtime (GNU libgomp, LLVM libomp, Intel libiomp5)?
bool
isOpenMPRuntime(const char* path)
{
    if (!path) {
        return false;
    }
    const char* name = std::strrchr(path, '/');
    name = name ? name + 1 : path;

    return !std::strncmp(name, "libgomp.", 8) || !std::strncmp(name, "libomp.", 7) || !std::strncmp(name, "libiomp5.", 9);
}

// keep the library of this path loaded until the process exits (the handle is never closed); true if it is loaded
bool
pinLibrary(const char* path)
{
    return dlopen(path, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) != NULL;
}

#ifndef __APPLE__
// Only collects the paths: dlopen() must not be called from a dl_iterate_phdr() callback. glibc holds
// dl_load_write_lock there, and dlopen() takes dl_load_lock, which a concurrent dlopen() or dlclose() holds while it
// waits for dl_load_write_lock: a lock-order inversion, which dead-locks (#263 review).
int
collectOpenMPRuntimes(struct dl_phdr_info* info, size_t /*size*/, void* data)
{
    if (isOpenMPRuntime(info->dlpi_name)) {
        static_cast<std::vector<std::string>*>(data)->push_back(info->dlpi_name);
    }

    return 0;
}
#endif
} // namespace
#endif

void
pinOpenMPRuntime()
{
    // Every runtime that is loaded is pinned, and only once: the ones pinned so far (a runtime may be loaded after
    // another one was pinned, e.g. libgomp after vcomp140). Called at load and after RAW reads: the cost is fine.
    static std::mutex* m = new std::mutex; // never destroyed
    static std::set<std::string>* pinnedRuntimes = new std::set<std::string>;
    std::lock_guard<std::mutex> lock(*m);
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    // MinGW's libgomp, LLVM's libomp, Intel's libiomp5 and MSVC's vcomp140: GetModuleHandleExW() only finds a module
    // that is already loaded, and pins it (it stays loaded until the process exits)
    static const struct {
        const wchar_t* name;
        const char* nameA; // the key in pinnedRuntimes
    } runtimes[] = {
        { L"libgomp-1.dll", "libgomp-1.dll" },
        { L"libomp.dll", "libomp.dll" },
        { L"libiomp5md.dll", "libiomp5md.dll" },
        { L"vcomp140.dll", "vcomp140.dll" },
    };
    for (const auto& r : runtimes) {
        HMODULE h;
        if (!pinnedRuntimes->count(r.nameA) && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, r.name, &h)) {
            pinnedRuntimes->insert(r.nameA);
        }
    }
#else
    std::vector<std::string> paths;
#if defined(__APPLE__)
    const uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; ++i) {
        const char* path = _dyld_get_image_name(i);
        if (isOpenMPRuntime(path)) {
            paths.push_back(path);
        }
    }
#else
    dl_iterate_phdr(collectOpenMPRuntimes, &paths);
#endif
    // (after dl_iterate_phdr() returned, see collectOpenMPRuntimes())
    for (const std::string& path : paths) {
        if (!pinnedRuntimes->count(path) && pinLibrary(path.c_str())) {
            pinnedRuntimes->insert(path);
        }
    }
#endif
}

std::string
oiioVersionString(int ver)
{
    std::ostringstream oss;
    oss << ver / 10000 << '.' << (ver % 10000) / 100 << '.' << (ver % 100);

    return oss.str();
}

std::string
oiioLibraryInfo()
{
    std::ostringstream oss;
    oss << "OpenImageIO version (compiled with / running with): " << OIIO_VERSION_STRING << '/';
    oss << oiioVersionString(openimageio_version()) << std::endl;
    const std::string libs = OIIO::get_string_attribute("library_list");
    if (!libs.empty()) {
        oss << std::endl
            << "Dependent libraries:" << std::endl;
        std::istringstream f(libs);
        std::string s;
        while (std::getline(f, s, ';')) {
            const size_t pos = s.find(':');
            oss << s.substr(pos + 1) << std::endl;
        }
    }

    // "format:ext1,ext2;format2:ext3" -> "format: ext1, ext2; format2: ext3; "
    std::string extensionsList;
    getattribute("extension_list", extensionsList);
    oss << "\nAll supported formats and extensions: ";
    std::istringstream formatss(extensionsList);
    std::string format;
    while (std::getline(formatss, format, ';')) {
        std::istringstream extensionss(format);
        std::string extension;
        std::getline(extensionss, extension, ':'); // extract the format
        oss << extension << ": ";
        bool first = true;
        while (std::getline(extensionss, extension, ',')) {
            if (!first) {
                oss << ", ";
            }
            first = false;
            oss << extension;
        }
        oss << "; ";
    }

    return oss.str();
}
