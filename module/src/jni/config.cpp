#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
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

static int deserialize_child_gating_config(const rapidjson::Value &doc, struct child_gating_config *out) {
    if (!doc.IsObject()) {
        return 0;
    }

    memset(out, 0, sizeof(*out));

    const rapidjson::Value &enabled = doc["enabled"];
    if (!enabled.IsBool()) {
        LOGE("invalid config: expected child_gating.enabled members to be a bool");
        return 0;
    }
    out->enabled = enabled.GetBool() ? 1 : 0;

    const rapidjson::Value &mode = doc["mode"];
    if (!mode.IsString()) {
        LOGE("invalid config: expected child_gating.mode members to be a string");
        return 0;
    }
    copy_str(out->mode, ZF_MAX_MODE, mode.GetString());

    if (doc.HasMember("injected_libraries")) {
        if (!deserialize_libraries(doc["injected_libraries"],
                                   out->injected_libraries,
                                   &out->injected_libraries_count)) {
            return 0;
        }
    }

    return 1;
}

static int deserialize_target_config(const rapidjson::Value &doc, struct target_config *out) {
    if (!doc.IsObject()) {
        LOGE("expected config targets array to contain objects");
        return 0;
    }

    memset(out, 0, sizeof(*out));

    const rapidjson::Value &app_name = doc["app_name"];
    if (!app_name.IsString()) {
        LOGE("expected config target to have a valid app_name");
        return 0;
    }
    copy_str(out->app_name, ZF_MAX_PATH, app_name.GetString());

    const rapidjson::Value &enabled = doc["enabled"];
    if (!enabled.IsBool()) {
        LOGE("invalid config: expected targets.enabled members to be a bool");
        return 0;
    }
    out->enabled = enabled.GetBool() ? 1 : 0;

    const rapidjson::Value &start_up_delay_ms = doc["start_up_delay_ms"];
    if (!start_up_delay_ms.IsUint64()) {
        LOGE("expected config target start_up_delay_ms to be an uint64");
        return 0;
    }
    out->start_up_delay_ms = start_up_delay_ms.GetUint64();

    const rapidjson::Value &injected_libraries = doc["injected_libraries"];
    if (!deserialize_libraries(injected_libraries,
                               out->injected_libraries,
                               &out->injected_libraries_count)) {
        return 0;
    }

    if (doc.HasMember("child_gating")) {
        if (!deserialize_child_gating_config(doc["child_gating"], &out->child_gating)) {
            return 0;
        }
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
        copy_str(libraries[0], ZF_MAX_PATH, "");
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

        memset(out, 0, sizeof(*out));
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
        return 1;
    }

    return load_simple_config(module_dir, app_name, out);
}
