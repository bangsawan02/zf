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
};

// Returns 1 and fills `out` when the app has a target configuration.
int load_config(const char *module_dir, const char *app_name, struct target_config *out);

#endif  // ZYGISKFRIDA_CONFIG_H
