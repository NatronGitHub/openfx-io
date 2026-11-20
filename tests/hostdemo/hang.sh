# Helpers for a hung test program (sourced by run.sh and reader_window.sh, #244).
#
# kill_hung <pid> <stacks file>: writes the native stacks of every thread of the process <pid> to <stacks file>,
# with gdb (MSYS2, Linux) or lldb (macOS), if one is installed, then kills it. On MSYS2, <pid> is the MSYS2 pid of
# the program, or of the shell that exec'd it: the native process is /proc/<pid>/winpid, which kill -9 may leave
# running, so it is also killed with taskkill (with its child processes).
#   HANG_DEBUGGER_TIMEOUT  seconds for the debugger (default 120)

kill_hung() {
    local pid=$1 stacks=$2 target=$1 winpid=""
    if [ -r "/proc/$pid/winpid" ]; then
        winpid=$(cat "/proc/$pid/winpid" 2> /dev/null)
        [ -n "$winpid" ] && target=$winpid
    fi
    local dbg_timeout=${HANG_DEBUGGER_TIMEOUT:-120}
    local to=""
    to=$(command -v timeout || command -v gtimeout)
    {
        echo "---- the stacks of the hung process (pid $pid${winpid:+, Windows pid $winpid})"
        if command -v gdb > /dev/null 2>&1; then
            ${to:+$to -k 10 $dbg_timeout} gdb -nx -batch -p "$target" -ex "set pagination off" -ex "info threads" \
                -ex "thread apply all bt 40" < /dev/null 2>&1
        elif command -v lldb > /dev/null 2>&1; then
            ${to:+$to -k 10 $dbg_timeout} lldb --batch -p "$target" -o "thread backtrace all" < /dev/null 2>&1
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
