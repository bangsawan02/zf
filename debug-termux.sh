#!/data/data/com.termux/files/usr/bin/bash
# Helper for debugging a running ZygiskFrida deployment from Termux.
#
# Usage:
#   ./debug-termux.sh status      # module + gadget + config overview
#   ./debug-termux.sh logs        # filtered logcat output
#   ./debug-termux.sh follow      # live logcat follow
#   ./debug-termux.sh check       # validate /data/local/tmp/re.zyg.fri/config.json
#   ./debug-termux.sh inject <pkg> # show which processes have the gadget loaded
#   ./debug-termux.sh attach <pkg> # attach lldb to a process (debuggable apps only)

set -uo pipefail

MODULE_DIR="/data/local/tmp/re.zyg.fri"
TAG="ZygiskFrida"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$1"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$1"; }
err()  { printf '\033[1;31m[error]\033[0m %s\n' "$1" >&2; }

need_root() { command -v su >/dev/null || { err "su not found"; exit 1; }; }

cmd_status() {
    say "APatch/KernelSU modules"
    su -c "ls /data/adb/modules" 2>/dev/null || warn "cannot list modules"
    if su -c "test -d /data/adb/modules/zygiskfrida"; then
        echo "  zygiskfrida: installed"
        su -c "ls -l /data/adb/modules/zygiskfrida/zygisk 2>/dev/null" | sed 's/^/    /'
    else
        warn "zygiskfrida not installed (or pending reboot)"
    fi

    say "$MODULE_DIR"
    su -c "ls -l $MODULE_DIR 2>/dev/null" | sed 's/^/  /' || warn "directory missing"

    say "config.json targets"
    su -c "cat $MODULE_DIR/config.json 2>/dev/null" | grep -E 'app_name|enabled|start_up_delay_ms' | sed 's/^/  /' \
        || warn "config.json not found"
}

cmd_logs() {
    su -c "logcat -d -s $TAG" 2>/dev/null | tail -n 200
}

cmd_follow() {
    say "following logcat (Ctrl-C to stop)"
    su -c "logcat -s $TAG" 2>/dev/null
}

cmd_check() {
    local cfg="$MODULE_DIR/config.json"
    if ! su -c "test -f $cfg"; then
        err "$cfg not found"
        return 1
    fi
    su -c "cat $cfg" > /data/data/com.termux/files/usr/tmp/zf-config.json 2>/dev/null
    if python3 -c "import json,sys; json.load(open('/data/data/com.termux/files/usr/tmp/zf-config.json'))" 2>/dev/null; then
        echo "config.json: valid JSON"
    else
        err "config.json: INVALID JSON"
        python3 -c "import json,sys; json.load(open('/data/data/com.termux/files/usr/tmp/zf-config.json'))"
        return 1
    fi
    su -c "cat $cfg" | grep -oE '\"path\"[[:space:]]*:[[:space:]]*\"[^\"]+\"' | sed -E 's/.*\"(.*)\"/\1/' | while read -r p; do
        if su -c "test -f '$p'"; then
            echo "  ok      $p"
        else
            echo "  MISSING $p"
        fi
    done
}

cmd_inject() {
    say "processes with a frida gadget loaded"
    su -c "for m in /proc/[0-9]*/maps; do grep -lE 'libgadget' \$m 2>/dev/null; done" 2>/dev/null | while read -r m; do
        pid="${m#/proc/}"; pid="${pid%/maps}"
        name="$(su -c "cat /proc/$pid/cmdline 2>/dev/null | tr '\\0' ' '")"
        echo "  pid=$pid  $name"
    done
}

cmd_attach() {
    local pkg="${1:-}"
    [ -n "$pkg" ] || { err "usage: $0 attach <package>"; exit 1; }
    local pid
    pid="$(su -c "pidof $pkg" 2>/dev/null | awk '{print $1}')"
    [ -n "$pid" ] || { err "process for $pkg not running"; exit 1; }
    say "attaching lldb to pid $pid ($pkg)"
    su -c "lldb -p $pid" 2>/dev/null \
        || su -c "$PREFIX/bin/lldb -p $pid" 2>/dev/null \
        || err "lldb attach failed (app must be debuggable and ptrace_scope permissive)"
}

case "${1:-status}" in
    status)  cmd_status ;;
    logs)    cmd_logs ;;
    follow)  cmd_follow ;;
    check)   cmd_check ;;
    inject)  cmd_inject ;;
    attach)  shift; cmd_attach "${1:-}" ;;
    *)       sed -n '2,16p' "$0" ;;
esac
