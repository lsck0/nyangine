#include <windows.h>
/**/
#include <bcrypt.h>

#include "nyangine/os/os_random.h"

b8 nya_os_random_bytes(OUT u8* out, u64 size) {
    // refused rather than asserted: this layer is below the assertion machinery, and a caller that
    // asks for nothing or for more than the bound gets the same "no bytes" answer as a starved pool.
    if (out == nullptr || size == 0 || size > NYA_OS_RANDOM_MAX_BYTES) return false;

    // The cast is safe because of the check above: NYA_OS_RANDOM_MAX_BYTES is far inside ULONG.
    static_assert(NYA_OS_RANDOM_MAX_BYTES <= 0xFFFFFFFFULL, "NYA_OS_RANDOM_MAX_BYTES must fit a ULONG");

    // A null algorithm handle with BCRYPT_USE_SYSTEM_PREFERRED_RNG asks for the system pool, so there is
    // no provider to open and close around each call.
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? true : false;
}
