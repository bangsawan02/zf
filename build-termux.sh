#!/data/data/com.termux/files/usr/bin/bash
# Build ZygiskFrida module directly inside Termux (no Gradle / no Android SDK).
#
# Requirements:
#   pkg install -y clang ndk-sysroot python
#
# Usage:
#   ./build-termux.sh            # release build (stripped, optimized)
#   ./build-termux.sh --debug    # debug build (symbols, -O0, no strip)
#   ./build-termux.sh --debug --no-package   # only compile, do not build the zip
#
# Outputs:
#   .termux-build/libzygiskfrida.so          # stripped binary used in the module
#   .termux-build/libzygiskfrida.so.debug    # unstripped binary (symbolication)
#   .termux-build/symbols/                   # debug symbols for crash reports
#   .termux-build/compile_commands.json      # for clangd / IDE support
#   ../ZygiskFrida-termux-zygisk.zip         # flashable module (unless --no-package)

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

MODE="release"
PACKAGE=1
for arg in "$@"; do
    case "$arg" in
        --debug)      MODE="debug" ;;
        --release)    MODE="release" ;;
        --no-package) PACKAGE=0 ;;
        -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

say()  { printf '\033[1;36m==>\033[0m %s\n' "$1"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$1"; }
die()  { printf '\033[1;31m[error]\033[0m %s\n' "$1" >&2; exit 1; }

command -v clang  >/dev/null || die "clang not found: pkg install -y clang"
command -v python >/dev/null || die "python not found: pkg install -y python"

# ---------------------------------------------------------------------------
# Flags
#
# -fno-exceptions / -fno-rtti mirror the NDK build (APP_STL=none): the module
# must never abort a host app because of an exception crossing the boundary.
# ---------------------------------------------------------------------------
COMMON_CXX=(
    -std=c++17 -fPIC -fno-exceptions -fno-rtti -fno-threadsafe-statics
    -fvisibility=hidden -fvisibility-inlines-hidden
    -fno-omit-frame-pointer
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -I"$JNI" -I"$JNI/xdl/include" -I"$JNI/include"
)
COMMON_C=(
    -fPIC -Wall -Wextra -Wno-unused-parameter
    -I"$JNI/xdl/include" -I"$JNI"
)

if [ "$MODE" = "debug" ]; then
    CXX_OPT=(-O0 -g3 -ggdb)
    C_OPT=(-O0 -g3 -ggdb)
    STRIP=0
    say "Build mode: DEBUG (-O0 -g3, symbols kept)"
else
    CXX_OPT=(-O2 -g)
    C_OPT=(-O2 -g)
    STRIP=1
    say "Build mode: RELEASE (-O2, stripped)"
fi

rm -rf "$BUILD"
mkdir -p "$OBJ" "$STAGE/lib" "$BUILD/symbols"

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------
say "Compiling xdl (C)"
for f in xdl/xdl.c xdl/xdl_iterate.c xdl/xdl_linker.c xdl/xdl_lzma.c xdl/xdl_util.c; do
    base="$(basename "$f" .c)"
    clang -c "${C_OPT[@]}" "${COMMON_C[@]}" "$JNI/$f" -o "$OBJ/$base.o"
done

say "Compiling module (C++)"
for f in inject.cpp config.cpp child_gating.cpp main_zygisk.cpp; do
    base="$(basename "$f" .cpp)"
    clang++ -c "${CXX_OPT[@]}" "${COMMON_CXX[@]}" \
        "$JNI/$f" -o "$OBJ/$base.o"
done

say "Linking libzygiskfrida.so"
clang++ -shared -fPIC "$OBJ"/*.o -o "$BUILD/libzygiskfrida.so.unstripped" \
    -nostdlib++ -llog -ldl -Wl,--allow-shlib-undefined

# The module must not depend on libc++ at all. It is loaded by Zygisk's
# builtin linker, which only resolves bionic symbols, and Android 13 has no
# /system/lib64/libc++_shared.so any more. All std:: containers/streams were
# replaced with plain C, so the module should have no libc++ imports.
if readelf -d "$BUILD/libzygiskfrida.so.unstripped" 2>/dev/null | grep -q "libc++"; then
    die "module still depends on libc++ - remove the remaining std:: usage"
fi

if nm -D -u "$BUILD/libzygiskfrida.so.unstripped" 2>/dev/null | grep -Eq "(_ZNSt|_ZSt|__ndk1|__cxx11)"; then
    echo "--- unresolved C++ symbols ---" >&2
    nm -D -u "$BUILD/libzygiskfrida.so.unstripped" | grep -E "(_ZNSt|_ZSt|__ndk1|__cxx11)" >&2
    die "module has unresolved C++ runtime symbols"
fi

# Keep an unstripped copy for symbolication, then strip the shipped binary.
cp "$BUILD/libzygiskfrida.so.unstripped" "$BUILD/libzygiskfrida.so.debug"
if [ "$STRIP" = "1" ]; then
    command -v llvm-strip >/dev/null && llvm-strip --strip-unneeded "$BUILD/libzygiskfrida.so.unstripped" \
        || strip --strip-unneeded "$BUILD/libzygiskfrida.so.unstripped" 2>/dev/null \
        || warn "no strip tool found, shipping unstripped binary"
fi
cp "$BUILD/libzygiskfrida.so.unstripped" "$BUILD/libzygiskfrida.so"

# Keep the unstripped binary around for crash symbolication.
cp "$BUILD/libzygiskfrida.so.unstripped" "$BUILD/symbols/libzygiskfrida.so.debug"
if command -v llvm-addr2line >/dev/null; then
    llvm-addr2line -f -C -e "$BUILD/libzygiskfrida.so.unstripped" > "$BUILD/symbols/symbols.txt" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# compile_commands.json (clangd / IDE)
# ---------------------------------------------------------------------------
say "Generating compile_commands.json"
{
    printf '[\n'
    first=1
    for f in xdl/xdl.c xdl/xdl_iterate.c xdl/xdl_linker.c xdl/xdl_lzma.c xdl/xdl_util.c; do
        [ $first -eq 1 ] || printf ',\n'
        first=0
        printf '  {"directory": "%s", "file": "%s", "arguments": ["clang", "-c", %s, "%s"]}' \
            "$JNI" "$JNI/$f" \
            "$(printf '"%s", ' "${C_OPT[@]}" "${COMMON_C[@]}")" \
            "$JNI/$f"
    done
    for f in inject.cpp config.cpp child_gating.cpp main_zygisk.cpp; do
        [ $first -eq 1 ] || printf ',\n'
        first=0
        printf '  {"directory": "%s", "file": "%s", "arguments": ["clang++", "-c", %s, "%s"]}' \
            "$JNI" "$JNI/$f" \
            "$(printf '"%s", ' "${CXX_OPT[@]}" "${COMMON_CXX[@]}")" \
            "$JNI/$f"
    done
    printf '\n]\n'
} > "$BUILD/compile_commands.json"
cp "$BUILD/compile_commands.json" "$ROOT/compile_commands.json"

# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------
say "Verifying binary"
READELF="$(command -v llvm-readelf || command -v readelf || echo true)"
if [ "$READELF" != "true" ]; then
    "$READELF" -h "$BUILD/libzygiskfrida.so" | grep -E 'Class|Machine|Type' || true
    echo "--- needed libraries ---"
    "$READELF" -d "$BUILD/libzygiskfrida.so" 2>/dev/null | grep NEEDED || true
fi
if command -v nm >/dev/null; then
    if nm -D --defined-only "$BUILD/libzygiskfrida.so" 2>/dev/null | grep -q zygisk_module_entry; then
        echo "  ok: zygisk_module_entry exported"
    else
        die "zygisk_module_entry is not exported - the module will not load"
    fi
fi

# ---------------------------------------------------------------------------
# Package
# ---------------------------------------------------------------------------
if [ "$PACKAGE" = "0" ]; then
    say "Skipping packaging (--no-package)"
    exit 0
fi

say "Staging module"
cp -r "$ROOT/template/magisk_module/." "$STAGE/"
cp "$BUILD/libzygiskfrida.so" "$STAGE/lib/arm64-v8a.so"

sed -i "s/^id=.*/id=$MODULE_ID/"                            "$STAGE/module.prop"
sed -i "s/^name=.*/name=$MODULE_NAME/"                      "$STAGE/module.prop"
sed -i "s/^version=.*/version=$MODULE_VERSION/"              "$STAGE/module.prop"
sed -i "s/^versionCode=.*/versionCode=$MODULE_VERSION_CODE/" "$STAGE/module.prop"
sed -i "s/^author=.*/author=$MODULE_AUTHOR/"                "$STAGE/module.prop"
sed -i "s/^description=.*/description=$MODULE_DESC/"        "$STAGE/module.prop"
sed -i "s/^FLAVOR=@FLAVOR@/FLAVOR=$FLAVOR/"                "$STAGE/customize.sh"
sed -i "s/^MODULE_ID=@MODULE_ID@/MODULE_ID=$MODULE_ID/"     "$STAGE/customize.sh"

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
say "Debug artifacts: $BUILD/libzygiskfrida.so.debug"
