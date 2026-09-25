#include "nyangine-std/os/os_library.h"

// After the engine's own header, like os_process_linux.c: these declare what they declare once the strict POSIX request has been seen.
#include <dlfcn.h>

b8 nya_os_library_probe(const char* soname) {
    if (soname == nullptr || soname[0] == '\0') return false;

    // RTLD_LAZY since nothing calls into the library, and RTLD_LOCAL so a probe never leaks its symbols into the global namespace.
    void* handle = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) return false;

    // Closed at once: this only answers whether it is here. dlopen reference counts, so a library loaded elsewhere is not unloaded by this close.
    (void)dlclose(handle);

    return true;
}
