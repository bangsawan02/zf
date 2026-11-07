#include "config.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "log.h"

// This must work in a non-exception environment as the libcxx we use don't have support for exception.
// Should avoid any libraries aborting on parse error as this will run on every app start and
// might otherwise cause issues for any app starting if misconfigured.
//
// The code intentionally avoids std::string / std::vector / std::optional. RapidJSON is
// header-only and only uses malloc/free here, so this translation unit emits no libc++
// runtime imports.

static void copy_str(char *dst, size_t dst_size, const char *src) {
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

void target_config_set_defaults(struct target_config *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->enabled = 1;
    out->hot_reload = 1;
    out->reload_interval_ms = 1500;
    // Stealth is OFF by default. Remapping a live gadget (mremap over its
    // executable segments while gum threads are running) and deleting the
    // payload so the next process has nothing to inject were the two ways the
    // old defaults killed hardened targets. Opt in explicitly per target.
    out->stealth.enabled = 0;
    out->stealth.remap = 0;
    out->stealth.rename_threads = 0;
    out->stealth.unlink_payload = 0;
    out->stealth.remap_delay_ms = 0;
    copy_str(out->stealth.thread_alias, ZF_MAX_THREAD_ALIAS, "Jit thread pool");
}

static char *read_entire_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    if (size > (1024L * 1024L)) {
        LOGE("config file too large (%ld bytes); refusing to load", size);
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *buffer = (char *) malloc((size_t) size + 1);
    if (buffer == NULL) {
        fclose(fp);
        return NULL;
    }

    size_t read_bytes = fread(buffer, 1, (size_t) size, fp);
    fclose(fp);
    buffer[read_bytes] = '\0';

    if (out_len != NULL) {
        *out_len = read_bytes;
    }
    return buffer;
}

static int deserialize_libraries(const rapidjson::Value &doc,
                                 char libraries[ZF_MAX_LIBS][ZF_MAX_PATH],
                                 int *count) {
    if (!doc.IsArray()) {
        LOGE("invalid config: expected injected_libraries to be an array");
        return 0;
    }

    int n = 0;
    for (rapidjson::SizeType i = 0; i < doc.Size(); i++) {
        const rapidjson::Value &library = doc[i];
        if (!library.IsObject()) {
            LOGE("invalid config: expected injected_libraries members to be objects");
            return 0;
        }

        const rapidjson::Value &path = library["path"];
        if (!path.IsString()) {
            LOGE("invalid config: expected injected_libraries.path to be a string");
            return 0;
        }

        if (n >= ZF_MAX_LIBS) {
            LOGE("invalid config: too many injected_libraries (max %d)", ZF_MAX_LIBS);
            return 0;
        }

        copy_str(libraries[n], ZF_MAX_PATH, path.GetString());
        n++;
    }

    *count = n;
    return 1;
}

// ---------------------------------------------------------------------------
// Config v2: "defaults" + "profiles" + "targets", with wildcard app_name.
//
// Precedence per field (weakest to strongest): defaults -> profile -> target.
//   - scalar fields (enabled, start_up_delay_ms, mode): last write wins
//   - injected_libraries arrays: replaced wholesale
//   - child_gating: merged field-by-field
//   - "app_name" supports '*' (any sequence) and '?' (single char)
// ---------------------------------------------------------------------------

static bool glob_match(const char *pattern, const char *str) {
    const char *p = pattern, *s = str;
    const char *star_p = NULL, *star_s = NULL;
    while (*s != '\0') {
        if (*p == '?' || *p == *s) {
            p++;
            s++;
            continue;
        }
        if (*p == '*') {
            star_p = p++;
            star_s = s;
            continue;
        }
        if (star_p != NULL) {
            p = star_p + 1;
            s = ++star_s;
            continue;
        }
        return false;
    }
    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}

static int merge_child_gating(struct child_gating_config *out,
                              const rapidjson::Value &doc,
                              const char *context) {
    if (!doc.IsObject()) {
        LOGE("%s: expected child_gating to be an object", context);
        return 0;
    }

    if (doc.HasMember("enabled")) {
        const rapidjson::Value &v = doc["enabled"];
        if (!v.IsBool()) {
            LOGE("%s: expected child_gating.enabled to be a bool", context);
            return 0;
        }
        out->enabled = v.GetBool() ? 1 : 0;
    }
    if (doc.HasMember("mode")) {
        const rapidjson::Value &v = doc["mode"];
        if (!v.IsString()) {
            LOGE("%s: expected child_gating.mode to be a string", context);
            return 0;
        }
        copy_str(out->mode, ZF_MAX_MODE, v.GetString());
    }
    if (doc.HasMember("injected_libraries")) {
        char libs[ZF_MAX_LIBS][ZF_MAX_PATH];
        int count = 0;
        if (!deserialize_libraries(doc["injected_libraries"], libs, &count)) {
            return 0;
        }
        memcpy(out->injected_libraries, libs, sizeof(libs));
        out->injected_libraries_count = count;
    }
    static const char *const known[] = {"enabled", "mode", "injected_libraries"};
    for (rapidjson::Value::ConstMemberIterator it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        bool found = false;
        const char *name = it->name.GetString();
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
            if (strcmp(name, known[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            LOGW("%s: ignoring unknown child_gating key \"%s\"", context, name);
        }
    }
    return 1;
}

static int merge_stealth(struct stealth_config *out,
                         const rapidjson::Value &doc,
                         const char *context) {
    if (!doc.IsObject()) {
        LOGE("%s: expected stealth to be an object", context);
        return 0;
    }

    static const char *const known[] = {"enabled", "remap", "rename_threads",
                                        "unlink_payload", "remap_delay_ms",
                                        "thread_alias"};
    for (rapidjson::Value::ConstMemberIterator it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        bool found = false;
        const char *name = it->name.GetString();
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
            if (strcmp(name, known[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            LOGW("%s: ignoring unknown stealth key \"%s\"", context, name);
        }
    }

    struct {
        const char *name;
        int *target;
    } const bools[] = {
        {"enabled", &out->enabled},
        {"remap", &out->remap},
        {"rename_threads", &out->rename_threads},
        {"unlink_payload", &out->unlink_payload},
    };
    for (size_t i = 0; i < sizeof(bools) / sizeof(bools[0]); i++) {
        if (!doc.HasMember(bools[i].name)) {
            continue;
        }
        const rapidjson::Value &v = doc[bools[i].name];
        if (!v.IsBool()) {
            LOGE("%s: expected stealth.%s to be a bool", context, bools[i].name);
            return 0;
        }
        *bools[i].target = v.GetBool() ? 1 : 0;
    }

    if (doc.HasMember("remap_delay_ms")) {
        const rapidjson::Value &v = doc["remap_delay_ms"];
        if (!v.IsUint64()) {
            LOGE("%s: expected stealth.remap_delay_ms to be an uint64", context);
            return 0;
        }
        out->remap_delay_ms = v.GetUint64();
    }
    if (doc.HasMember("thread_alias")) {
        const rapidjson::Value &v = doc["thread_alias"];
        if (!v.IsString()) {
            LOGE("%s: expected stealth.thread_alias to be a string", context);
            return 0;
        }
        copy_str(out->thread_alias, ZF_MAX_THREAD_ALIAS, v.GetString());
    }

    return 1;
}

static int merge_target_partial(struct target_config *out,
                                const rapidjson::Value &doc,
                                const char *context) {
    if (!doc.IsObject()) {
        LOGE("%s: expected an object", context);
        return 0;
    }

    static const char *const known[] = {"app_name", "enabled", "start_up_delay_ms",
                                        "injected_libraries", "child_gating", "profile",
                                        "extends", "hot_reload", "reload_interval_ms",
                                        "stealth"};
    for (rapidjson::Value::ConstMemberIterator it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        const char *name = it->name.GetString();
        bool found = false;
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
            if (strcmp(name, known[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            LOGW("%s: ignoring unknown key \"%s\"", context, name);
        }
    }

    if (doc.HasMember("enabled")) {
        const rapidjson::Value &v = doc["enabled"];
        if (!v.IsBool()) {
            LOGE("%s: expected enabled to be a bool", context);
            return 0;
        }
        out->enabled = v.GetBool() ? 1 : 0;
    }
    if (doc.HasMember("start_up_delay_ms")) {
        const rapidjson::Value &v = doc["start_up_delay_ms"];
        if (!v.IsUint64()) {
            LOGE("%s: expected start_up_delay_ms to be an uint64", context);
            return 0;
        }
        out->start_up_delay_ms = v.GetUint64();
    }
    if (doc.HasMember("injected_libraries")) {
        if (!deserialize_libraries(doc["injected_libraries"],
                                   out->injected_libraries,
                                   &out->injected_libraries_count)) {
            return 0;
        }
    }
    if (doc.HasMember("child_gating")) {
        if (!merge_child_gating(&out->child_gating, doc["child_gating"], context)) {
            return 0;
        }
    }
    if (doc.HasMember("hot_reload")) {
        const rapidjson::Value &v = doc["hot_reload"];
        if (!v.IsBool()) {
            LOGE("%s: expected hot_reload to be a bool", context);
            return 0;
        }
        out->hot_reload = v.GetBool() ? 1 : 0;
    }
    if (doc.HasMember("reload_interval_ms")) {
        const rapidjson::Value &v = doc["reload_interval_ms"];
        if (!v.IsUint64()) {
            LOGE("%s: expected reload_interval_ms to be an uint64", context);
            return 0;
        }
        out->reload_interval_ms = v.GetUint64();
    }
    if (doc.HasMember("stealth")) {
        if (!merge_stealth(&out->stealth, doc["stealth"], context)) {
            return 0;
        }
    }
    return 1;
}

#define ZF_MAX_PROFILE_DEPTH 12

static int apply_profile(struct target_config *out,
                         const rapidjson::Value *profiles,
                         const char *name,
                         int depth,
                         const char *context) {
    if (depth > ZF_MAX_PROFILE_DEPTH) {
        LOGE("%s: profile inheritance too deep or cyclic at \"%s\"", context, name);
        return 0;
    }
    const rapidjson::Value *profile = NULL;
    if (profiles != NULL && profiles->HasMember(name)) {
        profile = &(*profiles)[name];
    }
    if (profile == NULL) {
        LOGE("%s: profile \"%s\" not found", context, name);
        return 0;
    }
    if (!profile->IsObject()) {
        LOGE("%s: profile \"%s\" is not an object", context, name);
        return 0;
    }

    // A profile may inherit from one profile (string) or several (array).
    if (profile->HasMember("extends")) {
        const rapidjson::Value &parent = (*profile)["extends"];
        if (parent.IsString()) {
            if (!apply_profile(out, profiles, parent.GetString(), depth + 1, context)) {
                return 0;
            }
        } else if (parent.IsArray()) {
            for (rapidjson::SizeType i = 0; i < parent.Size(); i++) {
                if (!parent[i].IsString()) {
                    LOGE("%s: profile \"%s\" extends[%u] is not a string", context, name, (unsigned) i);
                    return 0;
                }
                if (!apply_profile(out, profiles, parent[i].GetString(), depth + 1, context)) {
                    return 0;
                }
            }
        } else {
            LOGE("%s: profile \"%s\" extends must be a string or an array", context, name);
            return 0;
        }
    }

    return merge_target_partial(out, *profile, context);
}

// Returns 1 when the pattern matched, 0 when it did not, -1 on config error.
// `entry` may be NULL (array entry described by app_name), a bool, a profile
// name (string) or an object with per-target overrides.
static int resolve_target(const char *pattern,
                          const rapidjson::Value *entry,
                          const rapidjson::Value *defaults,
                          const rapidjson::Value *profiles,
                          const char *app_name,
                          struct target_config *out) {
    if (!glob_match(pattern, app_name)) {
        return 0;
    }

    target_config_set_defaults(out);
    copy_str(out->app_name, ZF_MAX_PATH, app_name);

    if (defaults != NULL && !merge_target_partial(out, *defaults, "defaults")) {
        return -1;
    }

    if (entry == NULL) {
        return 1;
    }
    if (entry->IsBool()) {
        if (!entry->GetBool()) {
            out->enabled = 0;
        }
        return 1;
    }
    if (entry->IsString()) {
        return apply_profile(out, profiles, entry->GetString(), 0, "profile") ? 1 : -1;
    }
    if (!entry->IsObject()) {
        LOGE("config: target \"%s\" must be a bool, a profile name or an object", pattern);
        return -1;
    }

    if (entry->HasMember("profile")) {
        const rapidjson::Value &profile = (*entry)["profile"];
        if (!profile.IsString()) {
            LOGE("config: target \"%s\".profile is not a string", pattern);
            return -1;
        }
        if (!apply_profile(out, profiles, profile.GetString(), 0, "profile")) {
            return -1;
        }
    }

    if (!merge_target_partial(out, *entry, "target")) {
        return -1;
    }

    // Object-form targets usually omit app_name because the key is the pattern,
    // but an explicit app_name inside the object is harmless.
    return 1;
}

static int load_advanced_config_v2(const rapidjson::Document &doc,
                                   const char *app_name,
                                   struct target_config *out) {
    const rapidjson::Value &targets = doc["targets"];  // caller checked
    if (!targets.IsArray() && !targets.IsObject()) {
        LOGE("expected config targets to be an array or an object map");
        return 0;
    }

    // Optional denylist: excluded packages never reach target matching, even
    // when a catch-all wildcard target would match them.
    if (doc.HasMember("exclude")) {
        const rapidjson::Value &exclude = doc["exclude"];
        if (!exclude.IsArray()) {
            LOGE("expected config exclude to be an array");
            return 0;
        }
        for (rapidjson::SizeType i = 0; i < exclude.Size(); i++) {
            if (!exclude[i].IsString()) {
                LOGE("expected config exclude[%u] to be a string", (unsigned) i);
                return 0;
            }
            if (glob_match(exclude[i].GetString(), app_name)) {
                LOGI("config v2: %s is excluded by pattern \"%s\"", app_name, exclude[i].GetString());
                return 0;
            }
        }
    }

    const rapidjson::Value *defaults = NULL;
    if (doc.HasMember("defaults")) {
        if (!doc["defaults"].IsObject()) {
            LOGE("expected config defaults to be an object");
            return 0;
        }
        defaults = &doc["defaults"];
    }

    const rapidjson::Value *profiles = NULL;
    if (doc.HasMember("profiles")) {
        if (!doc["profiles"].IsObject()) {
            LOGE("expected config profiles to be an object");
            return 0;
        }
        profiles = &doc["profiles"];
    }

    if (targets.IsObject()) {
        // Object form is the shortest way to describe many apps:
        //   "targets": { "com.foo": "hook", "com.bar": { ... }, "com.baz": true }
        for (rapidjson::Value::ConstMemberIterator it = targets.MemberBegin();
             it != targets.MemberEnd(); ++it) {
            const char *key = it->name.GetString();
            const rapidjson::Value &value = it->value;

            // An object value may override the key with one or more patterns.
            if (value.IsObject() && value.HasMember("app_name")) {
                const rapidjson::Value &names = value["app_name"];
                if (names.IsString()) {
                    int result = resolve_target(names.GetString(), &value, defaults, profiles, app_name, out);
                    if (result < 0) {
                        return 0;
                    }
                    if (result == 1) {
                        LOGI("config v2: matched target \"%s\" for app %s (enabled=%d)",
                             names.GetString(), app_name, out->enabled);
                        return 1;
                    }
                    continue;
                }
                if (!names.IsArray()) {
                    LOGE("invalid config: target \"%s\".app_name must be a string or an array", key);
                    return 0;
                }
                for (rapidjson::SizeType n = 0; n < names.Size(); n++) {
                    if (!names[n].IsString()) {
                        LOGE("invalid config: target \"%s\".app_name[%u] is not a string",
                             key, (unsigned) n);
                        return 0;
                    }
                    const char *pattern = names[n].GetString();
                    int result = resolve_target(pattern, &value, defaults, profiles, app_name, out);
                    if (result < 0) {
                        return 0;
                    }
                    if (result == 1) {
                        LOGI("config v2: matched target \"%s\" for app %s (enabled=%d)",
                             pattern, app_name, out->enabled);
                        return 1;
                    }
                }
                continue;
            }

            int result = resolve_target(key, &value, defaults, profiles, app_name, out);
            if (result < 0) {
                return 0;
            }
            if (result == 1) {
                LOGI("config v2: matched target \"%s\" for app %s (enabled=%d)",
                     key, app_name, out->enabled);
                return 1;
            }
        }
        return 0;
    }

    for (rapidjson::SizeType i = 0; i < targets.Size(); i++) {
        const rapidjson::Value &target = targets[i];

        // Shorthand: a bare string entry means "use defaults for this package".
        if (target.IsString()) {
            int result = resolve_target(target.GetString(), NULL, defaults, profiles, app_name, out);
            if (result < 0) {
                return 0;
            }
            if (result == 1) {
                LOGI("config v2: matched target \"%s\" for app %s (enabled=%d)",
                     target.GetString(), app_name, out->enabled);
                return 1;
            }
            continue;
        }

        if (!target.IsObject()) {
            LOGE("invalid config: targets[%u] is not an object or a package string; skipping",
                 (unsigned) i);
            continue;
        }

        const rapidjson::Value *names = NULL;
        if (target.HasMember("app_name")) {
            names = &target["app_name"];
        }
        if (names == NULL) {
            LOGE("invalid config: targets[%u] has no app_name; skipping", (unsigned) i);
            continue;
        }

        if (names->IsString()) {
            int result = resolve_target(names->GetString(), &target, defaults, profiles, app_name, out);
            if (result < 0) {
                return 0;
            }
            if (result == 1) {
                LOGI("config v2: matched target \"%s\" for app %s (enabled=%d)",
                     names->GetString(), app_name, out->enabled);
                return 1;
            }
            continue;
        }

        if (!names->IsArray()) {
            LOGE("invalid config: targets[%u].app_name must be a string or an array", (unsigned) i);
            continue;
        }

        // One target entry may cover a whole family of packages:
        //   "app_name": ["com.foo", "com.foo:*", "com.bar"]
        for (rapidjson::SizeType n = 0; n < names->Size(); n++) {
            if (!(*names)[n].IsString()) {
                LOGE("invalid config: targets[%u].app_name[%u] is not a string",
                     (unsigned) i, (unsigned) n);
                return 0;
            }
            const char *pattern = (*names)[n].GetString();
            int result = resolve_target(pattern, &target, defaults, profiles, app_name, out);
            if (result < 0) {
                return 0;
            }
            if (result == 1) {
                LOGI("config v2: matched target \"%s\" for app %s (enabled=%d)",
                     pattern, app_name, out->enabled);
                return 1;
            }
        }
    }

    return 0;
}

static int deserialize_target_config(const rapidjson::Value &doc, struct target_config *out) {
    if (!doc.IsObject()) {
        LOGE("expected config targets array to contain objects");
        return 0;
    }

    target_config_set_defaults(out);

    const rapidjson::Value &app_name = doc["app_name"];
    if (!app_name.IsString()) {
        LOGE("expected config target to have a valid app_name");
        return 0;
    }
    copy_str(out->app_name, ZF_MAX_PATH, app_name.GetString());

    // v1 targets used to be fully explicit; going through the partial merge
    // keeps them working while allowing the new hot_reload/stealth keys.
    if (!merge_target_partial(out, doc, "target")) {
        return 0;
    }

    return 1;
}

static void trim_line(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }

    char *start = line;
    while (*start == ' ' || *start == '\t') {
        start++;
    }

    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }

    if (start != line) {
        memmove(line, start, strlen(start) + 1);
    }
}

static void parse_injected_libraries(const char *module_dir,
                                     char libraries[ZF_MAX_LIBS][ZF_MAX_PATH],
                                     int *count) {
    char config_file_path[ZF_MAX_PATH * 2];
    snprintf(config_file_path, sizeof(config_file_path), "%s/injected_libraries", module_dir);

    FILE *config_file = fopen(config_file_path, "r");
    if (config_file == NULL) {
        snprintf(libraries[0], ZF_MAX_PATH, "%s/libgadget.so", module_dir);
        *count = 1;
        return;
    }

    int n = 0;
    char line[ZF_MAX_PATH * 2];
    while (fgets(line, sizeof(line), config_file) != NULL) {
        trim_line(line);
        if (line[0] == '\0') {
            continue;
        }
        if (n >= ZF_MAX_LIBS) {
            LOGE("invalid injected_libraries: too many entries (max %d)", ZF_MAX_LIBS);
            break;
        }
        copy_str(libraries[n], ZF_MAX_PATH, line);
        n++;
    }

    fclose(config_file);
    *count = n;
}

static int load_simple_config(const char *module_dir, const char *app_name, struct target_config *out) {
    char config_file_path[ZF_MAX_PATH * 2];
    snprintf(config_file_path, sizeof(config_file_path), "%s/target_packages", module_dir);

    FILE *config_file = fopen(config_file_path, "r");
    if (config_file == NULL) {
        return 0;
    }

    char line[ZF_MAX_PATH * 2];
    while (fgets(line, sizeof(line), config_file) != NULL) {
        trim_line(line);
        if (line[0] == '\0') {
            continue;
        }

        char *comma = strchr(line, ',');
        char *delay = NULL;
        if (comma != NULL) {
            *comma = '\0';
            delay = comma + 1;
        }
        trim_line(line);
        if (delay != NULL) {
            trim_line(delay);
        }

        if (strcmp(line, app_name) != 0) {
            continue;
        }

        target_config_set_defaults(out);
        copy_str(out->app_name, ZF_MAX_PATH, line);
        out->enabled = 1;
        if (delay != NULL && delay[0] != '\0') {
            out->start_up_delay_ms = strtoull(delay, NULL, 10);
        }
        parse_injected_libraries(module_dir, out->injected_libraries, &out->injected_libraries_count);

        fclose(config_file);
        return 1;
    }

    fclose(config_file);
    return 0;
}

static int load_advanced_config(const char *module_dir, const char *app_name, struct target_config *out) {
    char config_file_path[ZF_MAX_PATH * 2];
    snprintf(config_file_path, sizeof(config_file_path), "%s/config.json", module_dir);

    size_t length = 0;
    char *data = read_entire_file(config_file_path, &length);
    if (data == NULL) {
        return 0;
    }

    rapidjson::Document doc;
    doc.Parse(data, length);

    if (doc.HasParseError()) {
        LOGE("config is not a valid json file offset %u: %s",
             (unsigned) doc.GetErrorOffset(),
             GetParseError_En(doc.GetParseError()));
        free(data);
        return 0;
    }

    if (!doc.IsObject()) {
        LOGE("config expected a json root object");
        free(data);
        return 0;
    }

    if (!doc.HasMember("targets")) {
        LOGE("expected config root to have a targets array");
        free(data);
        return 0;
    }

    if (doc.HasMember("version")) {
        if (doc["version"].IsUint() && doc["version"].GetUint() == 2) {
            LOGI("config v2 selected (defaults/profiles/targets)");
            int found = load_advanced_config_v2(doc, app_name, out);
            free(data);
            return found;
        }
        LOGE("unsupported config version (expected 2); advanced config ignored");
        free(data);
        return 0;
    }

    const rapidjson::Value &targets = doc["targets"];
    if (!targets.IsArray()) {
        LOGE("expected config targets to be an array");
        free(data);
        return 0;
    }

    int found = 0;
    for (rapidjson::SizeType i = 0; i < targets.Size(); i++) {
        struct target_config target;
        if (!deserialize_target_config(targets[i], &target)) {
            // Match upstream behavior: any malformed target invalidates the
            // advanced config, then the simple config may be used.
            free(data);
            return 0;
        }

        if (strcmp(target.app_name, app_name) == 0) {
            *out = target;
            found = 1;
            break;
        }
    }

    free(data);
    return found;
}

int load_config(const char *module_dir, const char *app_name, struct target_config *out) {
    if (module_dir == NULL || app_name == NULL || out == NULL) {
        return 0;
    }

    if (load_advanced_config(module_dir, app_name, out)) {
        LOGI("config loaded from advanced config.json");
        return 1;
    }

    if (load_simple_config(module_dir, app_name, out)) {
        LOGI("config loaded from simple target_packages");
        return 1;
    }

    LOGI("no target config found for %s", app_name);
    return 0;
}

// ---------------------------------------------------------------------------
// Hot reload support
//
// When the gadget config uses `"interaction": {"type": "script"}`, the module
// replaces the configured script path with a generated `hotreload.js` that
// watches the real script and re-evaluates it in place. Editing script.js is
// then enough: no app restart, no re-attach.
// ---------------------------------------------------------------------------

static const char *const ZF_HOTRELOAD_JS =
    "// Auto-generated by ZygiskFrida - DO NOT EDIT (edit script.js instead).\n"
    "(function () {\n"
    "    var TARGET = \"%s\";\n"
    "    var INTERVAL = %llu;\n"
    "    var lastSource = null;\n"
    "    function log(msg) { console.log(\"[zf-hotreload] \" + msg); }\n"
    "    function unloadPrevious() {\n"
    "        try {\n"
    "            if (typeof globalThis.__zfOnUnload === \"function\") globalThis.__zfOnUnload();\n"
    "        } catch (e) { log(\"__zfOnUnload failed: \" + e); }\n"
    "        try { Interceptor.detachAll(); } catch (e) { log(\"detachAll failed: \" + e); }\n"
    "    }\n"
    "    function evaluate(source) {\n"
    "        unloadPrevious();\n"
    "        try {\n"
    "            Script.evaluate(TARGET, source);\n"
    "            return true;\n"
    "        } catch (e) {\n"
    "            log(\"evaluate failed (\" + e + \"); retrying as ES module\");\n"
    "            try {\n"
    "                Script.load(TARGET, source).catch(function (err) { log(\"module load failed: \" + err); });\n"
    "                return true;\n"
    "            } catch (e2) {\n"
    "                log(\"module load failed: \" + e2);\n"
    "                return false;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    function poll() {\n"
    "        var source;\n"
    "        try { source = File.readAllText(TARGET); } catch (e) { return; }\n"
    "        if (source === lastSource) return;\n"
    "        lastSource = source;\n"
    "        log(\"loading \" + TARGET + \" (\" + source.length + \" chars)\");\n"
    "        evaluate(source);\n"
    "    }\n"
    "    log(\"watching \" + TARGET + \" every \" + INTERVAL + \"ms\");\n"
    "    poll();\n"
    "    setInterval(poll, INTERVAL);\n"
    "})();\n";

static int write_whole_file(const char *path, const char *data, size_t length,
                            int uid, int gid) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        LOGE("hot reload: cannot write %s: %s", path, strerror(errno));
        return 0;
    }

    size_t written_total = 0;
    while (written_total < length) {
        ssize_t written = write(fd, data + written_total, length - written_total);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            LOGE("hot reload: write failed for %s: %s", path, strerror(errno));
            return 0;
        }
        written_total += (size_t) written;
    }
    close(fd);

    if (uid != -1 && gid != -1) {
        chown(path, uid, gid);
    }
    chmod(path, 0644);
    return 1;
}

int config_rewrite_for_hot_reload(const char *config_path,
                                  const char *localized_dir,
                                  uint64_t interval_ms,
                                  int uid,
                                  int gid) {
    size_t length = 0;
    char *data = read_entire_file(config_path, &length);
    if (data == NULL) {
        return 0;
    }

    rapidjson::Document doc;
    doc.Parse(data, length);
    free(data);

    if (doc.HasParseError() || !doc.IsObject()) {
        return 0;
    }
    if (!doc.HasMember("interaction") || !doc["interaction"].IsObject()) {
        return 0;
    }

    rapidjson::Value &interaction = doc["interaction"];
    if (!interaction.HasMember("type") || !interaction["type"].IsString()) {
        return 0;
    }
    if (strcmp(interaction["type"].GetString(), "script") != 0) {
        return 0;
    }
    if (!interaction.HasMember("path") || !interaction["path"].IsString()) {
        return 0;
    }

    const char *script_path = interaction["path"].GetString();
    if (strstr(script_path, "hotreload.js") != NULL) {
        return 1;  // already installed
    }

    char wrapper_path[ZF_MAX_PATH * 4];
    snprintf(wrapper_path, sizeof(wrapper_path), "%s/hotreload.js", localized_dir);

    char wrapper[8192];
    int wrapper_len = snprintf(wrapper, sizeof(wrapper), ZF_HOTRELOAD_JS,
                               script_path, (unsigned long long) interval_ms);
    if (wrapper_len <= 0 || (size_t) wrapper_len >= sizeof(wrapper)) {
        LOGE("hot reload: generated script too large");
        return 0;
    }

    if (!write_whole_file(wrapper_path, wrapper, (size_t) wrapper_len, uid, gid)) {
        return 0;
    }

    interaction["path"].SetString(wrapper_path, doc.GetAllocator());

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    if (!doc.Accept(writer)) {
        LOGE("hot reload: cannot serialize %s", config_path);
        return 0;
    }

    if (!write_whole_file(config_path, buffer.GetString(), buffer.GetSize(), uid, gid)) {
        return 0;
    }

    LOGI("hot reload: %s now loads through %s (interval %llu ms)",
         config_path, wrapper_path, (unsigned long long) interval_ms);
    return 1;
}
