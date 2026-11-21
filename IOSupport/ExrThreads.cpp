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
#include "ExrThreads.h"

#include <mutex>
#include <thread>

// without OpenEXR's headers (a build without OpenEXR): a no-op
#if !defined(OFX_IO_NO_ILMTHREAD) && defined(__has_include)
#if !__has_include(<IlmThreadPool.h>)
#define OFX_IO_NO_ILMTHREAD
#endif
#endif
#ifndef OFX_IO_NO_ILMTHREAD
#include <IlmThreadPool.h>
#endif

namespace {
// never destroyed: a host may destroy instances from its own static destructors or atexit handlers
std::mutex&
exrThreadsMutex()
{
    static std::mutex* m = new std::mutex;

    return *m;
}

// Locked by exrThreadsMutex()
int gLiveUsers = 0;
// the number of threads of the pool when the last user stopped it (MS Windows), 0 if it did not
int gThreadsToRestore = 0;
} // namespace

ExrThreadsUser::ExrThreadsUser()
{
    std::lock_guard<std::mutex> lock(exrThreadsMutex());
    if (++gLiveUsers == 1) {
#ifndef OFX_IO_NO_ILMTHREAD
        IlmThread::ThreadPool& pool = IlmThread::ThreadPool::globalThreadPool();
        // unless someone else (e.g. OpenImageIO, with its exr_threads attribute) started it
        if (pool.numThreads() == 0) {
            int n = gThreadsToRestore;
            if (n <= 0) {
                n = (int)std::thread::hardware_concurrency();
            }
            if (n > 0) {
                pool.setNumThreads(n);
            }
        }
        gThreadsToRestore = 0;
#endif
    }
}

// MS Windows: stop the pool (with exrThreadsMutex() locked), remembering its size for the next user
static void
stopPoolLocked()
{
#if !defined(OFX_IO_NO_ILMTHREAD) && (defined(_WIN32) || defined(__WIN32__) || defined(WIN32))
    IlmThread::ThreadPool& pool = IlmThread::ThreadPool::globalThreadPool();
    const int n = pool.numThreads();
    if (n > 0) {
        gThreadsToRestore = n;
        pool.setNumThreads(0);
    }
#endif
}

ExrThreadsUser::~ExrThreadsUser()
{
    std::lock_guard<std::mutex> lock(exrThreadsMutex());
    if (--gLiveUsers == 0) {
        // the last user: stop the pool now, while the host still runs normally. Its threads would otherwise be
        // joined at exit by OpenEXR's static destructors, after ExitProcess terminated them, which may dead-lock with
        // MinGW (#250). The next user starts it again.
        stopPoolLocked();
    }
}

void
stopUnusedExrThreads()
{
    std::lock_guard<std::mutex> lock(exrThreadsMutex());
    if (gLiveUsers == 0) {
        stopPoolLocked();
    }
}

int
liveExrThreadsUsers()
{
    std::lock_guard<std::mutex> lock(exrThreadsMutex());

    return gLiveUsers;
}

int
exrPoolThreads()
{
#ifndef OFX_IO_NO_ILMTHREAD
    return IlmThread::ThreadPool::globalThreadPool().numThreads();
#else
    return -1;
#endif
}
