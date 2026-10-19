#!/data/data/com.termux/files/usr/bin/python3
"""probe.py - attach to a Frida Gadget listener and keep a script alive.

Useful for debugging hooks on a target app: the script stays loaded and every
message it sends is printed live, until Ctrl+C.

Usage:
    probe.py <port> [script.js]
    probe.py 27044                      # built-in SSL_write probe
    probe.py 27044 myhook.js            # your own script
    probe.py 27044 --scan SSL_write     # just locate a symbol

The gadget must be in "listen" mode. With "on_load": "wait" the app is
suspended until a client connects, so the traffic only starts flowing once
this script is attached.
"""
import os
import sys
import time

import frida


def builtin_probe():
    return """
var m = Process.findModuleByName('libssl.so');
if (!m) {
  console.log('libssl.so not loaded');
} else {
  var addr = m.findExportByName('SSL_write');
  console.log('SSL_write @ ' + addr);
  Interceptor.attach(addr, {
    onEnter(args) {
      var len = args[2].toInt32();
      var preview = '';
      try {
        var buf = new Uint8Array(args[1].readByteArray(Math.min(len, 160)));
        preview = String.fromCharCode.apply(null, buf);
      } catch (e) {}
      console.log('[SSL_write] len=' + len + ' data=' + JSON.stringify(preview));
    }
  });
  console.log('interceptor installed - waiting for traffic');
}
"""


def scan(symbol):
    return """
var needle = %s;
var out = [];
Process.enumerateModules().forEach(function (m) {
  try {
    m.enumerateExports().forEach(function (e) {
      if (e.name.indexOf(needle) >= 0) {
        out.push(m.name + ' :: ' + e.name + ' @ ' + e.address);
      }
    });
  } catch (e) {}
});
console.log(out.length ? out.join('\\n') : 'not found');
""" % repr(symbol)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    port = sys.argv[1]
    addr = "127.0.0.1:%s" % port

    if len(sys.argv) > 2 and sys.argv[2] == "--scan":
        source = scan(sys.argv[3] if len(sys.argv) > 3 else "SSL_write")
    elif len(sys.argv) > 2:
        with open(sys.argv[2]) as fh:
            source = fh.read()
    else:
        source = builtin_probe()

    device = frida.get_device_manager().add_remote_device(addr)

    # With "on_load": "wait" the gadget is suspended, so enumerate_processes()
    # may never answer. Attach by name instead.
    target = None
    try:
        procs = device.enumerate_processes()
        if procs:
            target = procs[0]
    except Exception:
        pass

    if target is None:
        print("[+] no process list (gadget suspended) - attaching to 'Gadget'")
        try:
            session = device.attach("Gadget")
            print("[+] attached to %s on %s" % (addr, addr))
        except Exception as exc:
            print("[!] attach failed: %s" % exc)
            return 1
    else:
        print("[+] attaching to %s (pid %d) on %s" % (target.name, target.pid, addr), flush=True)
        session = device.attach(target.pid)

    try:
        script = session.create_script(source)
    except Exception as exc:
        print("[!] create_script failed: %s" % exc)
        print("    the gadget may have closed the connection - restart the target app")
        return 1

    def on_message(message, data):
        if message.get("type") == "send":
            print("[JS]", message.get("payload"), flush=True)
        elif message.get("type") == "error":
            print("[!] script error:", message.get("description"))
            for line in (message.get("stack") or "").splitlines():
                print("    ", line)
        else:
            print("[JS]", message.get("payload") or message, flush=True)

    script.on("message", on_message)
    script.load()
    print("[+] script loaded - Ctrl+C to detach", flush=True)
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n[*] detaching")
        script.unload()
        session.detach()
    return 0


if __name__ == "__main__":
    sys.exit(main())
