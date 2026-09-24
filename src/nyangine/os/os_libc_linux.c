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
 * The fill is compiled ONLY where the libc lacks it (`__GLIBC_PREREQ` below): on a newer glibc
 * `<string.h>` already declares and provides the symbol, so defining our own — even weak — would trip
 * `-Wignored-attributes` (the attribute follows the header's declaration) and is pointless. Where it is
 * compiled it is the only definition, so the same archive links against both glibcs.
 */

#include <string.h>

#if defined(__GLIBC__) && !__GLIBC_PREREQ(2, 43)
void* memset_explicit(void* destination, int byte, size_t count) {
    memset(destination, byte, count);
    // The point of the C23 function over memset: a barrier so clearing about-to-be-freed secret bytes is not elided as a dead store.
    __asm__ __volatile__("" : : "r"(destination) : "memory");
    return destination;
}
#endif
