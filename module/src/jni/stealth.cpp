#include "stealth.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

// Plain C only: this translation unit must not pull in libc++.

#define ZF_MAPS_MAX_SIZE (8 * 1024 * 1024)
#define ZF_MAPS_MAX_SEGMENTS 256

struct map_segment {
    unsigned long start;
    unsigned long end;
    int prot;
};

static int prot_from_perms(const char *perms) {
    int prot = PROT_NONE;
    if (strchr(perms, 'r') != NULL) {
        prot |= PROT_READ;
    }
    if (strchr(perms, 'w') != NULL) {
        prot |= PROT_WRITE;
    }
    if (strchr(perms, 'x') != NULL) {
        prot |= PROT_EXEC;
    }
    return prot;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static int name_equals(const char *path, const char *name) {
    char trimmed[ZF_MAX_PATH];
    snprintf(trimmed, sizeof(trimmed), "%s", path);

    // A deleted file still shows up as "/path/libfoo.so (deleted)".
    char *deleted = strstr(trimmed, " (deleted)");
    if (deleted != NULL) {
        *deleted = '\0';
    }

    return strcmp(base_name(trimmed), name) == 0;
}

static char *read_text_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }

    char *buffer = (char *) malloc(ZF_MAPS_MAX_SIZE + 1);
    if (buffer == NULL) {
        close(fd);
        return NULL;
    }

    size_t total = 0;
    for (;;) {
        if (total >= ZF_MAPS_MAX_SIZE) {
            break;
        }
        ssize_t n = read(fd, buffer + total, ZF_MAPS_MAX_SIZE - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        total += (size_t) n;
    }
    close(fd);
    buffer[total] = '\0';
    return buffer;
}

static int remap_segment(struct map_segment *seg) {
    size_t size = (size_t) (seg->end - seg->start);
    void *source = (void *) seg->start;
    if (size == 0) {
        return 1;
    }

    int prot = seg->prot;
    if ((prot & PROT_READ) == 0) {
        // Need a readable view to copy from. Execute-only mappings (rare for a
        // dlopen'ed payload) are skipped rather than risking a fault.
        if (mprotect(source, size, PROT_READ) != 0) {
            LOGW("stealth: cannot read segment %p-%p (%s); skipping",
                 (void *) seg->start, (void *) seg->end, strerror(errno));
            return 0;
        }
    }

    void *copy = mmap(NULL, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (copy == MAP_FAILED) {
        LOGE("stealth: anonymous mmap failed for %zu bytes: %s", size, strerror(errno));
        return 0;
    }

    memcpy(copy, source, size);

    if (mprotect(copy, size, prot) != 0) {
        LOGE("stealth: mprotect failed: %s", strerror(errno));
        munmap(copy, size);
        return 0;
    }

    // Atomically replace the file-backed mapping with the anonymous copy. The
    // kernel performs the unmapping and the remap under a single mmap_lock, so
    // concurrent threads only observe the old or the new mapping.
    void *result = mremap(copy, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, source);
    if (result == MAP_FAILED) {
        LOGE("stealth: mremap failed: %s", strerror(errno));
        munmap(copy, size);
        return 0;
    }

    return 1;
}

static void remap_library(const char *lib_path) {
    const char *name = base_name(lib_path);
    if (name[0] == '\0') {
        return;
    }

    char *maps = read_text_file("/proc/self/maps");
    if (maps == NULL) {
        LOGE("stealth: cannot read /proc/self/maps: %s", strerror(errno));
        return;
    }

    struct map_segment segments[ZF_MAPS_MAX_SEGMENTS];
    int segment_count = 0;

    char *cursor = maps;
    while (cursor != NULL && *cursor != '\0') {
        char *line_end = strchr(cursor, '\n');
        if (line_end != NULL) {
            *line_end = '\0';
        }

        unsigned long start = 0;
        unsigned long end = 0;
        unsigned long offset = 0;
        unsigned long inode = 0;
        char perms[8] = {0};
        char dev[16] = {0};
        char path[ZF_MAX_PATH] = {0};

        int fields = sscanf(cursor, "%lx-%lx %7s %lx %15s %lu %511[^\n]",
                            &start, &end, perms, &offset, dev, &inode, path);
        if (fields >= 6 && path[0] != '\0' && name_equals(path, name)) {
            if (segment_count >= ZF_MAPS_MAX_SEGMENTS) {
                LOGW("stealth: too many mappings for %s, ignoring the rest", name);
                break;
            }
            segments[segment_count].start = start;
            segments[segment_count].end = end;
            segments[segment_count].prot = prot_from_perms(perms);
            segment_count++;
        }

        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }

    free(maps);

    if (segment_count == 0) {
        LOGW("stealth: no mappings found for %s", name);
        return;
    }

    int remapped = 0;
    for (int i = 0; i < segment_count; i++) {
        if (remap_segment(&segments[i])) {
            remapped++;
        }
    }

    LOGI("stealth: cloaked %s (%d/%d segments remapped)", name, remapped, segment_count);
}

static int is_frida_thread_name(const char *name) {
    if (strstr(name, "gum") != NULL || strstr(name, "frida") != NULL ||
        strstr(name, "Frida") != NULL) {
        return 1;
    }
    return strcmp(name, "gmain") == 0 || strcmp(name, "gdbus") == 0 ||
           strcmp(name, "pool-spawner") == 0;
}

static void rename_threads(const char *alias) {
    if (alias == NULL || alias[0] == '\0') {
        return;
    }

    DIR *tasks = opendir("/proc/self/task");
    if (tasks == NULL) {
        LOGW("stealth: cannot open /proc/self/task: %s", strerror(errno));
        return;
    }

    int renamed = 0;
    struct dirent *entry;
    while ((entry = readdir(tasks)) != NULL) {
        if (!isdigit((unsigned char) entry->d_name[0])) {
            continue;
        }

        char comm_path[ZF_MAX_PATH];
        snprintf(comm_path, sizeof(comm_path), "/proc/self/task/%s/comm", entry->d_name);

        int fd = open(comm_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        char comm[64] = {0};
        ssize_t n = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (n <= 0) {
            continue;
        }
        while (n > 0 && (comm[n - 1] == '\n' || comm[n - 1] == '\r')) {
            comm[--n] = '\0';
        }

        if (!is_frida_thread_name(comm)) {
            continue;
        }

        fd = open(comm_path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        ssize_t written = write(fd, alias, strlen(alias));
        close(fd);
        if (written > 0) {
            LOGI("stealth: renamed thread %s (\"%s\" -> \"%s\")",
                 entry->d_name, comm, alias);
            renamed++;
        }
    }

    closedir(tasks);
    LOGI("stealth: renamed %d frida worker thread(s)", renamed);
}

static int process_has_siblings(void) {
    uid_t self_uid = getuid();
    pid_t self_pid = getpid();

    DIR *proc = opendir("/proc");
    if (proc == NULL) {
        return 0;
    }

    int siblings = 0;
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char) entry->d_name[0])) {
            continue;
        }
        long pid = strtol(entry->d_name, NULL, 10);
        if (pid <= 0 || (pid_t) pid == self_pid) {
            continue;
        }

        char status_path[ZF_MAX_PATH];
        snprintf(status_path, sizeof(status_path), "/proc/%ld/status", pid);
        int fd = open(status_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        char header[256] = {0};
        ssize_t n = read(fd, header, sizeof(header) - 1);
        close(fd);
        if (n <= 0) {
            continue;
        }

        // Uid: <real> <effective> <saved> <fs>
        char *uid_line = strstr(header, "\nUid:");
        if (uid_line == NULL && strncmp(header, "Uid:", 4) == 0) {
            uid_line = header - 1;
        }
        if (uid_line == NULL) {
            continue;
        }
        unsigned long real_uid = strtoul(uid_line + 5, NULL, 10);
        if ((uid_t) real_uid == self_uid) {
            siblings++;
            break;
        }
    }

    closedir(proc);
    return siblings > 0;
}

static void unlink_payload(const char *lib_path, const char *app_data_dir) {
    if (app_data_dir == NULL || app_data_dir[0] == '\0') {
        return;
    }
    size_t prefix_len = strlen(app_data_dir);
    if (strncmp(lib_path, app_data_dir, prefix_len) != 0) {
        return;
    }

    if (unlink(lib_path) == 0) {
        LOGI("stealth: removed localized payload %s", lib_path);
    } else if (errno != ENOENT) {
        LOGW("stealth: cannot remove %s: %s", lib_path, strerror(errno));
    }
}

void stealth_apply(const struct target_config *cfg, const char *app_data_dir) {
    if (cfg == NULL || !cfg->stealth.enabled) {
        return;
    }

    if (cfg->stealth.remap_delay_ms > 0) {
        struct timespec ts;
        ts.tv_sec = (time_t) (cfg->stealth.remap_delay_ms / 1000);
        ts.tv_nsec = (long) ((cfg->stealth.remap_delay_ms % 1000) * 1000000L);
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        }
    }

    if (cfg->stealth.remap) {
        for (int i = 0; i < cfg->injected_libraries_count; i++) {
            remap_library(cfg->injected_libraries[i]);
        }
        if (cfg->child_gating.enabled) {
            for (int i = 0; i < cfg->child_gating.injected_libraries_count; i++) {
                remap_library(cfg->child_gating.injected_libraries[i]);
            }
        }
    }

    if (cfg->stealth.rename_threads) {
        rename_threads(cfg->stealth.thread_alias);
    }

    if (cfg->stealth.unlink_payload) {
        if (process_has_siblings()) {
            LOGI("stealth: sibling process of this app is running; keeping payload on disk "
                 "(remap already hides it from /proc/self/maps)");
        } else {
            for (int i = 0; i < cfg->injected_libraries_count; i++) {
                unlink_payload(cfg->injected_libraries[i], app_data_dir);
            }
        }
        // child_gating libraries are injected into forked children later on,
        // so their payload must stay on disk.
    }
}
