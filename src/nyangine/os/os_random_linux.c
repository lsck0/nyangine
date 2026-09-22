#include <errno.h>
#include <sys/random.h>

#include "nyangine/os/os_random.h"

b8 nya_os_random_bytes(OUT u8* out, u64 size) {
    // refused rather than asserted: this layer is below the assertion machinery, and a caller that
    // asks for nothing or for more than the bound gets the same "no bytes" answer as a starved pool.
    if (out == nullptr || size == 0 || size > NYA_OS_RANDOM_MAX_BYTES) return false;

    u64 filled = 0;

    // Looped rather than called once: getrandom returns short whenever a signal arrives mid call, and
    // for requests over 256 bytes it is documented to return short on its own.
    while (filled < size) {
        ssize_t got = getrandom(out + filled, size - filled, 0);

        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }

        // Only possible from a zero length request, which the precondition already excluded. Treated as
        // a failure rather than spun on, so a kernel that somehow does it cannot hang the caller.
        if (got == 0) return false;

        filled += (u64)got;
    }

    return true;
}
