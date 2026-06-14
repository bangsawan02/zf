#include <string>
#include <vector>
#include <optional>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include "inject.h"
#include "log.h"
#include "zygisk.h"
#include "config.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

static bool copy_file(const std::string& src, const std::string& dst) {
    int src_fd = open(src.c_str(), O_RDONLY);
    if (src_fd < 0) return false;
    
    int dst_fd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
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
                success = false;
                break;
            }
            bytes_written += written;
        }
        if (!success) break;
    }
    
    close(src_fd);
    close(dst_fd);
    return success;
}

static bool copy_file_at(int src_dir_fd, const char* src_rel_path, const std::string& dst_path, int uid = -1, int gid = -1) {
    int src_fd = openat(src_dir_fd, src_rel_path, O_RDONLY);
    if (src_fd < 0) return false;
    
    int dst_fd = open(dst_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
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
                success = false;
                break;
            }
            bytes_written += written;
        }
        if (!success) break;
    }
    
    close(src_fd);
    close(dst_fd);
    
    if (success && uid != -1 && gid != -1) {
        chown(dst_path.c_str(), uid, gid);
    }
    chmod(dst_path.c_str(), 0755);
    return success;
}

static void localize_single_lib(const std::string& lib_path, const std::string& app_data_dir, int uid, int gid) {
    if (lib_path.rfind("/data/local/tmp/re.zyg.fri/", 0) == 0) {
        size_t last_slash = lib_path.find_last_of('/');
        std::string filename = (last_slash == std::string::npos) ? lib_path : lib_path.substr(last_slash + 1);
        std::string localized_path = app_data_dir + "/" + filename;

        struct stat st;
        if (stat(lib_path.c_str(), &st) == 0) {
            if (copy_file(lib_path, localized_path)) {
                chown(localized_path.c_str(), uid, gid);
                chmod(localized_path.c_str(), 0755);
                LOGI("Localized library copied: %s -> %s (UID: %d, GID: %d)", lib_path.c_str(), localized_path.c_str(), uid, gid);
            } else {
                LOGE("Failed to copy library to app cache: %s -> %s", lib_path.c_str(), localized_path.c_str());
            }
        } else {
            LOGW("Library path scheduled for injection does not exist in tmp: %s", lib_path.c_str());
        }
    }
}

class MyModule : public zygisk::ModuleBase {
 public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        const char *raw_app_name = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!raw_app_name) return;
        std::string app_name(raw_app_name);
        this->env->ReleaseStringUTFChars(args->nice_name, raw_app_name);

        const char *raw_app_data_dir = env->GetStringUTFChars(args->app_data_dir, nullptr);
        if (!raw_app_data_dir) return;
        std::string app_data_dir(raw_app_data_dir);
        this->env->ReleaseStringUTFChars(args->app_data_dir, raw_app_data_dir);

        std::string module_dir = std::string("/data/local/tmp/re.zyg.fri");

        // 1. Ensure /data/local/tmp/re.zyg.fri directory exists (creates dynamic self-healing of missing directory)
        struct stat st;
        if (stat(module_dir.c_str(), &st) != 0) {
            if (mkdir(module_dir.c_str(), 0777) == 0) {
                chmod(module_dir.c_str(), 0777);
                chown(module_dir.c_str(), 2000, 2000); // shell:shell
                LOGI("Successfully created config directory at runtime: %s", module_dir.c_str());
            } else {
                LOGE("Failed to create config directory: %s", module_dir.c_str());
            }
        }

        // 2. Ensure config.json.example exists
        std::string example_config_path = module_dir + "/config.json.example";
        if (stat(example_config_path.c_str(), &st) != 0) {
            int module_dir_fd = api->getModuleDir();
            if (module_dir_fd >= 0) {
                if (copy_file_at(module_dir_fd, "config.json.example", example_config_path, 2000, 2000)) {
                    LOGI("Successfully copied config.json.example dynamically to %s", example_config_path.c_str());
                } else {
                    LOGE("Failed to copy config.json.example from module dir (fd: %d) to %s", module_dir_fd, example_config_path.c_str());
                }
            } else {
                LOGE("Could not obtain module directory fd to copy config.json.example");
            }
        }

        // 3. Localize libraries designed for injection to the target application's directory
        std::optional<target_config> cfg = load_config(module_dir, app_name);
        if (cfg.has_value() && cfg->enabled) {
            int uid = args->uid;
            int gid = args->gid;

            for (auto const &lib_path : cfg->injected_libraries) {
                localize_single_lib(lib_path, app_data_dir, uid, gid);
            }

            if (cfg->child_gating.enabled) {
                for (auto const &lib_path : cfg->child_gating.injected_libraries) {
                    localize_single_lib(lib_path, app_data_dir, uid, gid);
                }
            }
        }
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        const char *raw_app_name = env->GetStringUTFChars(args->nice_name, nullptr);
        std::string app_name = std::string(raw_app_name);
        this->env->ReleaseStringUTFChars(args->nice_name, raw_app_name);

        const char *raw_app_data_dir = env->GetStringUTFChars(args->app_data_dir, nullptr);
        std::string app_data_dir = std::string(raw_app_data_dir);
        this->env->ReleaseStringUTFChars(args->app_data_dir, raw_app_data_dir);

        if (!check_and_inject(app_name, app_data_dir)) {
            this->api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

 private:
    Api *api;
    JNIEnv *env;
};

REGISTER_ZYGISK_MODULE(MyModule)
