#include <string>
#include <vector>
#include <optional>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

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

static bool copy_config_and_rewrite(const std::string& src, const std::string& dst, const std::string& app_data_dir) {
    int src_fd = open(src.c_str(), O_RDONLY);
    if (src_fd < 0) return false;
    
    std::string content;
    char buffer[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        content.append(buffer, bytes_read);
    }
    close(src_fd);

    // Replace "/data/local/tmp/re.zyg.fri/" with app_data_dir + "/"
    std::string pattern = "/data/local/tmp/re.zyg.fri/";
    std::string replacement = app_data_dir + "/";
    size_t pos = 0;
    while ((pos = content.find(pattern, pos)) != std::string::npos) {
        content.replace(pos, pattern.length(), replacement);
        pos += replacement.length();
    }

    int dst_fd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) return false;

    ssize_t written = write(dst_fd, content.data(), content.size());
    close(dst_fd);

    return (written == static_cast<ssize_t>(content.size()));
}

static void localize_single_lib(const std::string& lib_path, const std::string& app_data_dir, int uid, int gid) {
    if (lib_path.rfind("/data/local/tmp/re.zyg.fri/", 0) == 0) {
        size_t last_slash = lib_path.find_last_of('/');
        std::string filename = (last_slash == std::string::npos) ? lib_path : lib_path.substr(last_slash + 1);
        
        // Use a dedicated subdirectory in app's cache for our localized files
        std::string localized_dir = app_data_dir + "/.zygisk_frida";
        LOGI("Localizing %s to %s", lib_path.c_str(), localized_dir.c_str());
        if (mkdir(localized_dir.c_str(), 0755) != 0 && errno != EEXIST) {
            LOGE("Failed to create localized directory %s: %s", localized_dir.c_str(), strerror(errno));
        } else {
            LOGI("Localized directory ready: %s", localized_dir.c_str());
        }
        if (chown(localized_dir.c_str(), uid, gid) != 0) {
            LOGW("Failed to chown localized directory %s to %d:%d: %s", localized_dir.c_str(), uid, gid, strerror(errno));
        }
        
        std::string localized_path = localized_dir + "/" + filename;

        struct stat st;
        if (stat(lib_path.c_str(), &st) == 0) {
            if (copy_file(lib_path, localized_path)) {
                chown(localized_path.c_str(), uid, gid);
                chmod(localized_path.c_str(), 0755);
                LOGI("Localized library successfully copied: %s -> %s (UID: %d, GID: %d)", lib_path.c_str(), localized_path.c_str(), uid, gid);

                // Auto-provision a gadget configuration
                if (lib_path.length() >= 3 && lib_path.substr(lib_path.length() - 3) == ".so") {
                    std::string base_name = filename.substr(0, filename.length() - 3);
                    std::string base_path = localized_dir + "/" + base_name;
                    
                    // We support multiple naming conventions for Frida Gadget configs
                    std::vector<std::string> config_dests = {
                        base_path + ".config.so",
                        base_path + ".so.config",
                        localized_dir + "/gadget.config"
                    };

                    std::string config_src = lib_path.substr(0, lib_path.length() - 3) + ".config.so";
                    bool config_provided = (stat(config_src.c_str(), &st) == 0);
                    
                    if (!config_provided) {
                        // try .so.config at source
                        config_src = lib_path + ".config";
                        config_provided = (stat(config_src.c_str(), &st) == 0);
                    }

                    if (config_provided) {
                        for (const auto& config_dst : config_dests) {
                            if (copy_config_and_rewrite(config_src, config_dst, localized_dir)) {
                                chown(config_dst.c_str(), uid, gid);
                                chmod(config_dst.c_str(), 0644);
                                LOGI("Provisoned config: %s -> %s", config_src.c_str(), config_dst.c_str());
                            }
                        }
                    } else {
                        // Create default configs pointing to localized script
                        std::string default_config = "{\n"
                                                     "  \"interaction\": {\n"
                                                     "    \"type\": \"script\",\n"
                                                     "    \"path\": \"" + localized_dir + "/script.js\"\n"
                                                     "  },\n"
                                                     "  \"logging\": {\n"
                                                     "    \"level\": \"info\",\n"
                                                     "    \"type\": \"logcat\"\n"
                                                     "  }\n"
                                                     "}";
                        for (const auto& config_dst : config_dests) {
                            int config_fd = open(config_dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                            if (config_fd >= 0) {
                                write(config_fd, default_config.c_str(), default_config.length());
                                close(config_fd);
                                chown(config_dst.c_str(), uid, gid);
                                LOGI("Created default config at %s", config_dst.c_str());
                            }
                        }
                    }
                }
            } else {
                LOGE("CRITICAL: Failed to copy library to app cache: %s -> %s (Errno: %d)", lib_path.c_str(), localized_path.c_str(), errno);
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
        if (!args->nice_name || !args->app_data_dir) {
            LOGW("preAppSpecialize: nice_name or app_data_dir is null, skipping localization check");
            return;
        }

        // Use the env provided in the args for the current thread/process context
        JNIEnv *env = args->env ? args->env : this->env;
        if (!env) {
            LOGE("preAppSpecialize: JNIEnv is null, cannot proceed");
            return;
        }

        const char *raw_app_name = env->GetStringUTFChars(args->nice_name, nullptr);
        if (!raw_app_name) return;
        this->app_name = std::string(raw_app_name);
        env->ReleaseStringUTFChars(args->nice_name, raw_app_name);

        const char *raw_app_data_dir = env->GetStringUTFChars(args->app_data_dir, nullptr);
        if (!raw_app_data_dir) return;
        this->app_data_dir = std::string(raw_app_data_dir);
        env->ReleaseStringUTFChars(args->app_data_dir, raw_app_data_dir);

        std::string app_name = this->app_name;
        std::string app_data_dir = this->app_data_dir;

        LOGI("preAppSpecialize for %s (UID: %d, Data Dir: %s)", app_name.c_str(), args->uid, app_data_dir.c_str());

        std::string module_dir = std::string("/data/local/tmp/re.zyg.fri");

        // 1. Ensure /data/local/tmp/re.zyg.fri directory exists (creates dynamic self-healing of missing directory)
        struct stat st;
        if (stat(module_dir.c_str(), &st) != 0 && errno == ENOENT) {
            if (mkdir(module_dir.c_str(), 0777) == 0) {
                chmod(module_dir.c_str(), 0777);
                chown(module_dir.c_str(), 2000, 2000); // shell:shell
                LOGI("Successfully created config directory at runtime: %s", module_dir.c_str());
            } else if (errno != EEXIST) {
                LOGE("Failed to create config directory: %s", module_dir.c_str());
            }
        }

        // 2. Ensure config.json.example and script.js exist in /data/local/tmp
        int module_dir_fd = api->getModuleDir();
        if (module_dir_fd >= 0) {
            std::string example_config_path = module_dir + "/config.json.example";
            if (stat(example_config_path.c_str(), &st) != 0 && errno == ENOENT) {
                if (copy_file_at(module_dir_fd, "config.json.example", example_config_path, 2000, 2000)) {
                    LOGI("Successfully copied config.json.example dynamically to %s", example_config_path.c_str());
                }
            }
            
            std::string script_template_path = module_dir + "/script.js";
            if (stat(script_template_path.c_str(), &st) != 0 && errno == ENOENT) {
                if (copy_file_at(module_dir_fd, "script.js", script_template_path, 2000, 2000)) {
                    LOGI("Successfully copied script.js template dynamically to %s", script_template_path.c_str());
                }
            }
        } else {
            LOGE("Could not obtain module directory fd to copy templates");
        }

        // 3. Localize libraries designed for injection to the target application's directory
        std::optional<target_config> cfg = load_config(module_dir, app_name);
        if (cfg.has_value() && cfg->enabled) {
            int uid = args->uid;
            int gid = args->gid;
            
            LOGI("Target detected: %s (Data dir: %s, UID: %d, GID: %d)", app_name.c_str(), app_data_dir.c_str(), uid, gid);

            // 4. Ensure script.js is copied/localized to app_data_dir for Frida script interaction
            std::string script_src = module_dir + "/script.js";
            std::string localized_dir = app_data_dir + "/.zygisk_frida";
            if (mkdir(localized_dir.c_str(), 0755) != 0 && errno != EEXIST) {
                LOGE("Failed to create localized directory for script %s: %s", localized_dir.c_str(), strerror(errno));
            }
            if (chown(localized_dir.c_str(), uid, gid) != 0) {
                LOGW("Failed to chown localized directory for script %s: %s", localized_dir.c_str(), strerror(errno));
            }
            
            std::string script_dst = localized_dir + "/script.js";
            struct stat script_st;
            if (stat(script_src.c_str(), &script_st) == 0) {
                if (copy_file(script_src, script_dst)) {
                    chown(script_dst.c_str(), uid, gid);
                    chmod(script_dst.c_str(), 0644);
                    LOGI("Localized script.js copied: %s -> %s (UID: %d, GID: %d)", script_src.c_str(), script_dst.c_str(), uid, gid);
                } else {
                    LOGE("Failed to copy script.js to app cache at %s", script_dst.c_str());
                }
            } else {
                LOGW("Source script.js not found at %s", script_src.c_str());
            }

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
        if (this->app_name.empty() || this->app_data_dir.empty()) {
            this->api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        if (!check_and_inject(this->app_name, this->app_data_dir)) {
            this->api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

 private:
    Api *api;
    JNIEnv *env;
    std::string app_name;
    std::string app_data_dir;
};

REGISTER_ZYGISK_MODULE(MyModule)
