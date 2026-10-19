#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inject.h"
#include "log.h"
#include "zygisk.h"
#include "config.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

// Plain C data only: no std::string / std::vector / std::optional anywhere in
// this file. The module is loaded by Zygisk's builtin linker, which only knows
// about bionic symbols, so libc++ imports would make preload fail.

#define ZF_MODULE_DIR "/data/local/tmp/re.zyg.fri"
#define ZF_LOCAL_PREFIX_SLASH "/data/local/tmp/re.zyg.fri/"

// ---------------------------------------------------------------------------
// Minimal C++ runtime shims
//
// Zygisk's header-only ABI helper allocates with `new T`, `new module_abi` and
// `new Api`; RapidJSON's MemoryPoolAllocator uses RAPIDJSON_NEW (`new`). Those
// are the only C++ runtime imports left. Implement operator new/delete locally
// on top of bionic malloc/free and hide them so we never interpose on the host
// app's libc++ runtime. Result: the module has zero libc++ dependencies.
// ---------------------------------------------------------------------------
void *operator new(size_t size) {
    void *ptr = malloc(size != 0 ? size : 1);
    if (ptr == nullptr) {
        abort();
    }
    return ptr;
}

void *operator new[](size_t size) {
    void *ptr = malloc(size != 0 ? size : 1);
    if (ptr == nullptr) {
        abort();
    }
    return ptr;
}

void operator delete(void *ptr) noexcept {
    free(ptr);
}

void operator delete[](void *ptr) noexcept {
    free(ptr);
}

void operator delete(void *ptr, size_t) noexcept {
    free(ptr);
}

void operator delete[](void *ptr, size_t) noexcept {
    free(ptr);
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
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

static bool copy_file(const char *src, const char *dst) {
    int src_fd = open(src, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) {
        return false;
    }

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (dst_fd < 0) {
        close(src_fd);
        return false;
    }

    char buffer[4096];
    ssize_t bytes_read;
    bool success = true;
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t bytes_written = 0;
        while (bytes_written < bytes_read) {
            ssize_t written = write(dst_fd, buffer + bytes_written, bytes_read - bytes_written);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                success = false;
                break;
            }
            bytes_written += written;
        }
        if (!success) {
            break;
        }
    }
    if (bytes_read < 0) {
        success = false;
    }

    close(src_fd);
    close(dst_fd);
    return success;
}

static bool copy_file_at(int src_dir_fd, const char *src_rel_path, const char *dst_path,
                         int uid, int gid) {
    int src_fd = openat(src_dir_fd, src_rel_path, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) {
        return false;
    }

    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (dst_fd < 0) {
        close(src_fd);
        return false;
    }

    char buffer[4096];
    ssize_t bytes_read;
    bool success = true;
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t bytes_written = 0;
        while (bytes_written < bytes_read) {
            ssize_t written = write(dst_fd, buffer + bytes_written, bytes_read - bytes_written);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                success = false;
                break;
            }
            bytes_written += written;
        }
        if (!success) {
            break;
        }
    }
    if (bytes_read < 0) {
        success = false;
    }

    close(src_fd);
    close(dst_fd);

    if (success && uid != -1 && gid != -1) {
        chown(dst_path, uid, gid);
    }
    chmod(dst_path, 0755);
    return success;
}

static bool copy_config_and_rewrite(const char *src, const char *dst, const char *app_data_dir) {
    int src_fd = open(src, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) {
        return false;
    }

    struct stat st;
    if (fstat(src_fd, &st) != 0 || st.st_size < 0) {
        close(src_fd);
        return false;
    }

    size_t content_size = (size_t) st.st_size;
    char *content = (char *) malloc(content_size + 1);
    if (content == NULL) {
        close(src_fd);
        return false;
    }

    size_t total = 0;
    while (total < content_size) {
        ssize_t bytes_read = read(src_fd, content + total, content_size - total);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(content);
            close(src_fd);
            return false;
        }
        if (bytes_read == 0) {
            break;
        }
        total += (size_t) bytes_read;
    }
    close(src_fd);
    content[total] = '\0';

    // Replace "/data/local/tmp/re.zyg.fri/" with app_data_dir + "/"
    const char *pattern = ZF_LOCAL_PREFIX_SLASH;
    char replacement[ZF_MAX_PATH * 2];
    snprintf(replacement, sizeof(replacement), "%s/", app_data_dir);

    size_t pattern_len = strlen(pattern);
    size_t replacement_len = strlen(replacement);

    size_t occurrences = 0;
    const char *cursor = content;
    while ((cursor = strstr(cursor, pattern)) != NULL) {
        occurrences++;
        cursor += pattern_len;
    }

    size_t extra = replacement_len > pattern_len
                   ? (replacement_len - pattern_len) * occurrences
                   : 0;
    char *rewritten = (char *) malloc(total + extra + 1);
    if (rewritten == NULL) {
        free(content);
        return false;
    }

    char *out = rewritten;
    cursor = content;
    const char *match;
    while ((match = strstr(cursor, pattern)) != NULL) {
        size_t chunk = (size_t) (match - cursor);
        memcpy(out, cursor, chunk);
        out += chunk;
        memcpy(out, replacement, replacement_len);
        out += replacement_len;
        cursor = match + pattern_len;
    }
    size_t tail = strlen(cursor);
    memcpy(out, cursor, tail);
    out += tail;
    *out = '\0';

    size_t out_len = (size_t) (out - rewritten);

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    bool success = false;
    if (dst_fd >= 0) {
        size_t written_total = 0;
        while (written_total < out_len) {
            ssize_t written = write(dst_fd, rewritten + written_total, out_len - written_total);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            written_total += (size_t) written;
        }
        success = (written_total == out_len);
        close(dst_fd);
    }

    free(rewritten);
    free(content);
    return success;
}

static void localize_single_lib(const char *lib_path, const char *app_data_dir, int uid, int gid) {
    if (strncmp(lib_path, ZF_LOCAL_PREFIX_SLASH, strlen(ZF_LOCAL_PREFIX_SLASH)) != 0) {
        return;
    }

    const char *last_slash = strrchr(lib_path, '/');
    const char *filename = last_slash != NULL ? last_slash + 1 : lib_path;

    // Use a dedicated subdirectory in app's cache for our localized files
    char localized_dir[ZF_MAX_PATH * 2];
    snprintf(localized_dir, sizeof(localized_dir), "%s/.zygisk_frida", app_data_dir);

    LOGI("Localizing %s to %s", lib_path, localized_dir);
    if (mkdir(localized_dir, 0755) != 0 && errno != EEXIST) {
        LOGE("Failed to create localized directory %s: %s", localized_dir, strerror(errno));
    } else {
        LOGI("Localized directory ready: %s", localized_dir);
    }
    if (chown(localized_dir, uid, gid) != 0) {
        LOGW("Failed to chown localized directory %s to %d:%d: %s",
             localized_dir, uid, gid, strerror(errno));
    }

    char localized_path[ZF_MAX_PATH * 3];
    snprintf(localized_path, sizeof(localized_path), "%s/%s", localized_dir, filename);

    struct stat st;
    if (stat(lib_path, &st) == 0) {
        if (copy_file(lib_path, localized_path)) {
            chown(localized_path, uid, gid);
            chmod(localized_path, 0755);
            LOGI("Localized library successfully copied: %s -> %s (UID: %d, GID: %d)",
                 lib_path, localized_path, uid, gid);

            // Auto-provision a gadget configuration
            size_t lib_len = strlen(lib_path);
            if (lib_len >= 3 && strcmp(lib_path + lib_len - 3, ".so") == 0) {
                char base_name[ZF_MAX_PATH];
                snprintf(base_name, sizeof(base_name), "%.*s", (int) (strlen(filename) - 3), filename);

                char base_path[ZF_MAX_PATH * 3];
                snprintf(base_path, sizeof(base_path), "%s/%s", localized_dir, base_name);

                // We support multiple naming conventions for Frida Gadget configs
                char config_dests[3][ZF_MAX_PATH * 3];
                snprintf(config_dests[0], sizeof(config_dests[0]), "%s.config.so", base_path);
                snprintf(config_dests[1], sizeof(config_dests[1]), "%s.so.config", base_path);
                snprintf(config_dests[2], sizeof(config_dests[2]), "%s/gadget.config", localized_dir);

                char config_src[ZF_MAX_PATH * 3];
                snprintf(config_src, sizeof(config_src), "%.*s.config.so",
                         (int) (lib_len - 3), lib_path);
                bool config_provided = (stat(config_src, &st) == 0);

                if (!config_provided) {
                    // try .so.config at source
                    snprintf(config_src, sizeof(config_src), "%s.config", lib_path);
                    config_provided = (stat(config_src, &st) == 0);
                }

                if (config_provided) {
                    for (int i = 0; i < 3; i++) {
                        if (copy_config_and_rewrite(config_src, config_dests[i], localized_dir)) {
                            chown(config_dests[i], uid, gid);
                            chmod(config_dests[i], 0644);
                            LOGI("Provisoned config: %s -> %s", config_src, config_dests[i]);
                        }
                    }
                } else {
                    // Create default configs pointing to localized script
                    char default_config[ZF_MAX_PATH * 4];
                    snprintf(default_config, sizeof(default_config),
                             "{\n"
                             "  \"interaction\": {\n"
                             "    \"type\": \"listen\",\n"
                             "    \"address\": \"127.0.0.1\",\n"
                             "    \"port\": 27042,\n"
                             "    \"on_load\": \"resume\"\n"
                             "  },\n"
                             "  \"logging\": {\n"
                             "    \"level\": \"info\",\n"
                             "    \"type\": \"logcat\"\n"
                             "  }\n"
                             "}");
                    for (int i = 0; i < 3; i++) {
                        int config_fd = open(config_dests[i], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                        if (config_fd >= 0) {
                            size_t config_len = strlen(default_config);
                            ssize_t written = write(config_fd, default_config, config_len);
                            (void) written;
                            close(config_fd);
                            chown(config_dests[i], uid, gid);
                            LOGI("Created default config at %s", config_dests[i]);
                        }
                    }
                }
            }
        } else {
            LOGE("CRITICAL: Failed to copy library to app cache: %s -> %s (Errno: %d)",
                 lib_path, localized_path, errno);
        }
    } else {
        LOGW("Library path scheduled for injection does not exist in tmp: %s", lib_path);
    }
}

class MyModule : public zygisk::ModuleBase {
 public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        if (args->nice_name == nullptr || args->app_data_dir == nullptr) {
            LOGW("preAppSpecialize: nice_name or app_data_dir is null, skipping localization check");
            return;
        }

        JNIEnv *env = this->env;
        if (env == nullptr) {
            LOGE("preAppSpecialize: JNIEnv is null, cannot proceed");
            return;
        }

        const char *raw_app_name = env->GetStringUTFChars(args->nice_name, nullptr);
        if (raw_app_name == nullptr) {
            return;
        }
        copy_string(this->app_name, sizeof(this->app_name), raw_app_name);
        env->ReleaseStringUTFChars(args->nice_name, raw_app_name);

        const char *raw_app_data_dir = env->GetStringUTFChars(args->app_data_dir, nullptr);
        if (raw_app_data_dir == nullptr) {
            return;
        }
        copy_string(this->app_data_dir, sizeof(this->app_data_dir), raw_app_data_dir);
        env->ReleaseStringUTFChars(args->app_data_dir, raw_app_data_dir);

        LOGI("preAppSpecialize for %s (UID: %d, Data Dir: %s)",
             this->app_name, args->uid, this->app_data_dir);

        // 1. Ensure /data/local/tmp/re.zyg.fri directory exists (creates dynamic self-healing of missing directory)
        struct stat st;
        if (stat(ZF_MODULE_DIR, &st) != 0 && errno == ENOENT) {
            if (mkdir(ZF_MODULE_DIR, 0777) == 0) {
                chmod(ZF_MODULE_DIR, 0777);
                chown(ZF_MODULE_DIR, 2000, 2000); // shell:shell
                LOGI("Successfully created config directory at runtime: %s", ZF_MODULE_DIR);
            } else if (errno != EEXIST) {
                LOGE("Failed to create config directory: %s", ZF_MODULE_DIR);
            }
        }

        // 2. Ensure config.json.example and script.js exist in /data/local/tmp
        int module_dir_fd = api->getModuleDir();
        if (module_dir_fd >= 0) {
            char example_config_path[ZF_MAX_PATH * 2];
            snprintf(example_config_path, sizeof(example_config_path), "%s/config.json.example", ZF_MODULE_DIR);
            if (stat(example_config_path, &st) != 0 && errno == ENOENT) {
                if (copy_file_at(module_dir_fd, "config.json.example", example_config_path, 2000, 2000)) {
                    LOGI("Successfully copied config.json.example dynamically to %s", example_config_path);
                }
            }

            char script_template_path[ZF_MAX_PATH * 2];
            snprintf(script_template_path, sizeof(script_template_path), "%s/script.js", ZF_MODULE_DIR);
            if (stat(script_template_path, &st) != 0 && errno == ENOENT) {
                if (copy_file_at(module_dir_fd, "script.js", script_template_path, 2000, 2000)) {
                    LOGI("Successfully copied script.js template dynamically to %s", script_template_path);
                }
            }
        } else {
            LOGE("Could not obtain module directory fd to copy templates");
        }

        // 3. Localize libraries designed for injection to the target application's directory
        struct target_config cfg;
        if (load_config(ZF_MODULE_DIR, this->app_name, &cfg) && cfg.enabled) {
            int uid = args->uid;
            int gid = args->gid;

            LOGI("Target detected: %s (Data dir: %s, UID: %d, GID: %d)",
                 this->app_name, this->app_data_dir, uid, gid);

            // 4. Ensure script.js is copied/localized to app_data_dir for Frida script interaction
            char script_src[ZF_MAX_PATH * 2];
            snprintf(script_src, sizeof(script_src), "%s/script.js", ZF_MODULE_DIR);

            char localized_dir[ZF_MAX_PATH * 2];
            snprintf(localized_dir, sizeof(localized_dir), "%s/.zygisk_frida", this->app_data_dir);

            if (mkdir(localized_dir, 0755) != 0 && errno != EEXIST) {
                LOGE("Failed to create localized directory for script %s: %s",
                     localized_dir, strerror(errno));
            }
            if (chown(localized_dir, uid, gid) != 0) {
                LOGW("Failed to chown localized directory for script %s: %s",
                     localized_dir, strerror(errno));
            }

            char script_dst[ZF_MAX_PATH * 3];
            snprintf(script_dst, sizeof(script_dst), "%s/script.js", localized_dir);

            struct stat script_st;
            if (stat(script_src, &script_st) == 0) {
                if (copy_file(script_src, script_dst)) {
                    chown(script_dst, uid, gid);
                    chmod(script_dst, 0644);
                    LOGI("Localized script.js copied: %s -> %s (UID: %d, GID: %d)",
                         script_src, script_dst, uid, gid);
                } else {
                    LOGE("Failed to copy script.js to app cache at %s", script_dst);
                }
            } else {
                LOGW("Source script.js not found at %s", script_src);
            }

            for (int i = 0; i < cfg.injected_libraries_count; i++) {
                localize_single_lib(cfg.injected_libraries[i], this->app_data_dir, uid, gid);
            }

            if (cfg.child_gating.enabled) {
                for (int i = 0; i < cfg.child_gating.injected_libraries_count; i++) {
                    localize_single_lib(cfg.child_gating.injected_libraries[i],
                                        this->app_data_dir, uid, gid);
                }
            }
        }
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        if (this->app_name[0] == '\0' || this->app_data_dir[0] == '\0') {
            this->api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (!check_and_inject(this->app_name, this->app_data_dir)) {
            this->api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

 private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;
    char app_name[ZF_MAX_PATH] = {0};
    char app_data_dir[ZF_MAX_PATH * 2] = {0};
};

REGISTER_ZYGISK_MODULE(MyModule)
