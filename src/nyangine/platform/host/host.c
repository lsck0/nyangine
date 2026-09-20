#include "nyangine/base/base.h"

/*
 * CPUID is the same instruction on every OS, so the processor name has no per target file. clang ships
 * cpuid.h for every x86 target it can compile for, Windows included, and the engine is x86_64 only:
 * flags.h passes -mavx2 unconditionally.
 */
#include <cpuid.h>

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The three extended leaves that spell out the 48 byte brand string, and the leaf that says they exist. */
#define _NYA_HOST_CPUID_EXTENDED_MAX   0x8000'0000U
#define _NYA_HOST_CPUID_BRAND_FIRST    0x8000'0002U
#define _NYA_HOST_CPUID_BRAND_LAST     0x8000'0004U
#define _NYA_HOST_CPUID_BRAND_BYTES    48

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_host_cpu_name(OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);

    // The fallback, so a processor without the extended leaves still names its architecture.
    NYA_ConstCString fallback = "x86_64";

    u32 registers[4] = { 0 };
    if (__get_cpuid(_NYA_HOST_CPUID_EXTENDED_MAX, &registers[0], &registers[1], &registers[2], &registers[3]) == 0 ||
        registers[0] < _NYA_HOST_CPUID_BRAND_LAST) {
        (void)snprintf((char*)buffer, capacity, "%s", fallback);
        return;
    }

    // Each leaf fills eax, ebx, ecx, edx with four characters, in that order, space padded and not
    // necessarily terminated, which is why the copy is by length rather than by strlen.
    char brand[_NYA_HOST_CPUID_BRAND_BYTES + 1] = { 0 };
    u32  written                                = 0;

    for (u32 leaf = _NYA_HOST_CPUID_BRAND_FIRST; leaf <= _NYA_HOST_CPUID_BRAND_LAST; leaf++) {
        if (__get_cpuid(leaf, &registers[0], &registers[1], &registers[2], &registers[3]) == 0) break;

        nya_memcpy(&brand[written], registers, sizeof(registers));
        written += (u32)sizeof(registers);
    }

    if (written == 0) {
        (void)snprintf((char*)buffer, capacity, "%s", fallback);
        return;
    }

    brand[written] = '\0';

    // Intel pads the brand out to 48 bytes with leading spaces; trimming them here means no caller has to.
    const char* start = brand;
    while (*start == ' ') start++;

    (void)snprintf((char*)buffer, capacity, "%s", start[0] != '\0' ? start : fallback);
}
