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
 * OFX RunScript plugin: running the script on POSIX systems, with posix_spawn() (the MS Windows version is
 * RunScriptWin32.cpp).
 */

#ifndef IO_RunScriptPosix_h
#define IO_RunScriptPosix_h

#if !(defined(_WIN32) || defined(__WIN32__) || defined(WIN32))

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string.h> // strerror_r (POSIX, not in std)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h> // _NSGetEnviron(): a bundle has no direct access to environ
#else
extern char** environ;
#endif

#include "RunScriptOutput.h"

namespace RunScriptPosix {

// strerror_r() returns an int (POSIX/XSI: macOS, musl) and writes the message in buf, also "Unknown error: N" when it
// fails with EINVAL on macOS; or it returns the message, which may not be in buf (GNU: glibc with _GNU_SOURCE, which
// g++ and clang++ define on Linux): one overload for each. buf is zero-initialised, so a libc that writes nothing
// leaves it empty.
inline const char*
strerrorResult(int /*ret*/,
               const char* buf)
{
    return buf;
}

inline const char*
strerrorResult(const char* ret,
               const char* /*buf*/)
{
    return ret;
}

// The message of the errno value err. Thread-safe, unlike std::strerror() ("The strerror function is not required to
// avoid data races with other calls to the strerror function", C11 7.24.6.2), and renders of RunScript may run
// concurrently (C++ Core Guidelines CP.2, https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#Rconc-races).
inline std::string
errorString(int err)
{
    char buf[256] = {};
    const char* s = strerrorResult(strerror_r(err, buf, sizeof(buf)), buf);

    return (s && *s) ? std::string(s) : ("error " + std::to_string(err));
}

// A file descriptor, closed by the destructor
class UniqueFd
{
public:
    explicit UniqueFd(int fd = -1)
        : _fd(fd)
    {
    }
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const { return _fd; }
    // close the file descriptor, and own fd instead
    void reset(int fd = -1)
    {
        if (_fd >= 0) {
            (void)::close(_fd);
        }
        _fd = fd;
    }
    // give up the ownership of the file descriptor
    int release()
    {
        const int fd = _fd;
        _fd = -1;

        return fd;
    }

private:
    int _fd;
};

enum RunResult {
    eRunOK, // the script closed its standard error and exited
    eRunAborted // aborted() returned true: the script was killed
};

// A script, run by posix_spawn() in a new process group, with its standard input from /dev/null, its standard output
// the host's, and its standard error read through a pipe. The pipe is close-on-exec, so that the scripts of
// concurrent renders do not inherit it (on macOS, no other file descriptor of the host is inherited either:
// POSIX_SPAWN_CLOEXEC_DEFAULT). posix_spawn() runs no code of ours in the child (no allocation between fork and exec),
// and it returns the error of exec() (e.g. ENOENT for a missing interpreter on the shebang line). A script without a
// "#!" line is run by /bin/sh, as execvp() does.
// The destructor kills the script and the processes of its group (SIGKILL) if it was not reaped yet (after an abort
// or an exception), then reaps it.
class Process
{
public:
    Process(const std::string& file,
            const std::vector<std::string>& argv)
        : _pid(0)
        , _running(false)
        , _status(-1)
        , _error(0)
    {
        // everything that posix_spawn() needs is built before it
        static const char kShell[] = "/bin/sh";
        std::vector<char*> args;
        args.reserve(argv.size() + 2);
        args.push_back(const_cast<char*>(kShell)); // (only for a script without "#!": see below)
        for (const std::string& a : argv) {
            args.push_back(const_cast<char*>(a.c_str())); // posix_spawn() does not modify them
        }
        args.push_back(NULL);
#ifdef __APPLE__
        char** const env = *_NSGetEnviron();
#else
        char** const env = environ;
#endif
        SpawnFileActions actions;
        SpawnAttributes attributes;
        if (!actions.ok() || !attributes.ok()) {
            _error = ENOMEM;

            return;
        }

        // The pipe is close-on-exec from its creation on Linux (pipe2()), and only the standard streams are inherited
        // on macOS (POSIX_SPAWN_CLOEXEC_DEFAULT), so that a script spawned at the same time by another thread (a
        // concurrent render) does not inherit it: it would hold the write end open, and this script's stderr would
        // never reach EOF. (On other POSIX systems, FD_CLOEXEC is set just after pipe(): a spawn between the two can
        // inherit it.) Both ends are moved to file descriptors >= 3 if the host closed its standard streams, so that
        // the write end is never already the script's stderr (adddup2() onto itself would not clear FD_CLOEXEC).
        int fds[2];
#ifdef __linux__
        const int p = ::pipe2(fds, O_CLOEXEC);
        const bool closeOnExec = true;
#else
        const int p = ::pipe(fds);
        const bool closeOnExec = false;
#endif
        if (p != 0) {
            _error = errno;

            return;
        }
        UniqueFd readEnd(fds[0]);
        UniqueFd writeEnd(fds[1]);
        if (!closeOnExecAbove2(&readEnd, closeOnExec) || !closeOnExecAbove2(&writeEnd, closeOnExec)) {
            _error = errno;

            return;
        }
        int r = posix_spawn_file_actions_addopen(&actions.a, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (r == 0) {
            r = posix_spawn_file_actions_adddup2(&actions.a, writeEnd.get(), STDERR_FILENO); // (not close-on-exec)
        }
        short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
        flags |= POSIX_SPAWN_CLOEXEC_DEFAULT; // only the standard streams are inherited
        // (unless the host closed its stdout: addinherit_np() of a closed fd makes posix_spawn() fail with EBADF)
        if (r == 0 && ::fcntl(STDOUT_FILENO, F_GETFD) != -1) {
            r = posix_spawn_file_actions_addinherit_np(&actions.a, STDOUT_FILENO);
        }
#endif
        // the script gets an empty signal mask, and the default action for SIGPIPE (which hosts often ignore)
        sigset_t noSignals;
        sigset_t sigpipe;
        sigemptyset(&noSignals);
        sigemptyset(&sigpipe);
        sigaddset(&sigpipe, SIGPIPE);
        if (r == 0) {
            r = posix_spawnattr_setpgroup(&attributes.a, 0); // a new group, whose ID is the PID of the script
        }
        if (r == 0) {
            r = posix_spawnattr_setsigmask(&attributes.a, &noSignals);
        }
        if (r == 0) {
            r = posix_spawnattr_setsigdefault(&attributes.a, &sigpipe);
        }
        if (r == 0) {
            r = posix_spawnattr_setflags(&attributes.a, flags);
        }
        pid_t pid = 0;
        if (r == 0) {
            r = posix_spawn(&pid, file.c_str(), &actions.a, &attributes.a, args.data() + 1, env);
        }
        if (r == ENOEXEC) {
            // not an executable format, e.g. no "#!" line: run it with /bin/sh, as execvp() does (POSIX: "the shell
            // is invoked with ... the first argument being the file")
            r = posix_spawn(&pid, kShell, &actions.a, &attributes.a, args.data(), env);
        }
        if (r != 0) {
            _error = r;

            return;
        }
        _pid = pid;
        _running = true;
        _stderr.reset(readEnd.release());
    } // writeEnd is closed here: the script then has the only write end of the pipe

    ~Process()
    {
        killGroup();
        _stderr.reset();
        while (_running && !reap(false)) {
            // EINTR
        }
    }

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // false if the script could not be started: see error()
    bool started() const { return _pid > 0; }
    // the error of posix_spawn() (that of exec(), or of fork()) or of pipe(), if the script could not be started
    int error() const { return _error; }
    // the status of the script (as given by waitpid()) after run() returned eRunOK, or -1
    int status() const { return _status; }

    // Read the standard error of the script into lines until its end, then wait for the script to exit (as before
    // #287, the render also waits for a process started in the background that keeps the standard error open).
    // aborted(context) is polled at least every kPollMs ms: if it returns true, the script and the processes of its
    // group are killed, and eRunAborted is returned.
    RunResult run(RunScriptOutput::LineCollector& lines,
                  RunScriptOutput::AbortCallback aborted,
                  void* context)
    {
        const int fd = _stderr.get();
        bool eof = fd < 0;
        char buf[16 * 1024];
        while (!eof) {
            if (aborted && aborted(context)) {
                killGroup();

                return eRunAborted;
            }
            pollfd pfd { fd, POLLIN, 0 };
            const int r = ::poll(&pfd, 1, kPollMs);
            if (r == 0 || (r < 0 && errno == EINTR)) {
                continue; // nothing to read yet
            }
            if (r < 0) {
                break;
            }
            // readable, or the other end was closed (POLLHUP): read() does not block
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                lines.add(buf, static_cast<std::size_t>(n));
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                eof = true;
            }
        }
        lines.finish();
        // the script closed its standard error (e.g. "exec 2>&-"), but may still be running
        while (_running && !reap(true)) {
            if (aborted && aborted(context)) {
                killGroup();

                return eRunAborted;
            }
            (void)::poll(NULL, 0, kPollMs);
        }

        return eRunOK;
    }

private:
    static constexpr int kPollMs = 20;

    struct SpawnFileActions {
        SpawnFileActions() { _ok = posix_spawn_file_actions_init(&a) == 0; }
        ~SpawnFileActions()
        {
            if (_ok) {
                (void)posix_spawn_file_actions_destroy(&a);
            }
        }
        SpawnFileActions(const SpawnFileActions&) = delete;
        SpawnFileActions& operator=(const SpawnFileActions&) = delete;
        bool ok() const { return _ok; }
        posix_spawn_file_actions_t a;

    private:
        bool _ok;
    };

    struct SpawnAttributes {
        SpawnAttributes() { _ok = posix_spawnattr_init(&a) == 0; }
        ~SpawnAttributes()
        {
            if (_ok) {
                (void)posix_spawnattr_destroy(&a);
            }
        }
        SpawnAttributes(const SpawnAttributes&) = delete;
        SpawnAttributes& operator=(const SpawnAttributes&) = delete;
        bool ok() const { return _ok; }
        posix_spawnattr_t a;

    private:
        bool _ok;
    };

    // make *fd close-on-exec (unless it already is: alreadyCloseOnExec), and >= 3 (not a standard stream)
    static bool closeOnExecAbove2(UniqueFd* fd,
                                  bool alreadyCloseOnExec)
    {
        if (fd->get() > STDERR_FILENO) {
            return alreadyCloseOnExec || ::fcntl(fd->get(), F_SETFD, FD_CLOEXEC) == 0;
        }
        const int moved = ::fcntl(fd->get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (moved < 0) {
            return false;
        }
        fd->reset(moved);

        return true;
    }

    // waitpid(): true if the script was reaped, or cannot be waited for (ECHILD if the host ignores SIGCHLD: the
    // system reaped it, and status() stays -1); false if nohang and it is still running, or on EINTR
    bool reap(bool nohang)
    {
        int st = 0;
        const pid_t r = ::waitpid(_pid, &st, nohang ? WNOHANG : 0);
        if (r == 0 || (r < 0 && errno == EINTR)) {
            return false;
        }
        if (r == _pid) {
            _status = st;
        }
        _running = false;

        return true;
    }

    // Kill the script and the processes of its group, unless it was reaped.
    // Not ::killpg(::getpgid(_pid)): getpgid() fails on a zombie on macOS, so a background child that keeps stderr
    // open after the script exited would survive.
    void killGroup()
    {
        if (_running) {
            // _running is true until waitpid() reaps the script: until then, even if it exited (a zombie), its PID,
            // which is also the ID of its process group, cannot be reused by another process. Unless the host reaps
            // children itself (SIGCHLD ignored, SA_NOCLDWAIT, or waitpid(-1) on another thread): then a PID reused
            // in the meantime could be signalled. That risk is accepted: a pidfd (pidfd_spawn(), glibc 2.39) is not
            // available on EL9, nor on macOS.
            (void)::killpg(_pid, SIGKILL);
            (void)::kill(_pid, SIGKILL); // (if the script left its group: setpgid(), setsid())
        }
    }

    pid_t _pid; // the PID of the script, and the ID of its process group (0 if it could not be started)
    bool _running; // the script was started and not reaped yet: _pid is still its PID
    int _status; // its status, from waitpid()
    int _error; // the error of posix_spawn() or pipe()
    UniqueFd _stderr; // the read end of the pipe of its standard error
};

} // namespace RunScriptPosix

#endif // !_WIN32

#endif // IO_RunScriptPosix_h
