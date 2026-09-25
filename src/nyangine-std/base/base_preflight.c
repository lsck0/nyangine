#include "nyangine-std/base/base_preflight.h"

#include "nyangine-std/base/base_logging.h"
#include "nyangine-std/os/os_library.h"
#include "nyangine-std/os/os_process.h"

// PRESENT / ABSENT

b8 nya_preflight_program_present(NYA_ConstCString name) {
    if (name == nullptr || name[0] == '\0') return false;

    // The resolved path is not wanted here, only whether there is one, so no buffer is passed down.
    return nya_os_process_which(name, nullptr, 0);
}

b8 nya_preflight_library_present(NYA_ConstCString soname) {
    if (soname == nullptr || soname[0] == '\0') return false;

    return nya_os_library_probe(soname);
}

// REQUIRE (CRASH WHEN ABSENT)

void nya_require_program(NYA_ConstCString name, NYA_ConstCString why) {
    if (nya_preflight_program_present(name)) return;

    // The three things a crash reader needs: what is missing, what wanted it, and where it was sought; install-and-relaunch, not a distro package name, since gpg is named differently on every system.
    nya_log_panic(
        "required program '%s' is not on PATH: %s needs it. install it and make sure it is on this process's PATH, then relaunch.",
        name != nullptr ? name : "(null)",
        why != nullptr && why[0] != '\0' ? why : "a feature of this program"
    );
}

void nya_require_library(NYA_ConstCString soname, NYA_ConstCString why) {
    if (nya_preflight_library_present(soname)) return;

    nya_log_panic(
        "required shared library '%s' could not be loaded: %s needs it. install the package that provides it, make sure the dynamic loader can "
        "find it, then relaunch.",
        soname != nullptr ? soname : "(null)",
        why != nullptr && why[0] != '\0' ? why : "a feature of this program"
    );
}
