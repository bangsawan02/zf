#include "inject.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "log.h"
#include "child_gating.h"
#include "xdl.h"

// No C++ containers / threads / streams here on purpose: the module is
// preloaded by Zygisk's builtin linker, which cannot resolve libc++ symbols.

#define ZF_MODULE_DIR "/data/local/tmp/re.zyg.fri"
#define ZF_LOCAL_PREFIX "/data/local/tmp/re.zyg.fri"
#define ZF_LOCAL_PREFIX_SLASH "/data/local/tmp/re.zyg.fri/"

static void copy_str_local(char *dst, size_t dst_size, const char *src) {
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    size_t len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void sleep_ms(uint64_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t) (ms / 1000);
    ts.tv_nsec = (long) ((ms % 1000) * 1000000L);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static void get_process_name(char *buffer, size_t size) {
    if (buffer == NULL || size == 0) {
        return;
    }

    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        buffer[0] = '\0';
        return;
    }

    ssize_t read_bytes = read(fd, buffer, size - 1);
    close(fd);

    if (read_bytes < 0) {
        read_bytes = 0;
    }
    buffer[read_bytes] = '\0';
}

static void wait_for_init(const char *app_name) {
    LOGI("Wait for process to complete init");

    // wait until the process is renamed to the package name
    char process_name[512];
    for (;;) {
        get_process_name(process_name, sizeof(process_name));
        if (strstr(process_name, app_name) != NULL) {
            break;
        }
        sleep_ms(10);
    }

    // additional tolerance for the init to complete after process rename
    sleep_ms(100);

    LOGI("Process init completed");
}

static void delay_start_up(uint64_t start_up_delay_ms) {
    if (start_up_delay_ms == 0) {
        return;
    }

    LOGI("Waiting for configured start up delay %llu ms", (unsigned long long) start_up_delay_ms);

    int countdown = 0;
    uint64_t delay = start_up_delay_ms;

    for (int i = 0; i < 10 && delay > 1000; i++) {
        delay -= 1000;
        countdown++;
    }

    sleep_ms(delay);

    for (int i = countdown; i > 0; i--) {
        LOGI("Injecting libs in %d seconds", i);
        sleep_ms(1000);
    }
}

void inject_lib(const char *lib_path, const char *log_context) {
    if (log_context == NULL) {
        log_context = "";
    }

    void *handle = xdl_open(lib_path, XDL_TRY_FORCE_LOAD);
    if (handle != NULL) {
        LOGI("%sInjected %s with handle %p", log_context, lib_path, handle);
        return;
    }

    const char *xdl_err = dlerror();
    const char *xdl_error_msg = xdl_err != NULL ? xdl_err : "No direct system link error registered";

    handle = dlopen(lib_path, RTLD_NOW);
    if (handle != NULL) {
        LOGI("%sInjected %s with handle %p (dlopen)", log_context, lib_path, handle);
        return;
    }

    const char *dl_err = dlerror();
    const char *dl_error_msg = dl_err != NULL ? dl_err : "No standard linking error registered";

    LOGE("%sFailed to inject %s (xdl_open): %s", log_context, lib_path, xdl_error_msg);
    LOGE("%sFailed to inject %s (dlopen): %s", log_context, lib_path, dl_error_msg);

    // Check if file is readable
    if (access(lib_path, R_OK) != 0) {
        LOGE("%sInjection failed: File is not readable or does not exist: %s (Errno: %d)",
             log_context, lib_path, errno);
    }
}

static void *inject_libs_thread(void *arg) {
    struct target_config *cfg = (struct target_config *) arg;

    // We need to wait for process initialization to complete.
    // Loading the gadget before that will freeze the process
    // before the init has completed. This make the process
    // undiscoverable or otherwise cause issue attaching.
    wait_for_init(cfg->app_name);

    if (cfg->child_gating.enabled) {
        enable_child_gating(&cfg->child_gating);
    }

    delay_start_up(cfg->start_up_delay_ms);

    for (int i = 0; i < cfg->injected_libraries_count; i++) {
        LOGI("Injecting %s", cfg->injected_libraries[i]);
        inject_lib(cfg->injected_libraries[i], "");
    }

    free(cfg);
    return NULL;
}

// Translate a path targeted at /data/local/tmp/re.zyg.fri to the app's
// localized subdirectory, falling back to the legacy app_data_dir location.
static void localize_library_path(char *lib_path, size_t lib_path_size,
                                  const char *app_data_dir, const char *localized_dir,
                                  int require_slash_prefix) {
    const char *prefix = require_slash_prefix ? ZF_LOCAL_PREFIX_SLASH : ZF_LOCAL_PREFIX;
    size_t prefix_len = strlen(prefix);

    if (strncmp(lib_path, prefix, prefix_len) != 0) {
        LOGI("Library path is already outside /data/local/tmp or match prefix not found: %s", lib_path);
        return;
    }

    const char *last_slash = strrchr(lib_path, '/');
    const char *filename = last_slash != NULL ? last_slash + 1 : lib_path;

    char localized_path[ZF_MAX_PATH * 2];
    snprintf(localized_path, sizeof(localized_path), "%s/%s", localized_dir, filename);

    LOGI("Found match for /data/local/tmp/re.zyg.fri. Checking localized candidate: %s", localized_path);
    if (access(localized_path, F_OK) == 0) {
        copy_str_local(lib_path, lib_path_size, localized_path);
        LOGI("SUCCESS: Found localized library, using: %s", lib_path);
        return;
    }

    int err = errno;

    // Fallback to direct app_data_dir (old behavior)
    char old_localized_path[ZF_MAX_PATH * 2];
    snprintf(old_localized_path, sizeof(old_localized_path), "%s/%s", app_data_dir, filename);
    LOGI("Localized file %s not found (errno: %d). Checking fallback: %s",
         localized_path, err, old_localized_path);

    if (access(old_localized_path, F_OK) == 0) {
        copy_str_local(lib_path, lib_path_size, old_localized_path);
        LOGI("SUCCESS: Using fallback localized library: %s", lib_path);
        return;
    }

    LOGW("FAILED: No localized version found at %s or %s (Will use original: %s)",
         localized_path, old_localized_path, lib_path);
}

int check_and_inject(const char *app_name, const char *app_data_dir) {
    struct target_config cfg;
    if (!load_config(ZF_MODULE_DIR, app_name, &cfg)) {
        return 0;
    }

    LOGI("App detected: %s", app_name);
    LOGI("PID: %d", getpid());

    if (!cfg.enabled) {
        LOGI("Injection disabled for %s", app_name);
        return 0;
    }

    char localized_dir[ZF_MAX_PATH * 2];
    snprintf(localized_dir, sizeof(localized_dir), "%s/.zygisk_frida", app_data_dir);

    // Translate any paths targeted to /data/local/tmp/re.zyg.fri to the specialized localized subdirectory
    LOGI("Processing %d libraries for possible localization", cfg.injected_libraries_count);
    for (int i = 0; i < cfg.injected_libraries_count; i++) {
        LOGI("Evaluating library for localization: %s", cfg.injected_libraries[i]);
        localize_library_path(cfg.injected_libraries[i], sizeof(cfg.injected_libraries[i]),
                              app_data_dir, localized_dir, 0);
    }

    if (cfg.child_gating.enabled) {
        for (int i = 0; i < cfg.child_gating.injected_libraries_count; i++) {
            localize_library_path(cfg.child_gating.injected_libraries[i],
                                  sizeof(cfg.child_gating.injected_libraries[i]),
                                  app_data_dir, localized_dir, 1);
        }
    }

    struct target_config *thread_cfg = (struct target_config *) malloc(sizeof(*thread_cfg));
    if (thread_cfg == NULL) {
        LOGE("Failed to allocate injection thread context");
        return 0;
    }
    *thread_cfg = cfg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    pthread_t thread;
    int result = pthread_create(&thread, &attr, inject_libs_thread, thread_cfg);
    pthread_attr_destroy(&attr);

    if (result != 0) {
        LOGE("Failed to start injection thread: %s", strerror(result));
        free(thread_cfg);
        return 0;
    }

    return 1;
}
