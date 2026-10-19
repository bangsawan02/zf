#include "child_gating.h"

#include <log.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inject.h"

static char child_gating_mode[ZF_MAX_MODE];  // NOLINT
static char injected_libraries[ZF_MAX_LIBS][ZF_MAX_PATH];
static int injected_libraries_count;

pid_t (*orig_fork)();

pid_t (*orig_vfork)();

static void copy_string(char *dst, size_t dst_size, const char *src) {
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

pid_t fork_replacement() {
    pid_t parent_pid = getpid();
    LOGI("[child_gating][pid %d] detected fork", parent_pid);

    pid_t child_pid = orig_fork();
    if (child_pid != 0) {
        LOGI("[child_gating][pid %d] returning from forking %d", parent_pid, child_pid);
        return child_pid;
    }

    child_pid = getpid();

    char log_context[64];
    snprintf(log_context, sizeof(log_context), "[child_gating][pid %d] ", child_pid);

    if (strcmp(child_gating_mode, "kill") == 0) {
        LOGI("%skilling child process", log_context);
        exit(0);
    }

    if (strcmp(child_gating_mode, "freeze") == 0) {
        LOGI("%sfreezing child process", log_context);
        for (;;) {
            pause();
        }
        return 0;
    }

    if (strcmp(child_gating_mode, "inject") != 0) {
        LOGI("%sunknown child_gating_mode %s", log_context, child_gating_mode);
        return 0;
    }

    for (int i = 0; i < injected_libraries_count; i++) {
        LOGI("%sInjecting %s", log_context, injected_libraries[i]);
        inject_lib(injected_libraries[i], log_context);
    }

    return 0;
}

pid_t vfork_replacement() {
    pid_t parent_pid = getpid();
    LOGI("[child_gating][pid %d] detected vfork", parent_pid);

    // CRITICAL: We cannot use orig_vfork() here because vfork shares memory with the parent.
    // If we use vfork, any memory allocation, string creation, or dlopen in the child
    // will corrupt the parent process's memory space and stack.
    // By falling back to orig_fork(), the child gets its own memory space and can safely execute complex logic.
    pid_t child_pid = orig_fork();
    if (child_pid != 0) {
        LOGI("[child_gating][pid %d] returning from vforking %d", parent_pid, child_pid);
        return child_pid;
    }

    child_pid = getpid();

    char log_context[64];
    snprintf(log_context, sizeof(log_context), "[child_gating][pid %d] ", child_pid);

    if (strcmp(child_gating_mode, "kill") == 0) {
        LOGI("%skilling child process", log_context);
        exit(0);
    }

    if (strcmp(child_gating_mode, "freeze") == 0) {
        LOGI("%sfreezing child process", log_context);
        for (;;) {
            pause();
        }
        return 0;
    }

    if (strcmp(child_gating_mode, "inject") != 0) {
        LOGI("%sunknown child_gating_mode %s", log_context, child_gating_mode);
        return 0;
    }

    for (int i = 0; i < injected_libraries_count; i++) {
        LOGI("%sInjecting %s", log_context, injected_libraries[i]);
        inject_lib(injected_libraries[i], log_context);
    }

    return 0;
}


void enable_child_gating(const struct child_gating_config *cfg) {
    if (cfg == NULL) {
        return;
    }

    copy_string(child_gating_mode, sizeof(child_gating_mode), cfg->mode);

    injected_libraries_count = cfg->injected_libraries_count;
    if (injected_libraries_count > ZF_MAX_LIBS) {
        injected_libraries_count = ZF_MAX_LIBS;
    }
    for (int i = 0; i < injected_libraries_count; i++) {
        copy_string(injected_libraries[i], ZF_MAX_PATH, cfg->injected_libraries[i]);
    }

    LOGW("[child_gating] Child gating is safely bypassed because postAppSpecialize already handles Zygote process specialization automatically. Hooking fork/vfork causes crash when executing system commands.");
}
