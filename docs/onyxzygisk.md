# OnyxZygisk on Termux: ptrace-based Zygisk runtime, zero-reboot workflow

Source: `OnyxZygisk/OnyxZygisk` (v1.09-399, AGPL-3.0).
Pure-ptrace Zygisk implementation (no kernel module), root-agnostic, works with
APatch / KernelSU / Magisk. Unlike Zygisk Next it can **hot-plug** modules
without a reboot.

Verified on this device: Android 13, arm64-v8a, APatch 115357, kernel 4.14.

## Why switch from Zygisk Next

Zygisk Next (Dr-TSNG) loads modules only at zygote startup. A staged module in
`/data/adb/modules_update/` is **not** activated by `apd soft-reboot`; the only
reliable activation is a real reboot.

OnyxZygisk is ptrace-based: the monitor attaches to the zygote and injects
`libzygisk.so` itself, so the runtime can be (re)started at runtime.

## Install

The release zip has a nested layout and APatch's installer does **not** extract
it correctly (the module dir ends up containing only `module.prop`). Extract by
hand and lay the files out the way `customize.sh` does:

```sh
VER=1.09
curl -L -o onyxzygisk.zip \
  "https://github.com/OnyxZygisk/OnyxZygisk/releases/download/v$VER/OnyxZygisk-v$VER-399-820b68b-release.zip"

python3 -c "
import zipfile, os
z = zipfile.ZipFile('onyxzygisk.zip')
d = '/data/adb/modules/onyxzygisk'
for i in z.infolist():
    if i.filename.startswith('META-INF/') or i.filename.endswith('.sha256'):
        continue
    z.extract(i, d)
"
```

Required final layout (per `customize.sh`):

| Source | Destination |
|---|---|
| `bin/arm64-v8a/zygiskd` | `bin/zygiskd64` |
| `bin/armeabi-v7a/zygiskd` | `bin/zygiskd32` |
| `lib/arm64-v8a/libzygisk.so` | `lib64/libzygisk.so` |
| `lib/arm64-v8a/libzygisk_ptrace.so` | `bin/zygisk-ptrace64` |
| `lib/armeabi-v7a/libzygisk.so` | `lib/libzygisk.so` |
| `lib/armeabi-v7a/libzygisk_ptrace.so` | `bin/zygisk-ptrace32` |

`bin/*` → mode 0755, `lib*/` → 0644 with `u:object_r:system_lib_file:s0`.

## Start the runtime without rebooting

`post-fs-data.sh` sources `zygisk-init.sh`, which needs `MODDIR` to be exported
and **the current working directory to be the module dir** — the monitor opens
`./module.prop` with a *relative* path and exits with code 1 otherwise.

```sh
su -c "cd /data/adb/modules/onyxzygisk && \
  TMP_PATH=/data/adb/onyxzygisk \
  nohup ./bin/zygisk-ptrace64 monitor >/data/adb/onyxzygisk/monitor.log 2>&1 &"
```

The monitor finds a pre-existing zygote, kills it once, and init respawns a
fresh one that it attaches to. Verify with:

```sh
su -c "cat /data/adb/onyxzygisk/module.prop"
```

Healthy output:

```
	monitor: 	tracing
	zygote64:	injected
	daemon64:	running
		Root: APatch
		Modules (2):
			zygisk_vector
			zygiskfrida
```

`daemon64: crashed(0x9 ...)` means the daemon was SIGKILLed — restart it with
`TMP_PATH=/data/adb/onyxzygisk ./bin/zygiskd64 daemon &`.

### Debugging the monitor exit code 1

No output, just exit 1. `strace` reveals the last syscalls:

```sh
su -c "cd /data/adb/modules/onyxzygisk && \
  /system/bin/strace -f -o /data/adb/onyxzygisk/trace.log ./bin/zygisk-ptrace64 monitor"
```

```
openat(AT_FDCWD, "./module.prop", O_RDONLY) = -1 ENOENT (No such file or directory)
write(1, "starting monitor mode...\n", 25) = 25
exit_group(1)
```

→ run it from the module directory. `strace` lives at `/system/bin/strace`;
Termux has no `strace` package.

## Gadget ports

OnyxZygisk loads the ZygiskFrida module normally, so the gadget listener comes
from the per-app config in `/data/local/tmp/re.zyg.fri/<app>/*.config.so`:

| App | Port |
|---|---|
| `id.co.cimbniaga.mobile.android` | 27042 |
| `com.android.chrome` | 27043 |
| `com.taxsee.driver` | 27044 |

## Attaching

```sh
ss -ltnp | grep 2704          # which gadget is live
~/attach.py 27043             # interactive REPL
~/attach.py 27043 script.js   # run a script file
```

Raw Python:

```python
import frida
d = frida.get_device_manager().add_remote_device("127.0.0.1:27043")
pid = d.enumerate_processes()[0].pid
s = d.attach(pid)
sc = s.create_script("rpc.exports={p:function(){return Process.id;}};")
sc.load()
print(sc.exports_sync.p())
```

A gadget in `listen` mode reports itself as a single process named `Gadget`;
that process **is** the target app.

## Notes / gotchas

- `zygiskd` (Zygisk Next) and `zygiskd64` (OnyxZygisk) have confusingly similar
  names — check `ps -A | grep zygisk` before killing anything, and never kill
  OnyxZygisk's `zygiskd64` by mistake.
- Only one Zygisk implementation may be active. Disable the other one
  (`touch /data/adb/modules/<id>/disable`).
- `/data/adb/onyxzygisk/boot_fail_count` increments once per boot. At 3 the
  daemon latches `injection_disabled` and stops injecting; delete that file to
  recover.
- The `hotplug` subcommand (`zygiskd hotplug <module-id> --workdir <path>`)
  swaps a staged module from `/data/adb/modules_update` into the active dir and
  takes effect on the next app launch, without a reboot.
