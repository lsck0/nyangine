#include <windows.h>
/**/
#include <bcrypt.h>

#include "nyangine/nyangine.h"

b8 nya_random_bytes(OUT u8* out, u64 size) {
    nya_assert(out != nullptr);
    nya_assert(size > 0 && size <= NYA_RANDOM_MAX_BYTES, "asked for %llu bytes of entropy", (unsigned long long)size);

    // The cast is safe because of the precondition above: NYA_RANDOM_MAX_BYTES is far inside ULONG.
    static_assert(NYA_RANDOM_MAX_BYTES <= 0xFFFFFFFFULL, "NYA_RANDOM_MAX_BYTES must fit a ULONG");

    // A null algorithm handle with BCRYPT_USE_SYSTEM_PREFERRED_RNG asks for the system pool, so there is
    // no provider to open and close around each call.
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? true : false;
}
