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
 * IOMutex: the mutex type of the IO plug-ins (GenericOCIO::Mutex), in its own header so that OCIOProcessorCache.h and
 * GenericOCIO.h can both use it.
 */

#ifndef IO_IOMutex_h
#define IO_IOMutex_h

#include "ofxsMultiThread.h"
#ifndef OFX_USE_MULTITHREAD_MUTEX
// some OFX hosts do not have mutex handling in the MT-Suite (e.g. Sony Catalyst Edit)
// prefer using the fast mutex by Marcus Geelnard http://tinythreadpp.bitsnbites.eu/
#include "fast_mutex.h"
#endif

#include "IOUtility.h" // NAMESPACE_OFX_IO_ENTER

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

#ifdef OFX_USE_MULTITHREAD_MUTEX
typedef OFX::MultiThread::Mutex IOMutex;
typedef OFX::MultiThread::AutoMutex IOAutoMutex;
#else
typedef tthread::fast_mutex IOMutex;
typedef OFX::MultiThread::AutoMutexT<tthread::fast_mutex> IOAutoMutex;
#endif

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT

#endif // IO_IOMutex_h
