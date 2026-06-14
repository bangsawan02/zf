SKIPUNZIP=1

FLAVOR=@FLAVOR@
MODULE_ID=@MODULE_ID@

TMP_MODULE_DIR=/data/local/tmp/re.zyg.fri

if [ "$FLAVOR" != "zygisk" ]; then
  abort "! Unknown ZygiskFrida flavor: $FLAVOR"
else
  ui_print "- ZygiskFrida flavor: $FLAVOR"
fi

if [ "$ARCH" != "arm64" ]; then
  abort "! Unsupported platform: $ARCH. This build only supports arm64-v8a."
else
  ui_print "- Device platform: $ARCH"
fi

ui_print "- Extracting verify.sh"
unzip -o "$ZIPFILE" 'verify.sh' -d "$TMPDIR" >&2
if [ ! -f "$TMPDIR/verify.sh" ]; then
  ui_print    "*********************************************************"
  ui_print    "! Unable to extract verify.sh!"
  ui_print    "! This zip may be corrupted, please try downloading again"
  abort "*********************************************************"
fi
. $TMPDIR/verify.sh

ui_print "- Extracting module files"
extract "$ZIPFILE" 'module.prop' "$MODPATH"
extract "$ZIPFILE" 'uninstall.sh' "$MODPATH"

LIB64_NAME="arm64-v8a.so"
LIB64_DEST="$MODPATH/zygisk"
BUSYBOX_BIN=/data/adb/magisk/busybox

if [ ! -f $BUSYBOX_BIN ]; then
  BUSYBOX_BIN=/data/adb/ksu/bin/busybox
fi

if [ ! -f $BUSYBOX_BIN ]; then
  BUSYBOX_BIN=/data/adb/ap/bin/busybox
fi

if [ ! -f $BUSYBOX_BIN ]; then
  abort "! unable to locate busybox"
fi

ui_print "- Using busybox: $BUSYBOX_BIN"

mkdir -p "$LIB64_DEST"

ui_print "- Extracting 64-bit libraries"
extract "$ZIPFILE" "lib/$LIB64_NAME" "$LIB64_DEST" true

ui_print "- Creating config directory: $TMP_MODULE_DIR"
mkdir -p "$TMP_MODULE_DIR"

ui_print "- Extracting config.json.example"
extract "$ZIPFILE" "config.json.example" "$TMP_MODULE_DIR" true

ui_print "- Adjusting directory permissions to 0777 (shell:shell)"
set_perm_recursive "$TMP_MODULE_DIR" 2000 2000 0777 0777
set_perm_recursive "$MODPATH" 0 0 0755 0644





