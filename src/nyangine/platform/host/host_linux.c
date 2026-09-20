#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Where the kernel graphics drivers publish the adapter's video memory, in bytes. Only amdgpu has it;
 * the nvidia proprietary driver exposes kilobytes through a different file, and i915 exposes nothing
 * because integrated graphics have no separate pool to report.
 * */
#define _NYA_HOST_VRAM_SYSFS_PATH "/sys/class/drm/card0/device/mem_info_vram_total"

/** Longest decimal a sysfs file holds here: 20 digits of u64 plus a newline and a terminator. */
#define _NYA_HOST_VRAM_TEXT_MAX 32

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_host_memory_total_bytes(OUT u64* out_bytes) {
    nya_assert(out_bytes != nullptr);

    const long pages     = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages < 1 || page_size < 1) return false;

    *out_bytes = (u64)pages * (u64)page_size;
    return true;
}

b8 nya_host_gpu_memory_total_bytes(OUT u64* out_bytes) {
    nya_assert(out_bytes != nullptr);

    // Raw descriptors rather than nya_file_read, which wants an arena: this is asked for on the crash
    // path, where the allocator is exactly what may have gone wrong.
    const s32 file = open(_NYA_HOST_VRAM_SYSFS_PATH, O_RDONLY);
    if (file < 0) return false;

    char          text[_NYA_HOST_VRAM_TEXT_MAX] = { 0 };
    const ssize_t length                        = read(file, text, sizeof(text) - 1);
    (void)close(file);

    if (length <= 0) return false;
    text[length] = '\0';

    char*     end   = nullptr;
    const u64 bytes = strtoull(text, &end, 10);
    if (end == text || bytes == 0) return false;

    *out_bytes = bytes;
    return true;
}
