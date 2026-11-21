# Helpers for a hung test program (sourced by run.sh and reader_window.sh, #244).
#
# kill_hung <pid> <stacks file>: writes the native stacks of every thread of the process <pid> to <stacks file>,
# with gdb (MSYS2, Linux) or lldb (macOS), if one is installed, then kills it. On MSYS2, <pid> is the MSYS2 pid of
# the program, or of the shell that exec'd it: the native process is /proc/<pid>/winpid, which kill -9 may leave
# running, so it is also killed with taskkill (with its child processes).
# On Linux, with yama's ptrace_scope >= 1 (GitHub's Ubuntu runners), a non-root gdb can't attach to a process that
# is not its child: gdb then runs with `sudo -n` if passwordless sudo is available, else the stacks are skipped,
# with a note. The debugger is bounded by a watchdog (a background sleep, as stock macOS has no timeout(1)), and
# the process is killed in every case.
#   HANG_DEBUGGER_TIMEOUT  seconds for the debugger (default 120)

# _hang_bounded <seconds> <command...>: runs the command, killed after <seconds> (a background sleep watchdog)
_hang_bounded() {
    local secs=$1
    shift
    "$@" &
    local cmd=$!
    (
        sleep "$secs"
        kill -9 $cmd 2> /dev/null && echo "(the debugger was killed after ${secs}s)"
    ) &
    local watchdog=$!
    wait $cmd 2> /dev/null
    kill $watchdog 2> /dev/null
    wait $watchdog 2> /dev/null
}

kill_hung() {
    local pid=$1 stacks=$2 target=$1 winpid=""
    if [ -r "/proc/$pid/winpid" ]; then
        winpid=$(cat "/proc/$pid/winpid" 2> /dev/null)
        [ -n "$winpid" ] && target=$winpid
    fi
    local secs=${HANG_DEBUGGER_TIMEOUT:-120}
    {
        echo "---- the stacks of the hung process (pid $pid${winpid:+, Windows pid $winpid})"
        if command -v gdb > /dev/null 2>&1; then
            local sudo=""
            # Linux: a non-root gdb can only attach to its children with yama's ptrace_scope >= 1
            if [ -z "$winpid" ] && [ "$(id -u)" != 0 ] && [ -r /proc/sys/kernel/yama/ptrace_scope ] \
                && [ "$(cat /proc/sys/kernel/yama/ptrace_scope)" -ge 1 ]; then
                if command -v sudo > /dev/null 2>&1 && sudo -n true 2> /dev/null; then
                    sudo="sudo -n"
                else
                    echo "(no stacks: ptrace_scope is $(cat /proc/sys/kernel/yama/ptrace_scope), and no passwordless sudo for gdb)"
                    sudo="none"
                fi
            fi
            if [ "$sudo" != none ]; then
                # shellcheck disable=SC2086
                _hang_bounded "$secs" $sudo gdb -nx -batch -p "$target" -ex "set pagination off" -ex "info threads" \
                    -ex "thread apply all bt 40" < /dev/null
            fi
        elif command -v lldb > /dev/null 2>&1; then
            _hang_bounded "$secs" lldb --batch -p "$target" -o "thread backtrace all" < /dev/null
        else
            echo "(no gdb or lldb: no stacks)"
        fi
        echo "---- end of the stacks"
    } > "$stacks" 2>&1
    kill -9 "$pid" 2> /dev/null
    if [ -n "$winpid" ] && command -v taskkill > /dev/null 2>&1; then
        taskkill //F //T //PID "$winpid" > /dev/null 2>&1
    fi
}
