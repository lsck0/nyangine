#include <fcntl.h>
#include <sys/utsname.h>
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

/** Where every systemd-era distribution names itself. Absent on a system without one, which is fine. */
#define _NYA_HOST_OS_RELEASE_PATH "/etc/os-release"

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

void nya_host_distribution_name(OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);

    (void)snprintf((char*)buffer, capacity, "Linux");

    NYA_Arena arena = nya_arena_create_on_stack(.name = "host_distribution");
    defer     nya_arena_destroy_on_stack(&arena);

    NYA_String* release = nya_string_create(&arena);
    if (!nya_file_read(_NYA_HOST_OS_RELEASE_PATH, release).ok) return;

    /*
     * PRETTY_NAME is the one field every distribution sets and the one a person recognises. It is
     * quoted by the spec but not always in practice, so both spellings are accepted.
     */
    // Through a cstring, not release->items: an NYA_String carries a length and is not terminated, so
    // strstr would run off the end of it and into the arena. ASan caught exactly that.
    NYA_ConstCString text = nya_string_to_cstring(&arena, release);
    const char*      at   = strstr(text, "PRETTY_NAME=");
    if (at == nullptr) return;

    at += strlen("PRETTY_NAME=");
    if (*at == '"') at++;

    u32 written = 0;
    while (*at != '\0' && *at != '\n' && *at != '"' && written + 1 < capacity) buffer[written++] = (u8)*at++;

    // Only on a value that had something in it: an empty PRETTY_NAME keeps the fallback above.
    if (written > 0) buffer[written] = '\0';
}

void nya_host_kernel_name(OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);

    struct utsname system = { 0 };
    if (uname(&system) != 0) {
        (void)snprintf((char*)buffer, capacity, "unknown");
        return;
    }

    // `release` rather than `version`: 6.2.6-arch2-1 is what a bug report needs, while `version` is
    // the build banner with a timestamp in it and says nothing extra.
    (void)snprintf((char*)buffer, capacity, "%s %s", system.sysname, system.release);
}

u32 nya_platform_processor_count(void) {
    // _SC_NPROCESSORS_ONLN, not _CONF: the online count is what is actually schedulable now, which
    // is the smaller number on a machine with cores offline and the honest answer either way.
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) return 1;

    return (u32)count;
}

b8 nya_host_environment_add(NYA_ConstCString name, NYA_ConstCString value) {
    nya_assert(name != nullptr && value != nullptr);

    return setenv(name, value, 1) == 0;
}

b8 nya_host_environment_remove(NYA_ConstCString name) {
    nya_assert(name != nullptr);

    return unsetenv(name) == 0;
}
