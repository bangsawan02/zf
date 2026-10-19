#!/data/data/com.termux/files/usr/bin/python3
"""jbrun.py - run a TypeScript/JS script with the Frida Java bridge.

Frida 17 removed the built-in `Java` global: the bridge now lives in the
`frida-java-bridge` npm package and must be bundled. The bundled result is
~235 KB, but the Frida transport refuses payloads above ~128 KB
("the connection is closed").

Workaround: compile with frida.Compiler, push the bundle to a file the target
can read, then send a tiny loader that reads the file and evals it.

Usage:
    jbrun.py script.ts [port]
    jbrun.py script.ts 27044

    # compile only
    jbrun.py --compile-only script.ts

The remote file is written next to the gadget payload, e.g.
    /data/local/tmp/re.zyg.fri/taxsee/_frida_script.js
"""
import os
import subprocess
import sys
import time

import frida

REMOTE_DIR = "/data/local/tmp/re.zyg.fri/taxsee"
REMOTE_FILE = REMOTE_DIR + "/_frida_script.js"

LOADER = """
(function () {
  var path = %s;
  try {
    var f = new File(path, 'r');
    var code = f.readText();
    f.close();
    console.log('[jbrun] loaded ' + code.length + ' bytes from ' + path);
    (0, eval)(code);
  } catch (e) {
    console.log('[jbrun] ERROR: ' + (e && e.message ? e.message : e));
  }
})();
"""


def compile_ts(src, out):
    compiler = frida.Compiler()
    # The frida-java-bridge dependency must be resolvable from project_root.
    # Look for node_modules next to the script, then in $HOME/jswork, then in
    # the global npm root.
    candidates = [
        os.path.dirname(os.path.abspath(src)),
        os.path.join(os.path.expanduser("~"), "jswork"),
        os.path.join(os.path.expanduser("~"), "node_modules"),
    ]
    try:
        global_root = subprocess.check_output(
            ["npm", "root", "-g"], text=True).strip()
        candidates.append(os.path.dirname(global_root))
    except Exception:
        pass

    project_root = None
    for cand in candidates:
        if cand and os.path.isdir(os.path.join(cand, "node_modules", "frida-java-bridge")):
            project_root = cand
            break
    if project_root is None:
        project_root = candidates[0]
        print("[jbrun] warning: frida-java-bridge not found under %s" % candidates)

    result = compiler.build(
        src,
        compression="terser",
        source_maps="omitted",
        bundle_format="iife",
        type_check="none",
        project_root=project_root,
    )
    with open(out, "w") as fh:
        fh.write(result)
    return len(result)


def push_to_device(local_path, remote_path):
    # /data/local/tmp/re.zyg.fri is owned by shell, so Termux cannot write
    # there directly - go through su.
    subprocess.run(
        ["su", "-c", "cat '%s' > '%s'" % (local_path, remote_path)],
        check=True,
    )
    subprocess.run(["su", "-c", "chmod 666 '%s'" % remote_path], check=True)


def main():
    args = sys.argv[1:]
    compile_only = False
    if args and args[0] == "--compile-only":
        compile_only = True
        args = args[1:]
    if not args:
        print(__doc__)
        return 2

    src = args[0]
    port = args[1] if len(args) > 1 else "27044"
    addr = "127.0.0.1:%s" % port

    local_out = os.path.join(
        os.path.dirname(os.path.abspath(src)), os.path.basename(src) + ".compiled.js"
    )
    size = compile_ts(src, local_out)
    print("[jbrun] compiled %s -> %s (%d bytes)" % (src, local_out, size))
    if compile_only:
        return 0

    push_to_device(local_out, REMOTE_FILE)
    print("[jbrun] pushed to %s" % REMOTE_FILE)

    device = frida.get_device_manager().add_remote_device(addr)
    try:
        session = device.attach("Gadget")
    except Exception as exc:
        print("[!] attach failed: %s" % exc)
        return 1
    print("[+] attached to %s" % addr)

    script = session.create_script(LOADER % repr(REMOTE_FILE))

    def on_message(message, data):
        if message.get("type") == "error":
            print("[!] script error:", message.get("description"))
            for line in (message.get("stack") or "").splitlines():
                print("    ", line)
        else:
            print("[JS]", message.get("payload") or message, flush=True)

    script.on("message", on_message)
    script.load()
    print("[+] running - Ctrl+C to detach")
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        script.unload()
        session.detach()
    return 0


if __name__ == "__main__":
    sys.exit(main())
