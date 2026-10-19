#!/data/data/com.termux/files/usr/bin/bash
# Build ZygiskFrida module directly inside Termux (no Gradle / no Android SDK).
#
# Requirements:
#   pkg install -y clang ndk-sysroot python
#
# Usage:
#   ./build-termux.sh
#
# Output:
#   ../ZygiskFrida-termux-zygisk.zip   (flashable Magisk/APatch/KernelSU module)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
JNI="$ROOT/module/src/jni"
BUILD="$ROOT/.termux-build"
OBJ="$BUILD/obj"
STAGE="$BUILD/module"
OUT="$ROOT/../ZygiskFrida-termux-zygisk.zip"

MODULE_ID="zygiskfrida"
MODULE_NAME="ZygiskFrida"
MODULE_VERSION="v1.9.0"
MODULE_VERSION_CODE="12"
MODULE_AUTHOR="lico-n"
MODULE_DESC="Injects frida gadget via zygisk. (flavor: zygisk)"
FLAVOR="zygisk"

say() { printf '\033[1;36m==>\033[0m %s\n' "$1"; }

command -v clang >/dev/null || { echo "clang not found: pkg install -y clang"; exit 1; }
command -v python >/dev/null || { echo "python not found: pkg install -y python"; exit 1; }

rm -rf "$BUILD"
mkdir -p "$OBJ" "$STAGE/lib"

say "Compiling xdl (C)"
for f in xdl/xdl.c xdl/xdl_iterate.c xdl/xdl_linker.c xdl/xdl_lzma.c xdl/xdl_util.c; do
    base="$(basename "$f" .c)"
    clang -c -fPIC -I"$JNI/xdl/include" -I"$JNI" "$JNI/$f" -o "$OBJ/$base.o"
done

say "Compiling module (C++)"
for f in inject.cpp config.cpp child_gating.cpp main_zygisk.cpp; do
    base="$(basename "$f" .cpp)"
    clang++ -c -fPIC -std=c++17 -include optional \
        -I"$JNI" -I"$JNI/xdl/include" -I"$JNI/include" \
        "$JNI/$f" -o "$OBJ/$base.o"
done

say "Linking libzygiskfrida.so"
clang++ -shared -fPIC "$OBJ"/*.o -o "$BUILD/libzygiskfrida.so" \
    -lc++_shared -llog -ldl -Wl,--allow-shlib-undefined

say "Staging module"
cp -r "$ROOT/template/magisk_module/." "$STAGE/"
cp "$BUILD/libzygiskfrida.so" "$STAGE/lib/arm64-v8a.so"

sed -i "s/^id=.*/id=$MODULE_ID/"                          "$STAGE/module.prop"
sed -i "s/^name=.*/name=$MODULE_NAME/"                    "$STAGE/module.prop"
sed -i "s/^version=.*/version=$MODULE_VERSION/"            "$STAGE/module.prop"
sed -i "s/^versionCode=.*/versionCode=$MODULE_VERSION_CODE/" "$STAGE/module.prop"
sed -i "s/^author=.*/author=$MODULE_AUTHOR/"              "$STAGE/module.prop"
sed -i "s/^description=.*/description=$MODULE_DESC/"      "$STAGE/module.prop"
sed -i "s/^FLAVOR=@FLAVOR@/FLAVOR=$FLAVOR/"              "$STAGE/customize.sh"
sed -i "s/^MODULE_ID=@MODULE_ID@/MODULE_ID=$MODULE_ID/"   "$STAGE/customize.sh"

say "Generating sha256sum manifests"
cd "$STAGE"
find . -type f ! -name "*.sha256sum" | while read -r f; do
    sha256sum "$f" | cut -d' ' -f1 > "$f.sha256sum"
done

say "Zipping"
rm -f "$OUT"
python3 - "$STAGE" "$OUT" <<'PY'
import os, sys, zipfile
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for dirpath, _, files in os.walk(root):
        for f in files:
            full = os.path.join(dirpath, f)
            z.write(full, os.path.relpath(full, root))
PY

say "Done: $OUT"
ls -lh "$OUT"
