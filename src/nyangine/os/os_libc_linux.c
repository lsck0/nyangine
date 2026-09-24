/**
 * @file os_libc_linux.c
 *
 * A weak fill for one libc function the vendored dependencies reach for but an older glibc does not
 * export.
 *
 * `memset_explicit` is C23's memset that a compiler may not optimise away — glibc gained it in 2.43.
 * Vendored libcurl, configured on a host whose glibc has it, compiles the code path that calls it (its
 * `curlx_memzero`/`curlx_strzero`). Linking that same archive against an older glibc — a devenv shell
 * pinned to 2.42, say, while the host is 2.44 — then fails with `undefined symbol: memset_explicit`.
 *
 * This definition is `weak`: the libc's own strong symbol overrides it wherever the runtime has one, so
 * the host build is byte-for-byte unchanged, and it is the only definition where the runtime does not,
 * so the same archive links in both. Compiled once, in the engine's unity build, next to the other
 * Linux os translation units.
 */

#include <string.h>

// Redundant where <string.h> already declares it (a glibc that has the symbol), needed where it does
// not; the declaration also marks the definition below as deliberately external, not a file-local it
// could be made static. The parameter names differ from glibc's internal `__s`/`__c`/`__n`, which is
// only a readability note against a header the caller never sees.
// NOLINTNEXTLINE(readability-redundant-declaration)
void* memset_explicit(void* destination, int byte, size_t count);

// NOLINTNEXTLINE(readability-inconsistent-declaration-parameter-name)
__attribute__((weak)) void* memset_explicit(void* destination, int byte, size_t count) {
    memset(destination, byte, count);
    // The whole point of the C23 function over memset: a barrier so the clear of about-to-be-freed
    // secret bytes is not elided as a dead store, which is exactly what glibc's own version guarantees.
    __asm__ __volatile__("" : : "r"(destination) : "memory");
    return destination;
}
