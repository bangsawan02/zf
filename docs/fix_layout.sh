#!/system/bin/sh
SRC=/data/data/com.termux/files/usr/tmp/opencode/onyx_extract
DST=/data/adb/modules/onyxzygisk
mkdir -p "$DST/bin" "$DST/lib" "$DST/lib64"

cp "$SRC/bin/arm64-v8a/zygiskd"            "$DST/bin/zygiskd64"
cp "$SRC/bin/armeabi-v7a/zygiskd"          "$DST/bin/zygiskd32"
cp "$SRC/lib/arm64-v8a/libzygisk.so"        "$DST/lib64/libzygisk.so"
cp "$SRC/lib/arm64-v8a/libzygisk_ptrace.so" "$DST/bin/zygisk-ptrace64"
cp "$SRC/lib/armeabi-v7a/libzygisk.so"      "$DST/lib/libzygisk.so"
cp "$SRC/lib/armeabi-v7a/libzygisk_ptrace.so" "$DST/bin/zygisk-ptrace32"

chmod 755 "$DST"/bin/*
chmod 644 "$DST"/lib64/*.so "$DST"/lib/*.so
chcon u:object_r:system_lib_file:s0 "$DST/lib64/libzygisk.so" 2>/dev/null
chcon u:object_r:system_lib_file:s0 "$DST/lib/libzygisk.so" 2>/dev/null

# runtime workdir
mkdir -p /data/adb/onyxzygisk/lib64
cp "$SRC/lib/arm64-v8a/libzygisk.so" /data/adb/onyxzygisk/lib64/libzygisk.so
chmod 555 /data/adb/onyxzygisk/lib64
chmod 644 /data/adb/onyxzygisk/lib64/libzygisk.so
chcon u:object_r:system_file:s0 /data/adb/onyxzygisk/lib64/libzygisk.so 2>/dev/null

echo "layout ok"
ls -la "$DST/bin/" "$DST/lib64/"
