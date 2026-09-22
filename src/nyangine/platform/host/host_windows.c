#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_host_memory_total_bytes(OUT u64* out_bytes) {
    nya_assert(out_bytes != nullptr);

    MEMORYSTATUSEX status = { .dwLength = sizeof(status) };
    if (!GlobalMemoryStatusEx(&status)) return false;
    if (status.ullTotalPhys == 0) return false;

    *out_bytes = (u64)status.ullTotalPhys;
    return true;
}

b8 nya_host_gpu_memory_total_bytes(OUT u64* out_bytes) {
    nya_unused(out_bytes);

    /*
     * Not answered rather than guessed. The number lives behind DXGI (IDXGIAdapter::GetDesc), which
     * means linking dxgi.lib and creating a factory just to print one line in a crash report, and the
     * engine's device is SDL_GPU's, which may be Vulkan. The report prints the driver's own name and
     * version instead, which is what a bug triage actually starts from.
     */
    return false;
}

void nya_host_distribution_name(OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);

    /*
     * The registry rather than GetVersionEx, which lies: since Windows 8.1 it reports the version the
     * executable declares compatibility with in its manifest, not the one it is running on, so a build
     * without the right manifest GUID reads 6.2 forever. CurrentBuild is what winver shows.
     */
    NYA_ConstCString key = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

    char product[NYA_HOST_DISTRIBUTION_NAME_MAX] = { 0 };
    char build[32]                               = { 0 };

    DWORD product_size = sizeof(product);
    DWORD build_size   = sizeof(build);

    b8 has_product = RegGetValueA(HKEY_LOCAL_MACHINE, key, "ProductName", RRF_RT_REG_SZ, nullptr, product, &product_size) == ERROR_SUCCESS;
    b8 has_build   = RegGetValueA(HKEY_LOCAL_MACHINE, key, "CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &build_size) == ERROR_SUCCESS;

    if (has_product && has_build) {
        (void)snprintf((char*)buffer, capacity, "%s (build %s)", product, build);
    } else if (has_product) {
        (void)snprintf((char*)buffer, capacity, "%s", product);
    } else {
        (void)snprintf((char*)buffer, capacity, "Windows");
    }
}

void nya_host_kernel_name(OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);

    /*
     * Windows has no kernel version separate from the product one a person would quote, and the build
     * number nya_host_distribution_name already prints is that number. Answered rather than left to a
     * caller to special-case: the crash report prints one line per fact on every target.
     */
    (void)snprintf((char*)buffer, capacity, "Windows NT");
}

b8 nya_host_environment_set(NYA_ConstCString name, NYA_ConstCString value) {
    nya_assert(name != nullptr && value != nullptr);

    // the CRT's copy rather than SetEnvironmentVariable, since getenv reads the CRT's and not the OS block.
    return _putenv_s(name, value) == 0;
}

b8 nya_host_environment_remove(NYA_ConstCString name) {
    nya_assert(name != nullptr);

    // an empty value is how the CRT spells removal.
    return _putenv_s(name, "") == 0;
}
