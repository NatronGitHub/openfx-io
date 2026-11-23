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
 * OCIOProcessorCache: the last OpenColorIO processor of a plug-in instance, shared by its render threads.
 */

#ifndef IO_OCIOProcessorCache_h
#define IO_OCIOProcessorCache_h

#ifdef OFX_IO_USING_OCIO

#include <string>
#include <type_traits>
#include <utility>

#include <OpenColorIO/OpenColorIO.h>

#include "GenericOCIO.h" // GenericOCIO::Mutex, GenericOCIO::AutoMutex

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

// The identity of the OCIO config and context that a processor is built with, for the key of an OCIOProcessorCache.
// The config is compared by pointer and held, so that its address can't be reused by another config while the key
// exists. (A const config never changes: loading another config file, or OCIO::SetCurrentConfig(), makes a new one.)
// The context is compared by its cache ID, a hash of its search path, working directory and string variables
// (OCIO Context::getCacheID()): GenericOCIO::getLocalContext() makes a new context object at each call when a context
// variable is set.
struct OCIOConfigContextKey {
    OCIO_NAMESPACE::ConstConfigRcPtr config;
    std::string contextCacheID; // empty: the current context of config

    OCIOConfigContextKey() = default;
    explicit OCIOConfigContextKey(OCIO_NAMESPACE::ConstConfigRcPtr cfg,
                                  const OCIO_NAMESPACE::ConstContextRcPtr& context = OCIO_NAMESPACE::ConstContextRcPtr())
        : config(std::move(cfg))
        , contextCacheID(context ? context->getCacheID() : "")
    {
    }

    friend bool operator==(const OCIOConfigContextKey& a, const OCIOConfigContextKey& b)
    {
        return a.config == b.config && a.contextCacheID == b.contextCacheID;
    }
};

// One cached processor and the key that it was built for. Key must be default-constructible, copy-constructible,
// nothrow move-assignable (checked by get()) and equality-comparable, and must contain everything that the processor
// depends on and that the plug-in can see: the config and context (an OCIOConfigContextKey) and the values of every
// param used to build it. (Not the contents of the files that the transforms read: OCIO caches them.)
// The render actions of an instance run concurrently (eRenderFullySafe, OFX ofxRendering.rst: "any instance of a plugin
// can have multiple renders running simultaneously"), so get() returns a copy of the processor taken under the lock
// (C++ Core Guidelines CP.2, CP.31): the caller owns a reference, and another thread may replace the cached one
// meanwhile. An OCIO processor is safe to use from several threads once built: its const methods
// (getOptimizedCPUProcessor(), getDefaultGPUProcessor(), ...) guard their caches with a mutex (OpenColorIO 2.6
// src/OpenColorIO/Processor.cpp).
template <class Key>
class OCIOProcessorCache {
public:
    // The processor for key: the cached one if it was built for key, else build(), which is then cached.
    // build() runs without the lock (CP.22: no unknown code under a lock): it may take any lock and take long (OCIO:
    // "Constructing a Processor object is likely a blocking operation"), and two threads that miss at the same time
    // both build (OCIO's own processor cache makes the second one cheap). If build() throws, the cache is unchanged and
    // the exception propagates.
    template <class Build>
    OCIO_NAMESPACE::ConstProcessorRcPtr get(const Key& key,
                                            Build&& build)
    {
        // (checked here, where Key is complete: it is often a nested class of the plug-in)
        static_assert(std::is_default_constructible<Key>::value, "Key must be default-constructible");
        static_assert(std::is_copy_constructible<Key>::value, "Key must be copy-constructible");
        static_assert(std::is_nothrow_move_assignable<Key>::value,
                      "Key must be nothrow move-assignable (exception safety)");
        static_assert(std::is_convertible<decltype(build()), OCIO_NAMESPACE::ConstProcessorRcPtr>::value,
                      "build() must return an OCIO processor");
        {
            GenericOCIO::AutoMutex guard(_mutex);
            if (_proc && (_key == key)) {
                return _proc;
            }
        }
        OCIO_NAMESPACE::ConstProcessorRcPtr proc = build();
        if (proc) {
            Key k(key); // (copied before the lock: the moves under it don't throw, so _proc and _key stay a pair)
            GenericOCIO::AutoMutex guard(_mutex);
            _proc = proc;
            _key = std::move(k);
        }

        return proc;
    }

private:
    GenericOCIO::Mutex _mutex; // guards _proc and _key; never held while calling out (no lock order)
    OCIO_NAMESPACE::ConstProcessorRcPtr _proc; // the processor for _key (null: none yet)
    Key _key;
};

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT

#endif // OFX_IO_USING_OCIO

#endif // IO_OCIOProcessorCache_h
