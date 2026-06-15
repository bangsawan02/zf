#include "child_gating.h"

#include <dobby.h>
#include <log.h>
#include <unistd.h>
#include <xdl.h>

#include <cstdlib>
#include <future>
#include <string>
#include <vector>

#include "config.h"
#include "inject.h"

static std::string child_gating_mode;  // NOLINT
static std::vector<std::string> injected_libraries;

pid_t (*orig_fork)();

pid_t (*orig_vfork)();

pid_t fork_replacement() {
    pid_t parent_pid = getpid();
    LOGI("[child_gating][pid %d] detected fork", parent_pid);

    pid_t child_pid = orig_fork();
    if (child_pid != 0) {
        LOGI("[child_gating][pid %d] returning from forking %d", parent_pid, child_pid);
        return child_pid;
    }

    child_pid = getpid();

    auto logContext = "[child_gating][pid " + std::to_string(child_pid) + "] ";

    if (child_gating_mode == "kill") {
        LOGI("%skilling child process", logContext.c_str());
        exit(0);
    }

    if (child_gating_mode == "freeze") {
        LOGI("%sfreezing child process",  logContext.c_str());
        std::promise<void>().get_future().wait();
        return 0;
    }

    if (child_gating_mode != "inject") {
        LOGI("%sunknown child_gating_mode %s",  logContext.c_str(), child_gating_mode.c_str());
        return 0;
    }

    for (auto &lib_path : injected_libraries) {
        LOGI("%sInjecting %s",  logContext.c_str(), lib_path.c_str());
        inject_lib(lib_path, logContext);
    }

    return 0;
}

pid_t vfork_replacement() {
    pid_t parent_pid = getpid();
    LOGI("[child_gating][pid %d] detected vfork", parent_pid);

    // CRITICAL: We cannot use orig_vfork() here because vfork shares memory with the parent.
    // If we use vfork, any memory allocation, string creation, or dlopen in the child
    // will corrupt the parent process's memory space and stack.
    // By falling back to orig_fork(), the child gets its own memory space and can safely execute complex C++ logic.
    pid_t child_pid = orig_fork();
    if (child_pid != 0) {
        LOGI("[child_gating][pid %d] returning from vforking %d", parent_pid, child_pid);
        return child_pid;
    }

    child_pid = getpid();

    auto logContext = "[child_gating][pid " + std::to_string(child_pid) + "] ";

    if (child_gating_mode == "kill") {
        LOGI("%skilling child process", logContext.c_str());
        exit(0);
    }

    if (child_gating_mode == "freeze") {
        LOGI("%sfreezing child process",  logContext.c_str());
        std::promise<void>().get_future().wait();
        return 0;
    }

    if (child_gating_mode != "inject") {
        LOGI("%sunknown child_gating_mode %s",  logContext.c_str(), child_gating_mode.c_str());
        return 0;
    }

    for (auto &lib_path : injected_libraries) {
        LOGI("%sInjecting %s",  logContext.c_str(), lib_path.c_str());
        inject_lib(lib_path, logContext);
    }

    return 0;
}


void enable_child_gating(child_gating_config const &cfg) {
    child_gating_mode = cfg.mode;
    injected_libraries = cfg.injected_libraries;

    LOGW("[child_gating] Child gating is safely bypassed because postAppSpecialize already handles Zygote process specialization automatically. Hooking fork/vfork causes crash when executing system commands.");
}

