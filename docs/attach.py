#!/data/data/com.termux/files/usr/bin/bash
# attach.py - attach to a Frida Gadget listener on this device and run a script
#
# Usage:
#   attach.py <port> [script.js]
#   attach.py 27043                 # interactive REPL on the gadget
#   attach.py 27043 myhook.js       # run a script file
#
# Ports are per target app (see /data/local/tmp/re.zyg.fri/<app>/*.config.so):
#   27042 -> bank (id.co.cimbniaga.mobile.android)
#   27043 -> chrome (com.android.chrome)
#   27044 -> taxsee (com.taxsee.driver)

set -euo pipefail

PORT="${1:-27043}"
SCRIPT="${2:-}"
ADDR="127.0.0.1:${PORT}"

if ! ss -ltn 2>/dev/null | grep -q "127.0.0.1:${PORT}"; then
    echo "[!] No gadget listening on ${ADDR}" >&2
    echo "    Launch the matching app first, then re-run." >&2
    exit 1
fi

echo "[*] Connecting to gadget at ${ADDR}..."
exec python3 - "$ADDR" "$SCRIPT" <<'PY'
import sys, frida

addr, script_path = sys.argv[1], sys.argv[2]
device = frida.get_device_manager().add_remote_device(addr)

procs = device.enumerate_processes()
if not procs:
    print("[!] gadget has no target process")
    sys.exit(1)

target = procs[0]
print(f"[+] attached to pid {target.pid} ({target.name}) on {addr}")

session = device.attach(target.pid)

if script_path:
    with open(script_path) as fh:
        code = fh.read()
    script = session.create_script(code)
    script.on("message", lambda m, d: print("[JS]", m.get("payload", m)))
    script.load()
    print("[+] script loaded, press Ctrl+C to detach")
    try:
        import time
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        script.unload()
        session.detach()
else:
    from frida_tools.repl import REPL
    REPL(session).start()
PY
