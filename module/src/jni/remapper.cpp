#include "remapper.h"

#include <link.h>
#include <sys/mman.h>

#include <cinttypes>
#include <cstdint>
#include <string>
#include <vector>

#include "log.h"

// Struct to hold a single entry in /proc/maps/
// Format: 7ac49c2000(start)-7ac4a26000(end) r--p (permissions) 00000000(offset) 00:00 0 (dev) 1245 (inode) /apex/com.android.runtime/bin/linker64 (path) // NOLINT
struct PROCMAPSINFO {
    uintptr_t start, end, offset;
    uint8_t perms;
    ino_t inode;
    std::string dev;
    std::string path;
};


std::vector<PROCMAPSINFO> get_modules_by_name(std::string mName) {
    std::string process_maps_locations = "/proc/self/maps";

    std::vector<PROCMAPSINFO> maps;

    char buffer[512];
    FILE *fp = fopen(process_maps_locations.c_str(), "re");

    if (fp == nullptr) {
        return maps;
    }

    while (fgets(buffer, sizeof(buffer), fp)) {
        if (strstr(buffer, mName.c_str())) {
            PROCMAPSINFO info{};
            char perms[10];
            char path[255];
            char dev[25];

            // Initialize buffers to avoid reading garbage if sscanf fails to match them
            perms[0] = '\0';
            path[0] = '\0';
            dev[0] = '\0';

            sscanf(
                buffer,
                "%" SCNxPTR "-%" SCNxPTR " %s %" SCNxPTR " %s %ld %s",
                &info.start, &info.end, perms, &info.offset, dev, &info.inode, path);

            /* Store process permissions in the struct directly via bitwise operations */
            if (strchr(perms, 'r')) info.perms |= PROT_READ;
            if (strchr(perms, 'w')) info.perms |= PROT_WRITE;
            if (strchr(perms, 'x')) info.perms |= PROT_EXEC;

            info.dev = std::string(dev);
            info.path = std::string(path);

            maps.push_back(info);
        }
    }

    fclose(fp);

    return maps;
}

void remap_lib(std::string lib_path) {
    std::string lib_name = lib_path.substr(lib_path.find_last_of("/\\") + 1);

    std::vector<PROCMAPSINFO> maps = get_modules_by_name(lib_name);
    if (maps.size() == 0) {
        return;
    }

    LOGI("Remapping %s", lib_name.c_str());

    for (PROCMAPSINFO info : maps) {
        void *address = reinterpret_cast<void *>(info.start);
        size_t size = info.end - info.start;

        void *map = mmap(0, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

        if (map == MAP_FAILED || map == nullptr) {
            LOGE("Failed to Allocate Memory: %s", strerror(errno));
            return;
        }

        if ((info.perms & PROT_READ) == 0) {
            LOGI("Removing memory protection: %s", info.path.c_str());
            mprotect(address, size, PROT_READ);
        }

        /* Copy the in-memory data to new virtual location */
        std::memmove(map, address, size);

        /* Apply target permissions BEFORE mremap to prevent mid-transition page crashes for concurrent threads */
        if (mprotect(map, size, info.perms) != 0) {
            LOGE("Failed to mprotect temp mapping to original perms: %s", strerror(errno));
        }

        /* Perform mremap to atomically replace the original mapping with the new anonymous one */
        void *remapped = mremap(map, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, info.start);
        if (remapped == MAP_FAILED) {
            LOGE("Failed to mremap: %s", strerror(errno));
        }

        LOGI("Allocated and remapped at address %p with size of %zu", reinterpret_cast<void *>(info.start), size);
    }

    LOGI("Remapped");
}
