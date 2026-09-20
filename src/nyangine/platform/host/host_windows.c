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
