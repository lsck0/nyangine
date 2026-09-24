#include "nyangine/os/os_library.h"

// after the engine's own header, for the same reason os_process_linux.c orders its POSIX includes
// after it: these declare what they declare once the strict POSIX request has been seen.
#include <dlfcn.h>

b8 nya_os_library_probe(const char* soname) {
    if (soname == nullptr || soname[0] == '\0') return false;

    // RTLD_LAZY, because nothing here calls into the library: binding its symbols now would be work for
    // a handle about to be dropped. RTLD_LOCAL, so a probe never leaks a library's symbols into the
    // global namespace where later code might bind to them by accident.
    void* handle = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) return false;

    // Closed at once: this only answers whether it is here. dlopen reference counts, so a library the
    // program has genuinely loaded elsewhere is not unloaded by this close.
    (void)dlclose(handle);

    return true;
}
