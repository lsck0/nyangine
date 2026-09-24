#include <errno.h>
#include <sys/random.h>

#include "nyangine/os/os_random.h"

b8 nya_os_random_bytes(OUT u8* out, u64 size) {
    // Refused rather than asserted: this layer is below the assertion machinery, so a bad-size request gets the same "no bytes" answer as a starved pool.
    if (out == nullptr || size == 0 || size > NYA_OS_RANDOM_MAX_BYTES) return false;

    u64 filled = 0;

    // Looped rather than called once: getrandom returns short on a signal mid-call, and for requests over 256 bytes it is documented to return short on its own.
    while (filled < size) {
        ssize_t got = getrandom(out + filled, size - filled, 0);

        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }

        // Only possible from a zero-length request, already excluded; treated as failure not spun on, so a kernel that somehow does it cannot hang the caller.
        if (got == 0) return false;

        filled += (u64)got;
    }

    return true;
}
