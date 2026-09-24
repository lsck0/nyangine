#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nyangine/os/os_library.h"

b8 nya_os_library_probe(const char* soname) {
    if (soname == nullptr || soname[0] == '\0') return false;

    // LoadLibraryA does the loader's own search: the same resolution a genuine load would do, so a
    // module that loads here is one the code that needs it will find too.
    HMODULE module = LoadLibraryA(soname);
    if (module == nullptr) return false;

    // Reference counted like dlopen, so freeing the probe's own reference leaves a module the program
    // has really loaded elsewhere in place.
    (void)FreeLibrary(module);

    return true;
}
