#ifndef ZYGISKFRIDA_CONFIG_H
#define ZYGISKFRIDA_CONFIG_H

#include <stdint.h>

// The module is loaded into every app process by zygote. It must not depend on
// the C++ standard library: Zygisk resolves the module's imports with a minimal
// builtin linker that has no libc++_shared.so available. Everything below is
// therefore plain C data (fixed-size buffers), so the resulting .so has zero
// libc++ runtime symbols.

#define ZF_MAX_LIBS 32
#define ZF_MAX_PATH 512
#define ZF_MAX_MODE 32
#define ZF_MAX_THREAD_ALIAS 32

// Post-injection stealth options. All of these run inside the target process
// after the gadget has been loaded.
struct stealth_config {
    int enabled;                            // master switch (default: 1)
    int remap;                              // scrub file paths from /proc/self/maps (default: 1)
    int rename_threads;                     // rename gum-*/gmain/gdbus threads (default: 1)
    int unlink_payload;                     // delete localized .so after dlopen (default: 1)
    uint64_t remap_delay_ms;                // let the gadget finish init first (default: 750)
    char thread_alias[ZF_MAX_THREAD_ALIAS]; // replacement thread name
};

struct child_gating_config {
    int enabled;
    char mode[ZF_MAX_MODE];
    char injected_libraries[ZF_MAX_LIBS][ZF_MAX_PATH];
    int injected_libraries_count;
};

struct target_config {
    int enabled;
    char app_name[ZF_MAX_PATH];
    uint64_t start_up_delay_ms;
    char injected_libraries[ZF_MAX_LIBS][ZF_MAX_PATH];
    int injected_libraries_count;
    struct child_gating_config child_gating;

    // Hot reload of the Frida script: a generated hotreload.js watches the
    // localized script and re-evaluates it whenever it changes.
    int hot_reload;               // default: 1
    uint64_t reload_interval_ms;  // default: 1500

    struct stealth_config stealth;
};

// Fill `out` with sane defaults before any config layer is merged.
void target_config_set_defaults(struct target_config *out);

// Returns 1 and fills `out` when the app has a target configuration.
int load_config(const char *module_dir, const char *app_name, struct target_config *out);

// Rewrites a gadget config so `interaction.script.path` is replaced by a
// generated hotreload.js wrapper (no-op for listen mode or when already
// installed). `localized_dir` is where the wrapper is written.
int config_rewrite_for_hot_reload(const char *config_path,
                                  const char *localized_dir,
                                  uint64_t interval_ms,
                                  int uid,
                                  int gid);

#endif  // ZYGISKFRIDA_CONFIG_H
