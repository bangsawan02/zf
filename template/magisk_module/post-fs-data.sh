#!/system/bin/sh
MODDIR=${0%/*}

TMP_MODULE_DIR=/data/local/tmp/re.zyg.fri

# Dynamically repair or create directory on boot (some platforms clear /data/local/tmp)
if [ ! -d "$TMP_MODULE_DIR" ]; then
    mkdir -p "$TMP_MODULE_DIR"
    if [ -f "$MODDIR/config.json.example" ]; then
        cp "$MODDIR/config.json.example" "$TMP_MODULE_DIR/config.json.example"
    fi
    if [ -f "$MODDIR/libgadget.config.so" ]; then
        cp "$MODDIR/libgadget.config.so" "$TMP_MODULE_DIR/libgadget.config.so"
    fi
    if [ -f "$MODDIR/script.js" ]; then
        cp "$MODDIR/script.js" "$TMP_MODULE_DIR/script.js"
    fi
fi

# The live config is the only thing the module cannot regenerate by itself.
# Restore it from the module directory when /data/local/tmp was cleared.
if [ ! -f "$TMP_MODULE_DIR/config.json" ] && [ -f "$MODDIR/config.json" ]; then
    cp "$MODDIR/config.json" "$TMP_MODULE_DIR/config.json"
fi

if [ -d "$TMP_MODULE_DIR" ]; then
    chown -R 2000:2000 "$TMP_MODULE_DIR" # shell:shell
    chmod -R 0777 "$TMP_MODULE_DIR"
fi
