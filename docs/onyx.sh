#!/data/data/com.termux/files/usr/bin/bash
# onyx.sh - control the OnyxZygisk runtime on this device (no reboot needed)
#
# Usage:
#   ./onyx.sh start      # start the monitor + daemon, attach to the zygote
#   ./onyx.sh status     # show runtime state and loaded modules
#   ./onyx.sh restart    # stop then start again
#   ./onyx.sh stop       # stop the runtime
#   ./onyx.sh install    # extract the release zip into the module dir (first time)
#
# OnyxZygisk is a ptrace-based Zygisk runtime. It can be (re)started at
# runtime, which is what makes staged module updates take effect without a
# reboot. Zygisk Next cannot do that: it only loads modules at zygote start.
#
# Docs: docs/ONYXZYGISK.md

set -euo pipefail

MODDIR="/data/adb/modules/onyxzygisk"
WORKDIR="/data/adb/onyxzygisk"
DAEMON="$MODDIR/bin/zygiskd64"
MONITOR="$MODDIR/bin/zygisk-ptrace64"
CACHE="${TMPDIR:-/data/data/com.termux/files/usr/tmp}/opencode"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$1"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$1"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$1" >&2; exit 1; }

as_root() { su -c "$*"; }

# ---------------------------------------------------------------------------
# install: lay out the release zip the way customize.sh does
# ---------------------------------------------------------------------------
do_install() {
    local ver="${ONYX_VERSION:-1.09}"
    local zip="$CACHE/onyxzygisk.zip"

    command -v curl >/dev/null || die "curl not found"
    mkdir -p "$CACHE"

    if [ ! -f "$zip" ]; then
        say "downloading OnyxZygisk v$ver"
        local url
        url="$(curl -sL "https://api.github.com/repos/OnyxZygisk/OnyxZygisk/releases/latest" \
              | python3 -c "
import sys, json
d = json.load(sys.stdin)
for a in d['assets']:
    if a['name'].endswith('.zip'):
        print(a['browser_download_url']); break
")"
        [ -n "$url" ] || die "could not resolve the release asset URL"
        curl -L -o "$zip" "$url"
    fi

    say "extracting to a temp dir"
    rm -rf "$CACHE/onyx_extract"
    python3 -c "
import zipfile, os
z = zipfile.ZipFile('$zip')
d = '$CACHE/onyx_extract'
os.makedirs(d, exist_ok=True)
for i in z.infolist():
    if i.filename.startswith('META-INF/') or i.filename.endswith('.sha256'):
        continue
    z.extract(i, d)
"

    say "installing into $MODDIR"
    as_root "cp -a '$CACHE/onyx_extract/.' '$MODDIR/'"
    as_root "mkdir -p '$MODDIR/bin' '$MODDIR/lib' '$MODDIR/lib64'"
    # customize.sh renames the arm64 payloads; the zip keeps them in arch dirs.
    as_root "cp '$MODDIR/bin/arm64-v8a/zygiskd'            '$MODDIR/bin/zygiskd64'"
    as_root "cp '$MODDIR/bin/armeabi-v7a/zygiskd'          '$MODDIR/bin/zygiskd32'"
    as_root "cp '$MODDIR/lib/arm64-v8a/libzygisk.so'        '$MODDIR/lib64/libzygisk.so'"
    as_root "cp '$MODDIR/lib/arm64-v8a/libzygisk_ptrace.so' '$MODDIR/bin/zygisk-ptrace64'"
    as_root "cp '$MODDIR/lib/armeabi-v7a/libzygisk.so'      '$MODDIR/lib/libzygisk.so'"
    as_root "cp '$MODDIR/lib/armeabi-v7a/libzygisk_ptrace.so' '$MODDIR/bin/zygisk-ptrace32'"
    as_root "chmod 755 '$MODDIR'/bin/*"
    as_root "chmod 644 '$MODDIR'/lib64/*.so '$MODDIR'/lib/*.so"
    as_root "chcon u:object_r:system_lib_file:s0 '$MODDIR/lib64/libzygisk.so' 2>/dev/null || true"

    say "installed"
}

# ---------------------------------------------------------------------------
# start
# ---------------------------------------------------------------------------
do_start() {
    [ -x "$MONITOR" ] || die "monitor missing - run '$0 install' first"

    # A leftover failure latch stops the daemon from injecting anything.
    if as_root "test -f '$WORKDIR/injection_disabled'"; then
        warn "clearing the boot-loop fail-safe latch"
        as_root "rm -f '$WORKDIR/injection_disabled' '$WORKDIR/boot_fail_count' '$WORKDIR/boot_fail_boot_id'"
    fi

    say "starting the monitor"
    # The monitor opens ./module.prop with a RELATIVE path, so the working
    # directory must be the module dir. Without this it exits with code 1.
    as_root "cd '$MODDIR' && TMP_PATH='$WORKDIR' nohup '$MONITOR' monitor \
             >'$WORKDIR/monitor.log' 2>&1 &"

    sleep 4

    # The monitor only attaches to a zygote that was born AFTER it started.
    local zy
    zy="$(as_root "pidof zygote64" | awk '{print $1}')"
    if [ -n "$zy" ]; then
        say "restarting the zygote (pid $zy) so the monitor can attach"
        as_root "kill -9 '$zy'"
        sleep 8
    fi

    say "runtime state:"
    as_root "cat '$WORKDIR/module.prop' 2>/dev/null" || warn "no module.prop yet"
}

# ---------------------------------------------------------------------------
# stop
# ---------------------------------------------------------------------------
do_stop() {
    say "stopping the runtime"
    as_root "pkill -9 -f zygisk-ptrace64 2>/dev/null || true"
    as_root "pkill -9 -f zygiskd64 2>/dev/null || true"
    as_root "rm -f '$WORKDIR'/*.sock '$WORKDIR/init_monitor'"
    sleep 1
    as_root "ps -A | grep -i zygisk || echo '  no zygisk processes left'"
}

# ---------------------------------------------------------------------------
# status
# ---------------------------------------------------------------------------
do_status() {
    echo "=== processes ==="
    as_root "ps -A | grep -i zygisk || echo '  none'"
    echo
    echo "=== runtime state ($WORKDIR/module.prop) ==="
    as_root "cat '$WORKDIR/module.prop' 2>/dev/null || echo '  not present'"
    echo
    echo "=== gadget listeners ==="
    as_root "ss -ltnp 2>/dev/null | grep 2704 || echo '  no gadget listening'"
    echo
    echo "=== modules dir ==="
    as_root "ls -1 /data/adb/modules/"
}

case "${1:-status}" in
    start)   do_start ;;
    stop)    do_stop ;;
    restart) do_stop; sleep 2; do_start ;;
    status)  do_status ;;
    install) do_install ;;
    -h|--help) sed -n '2,22p' "$0" ;;
    *) die "unknown command: $1 (start|stop|restart|status|install)" ;;
esac
