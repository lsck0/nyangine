/**
 * The runtime-dependency preflight: the present/absent core answers on real and bogus names without
 * dying, and the require wrappers crash with a descriptive message when the dependency is absent.
 *
 * The core and the crash are tested apart on purpose. The core is a plain function a test drives on a
 * program it knows is there (`sh`) and one it knows is not, and on a library the same way. The abort
 * is driven under nya_expect_crash, which arms crash prevention so the panic is caught rather than
 * ending the test process, and the caught crash is then inspected: a PANIC whose message names the
 * missing dependency and the feature that wanted it.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** A name nothing on any PATH is called, so the "absent" answer is not a flake about the test box. */
#define BOGUS_PROGRAM "nyangine-nonexistent-preflight-xyzzy"

/** A soname no loader has, for the same reason. */
#define BOGUS_LIBRARY "libnyangine-nonexistent-preflight-xyzzy.so.999"

s32 main(void) {
    // ── The program core: a present program is found, a bogus one is not, and neither crashes.
    {
        nya_check(nya_preflight_program_present("sh"), "sh should be found on PATH");
        nya_check(!nya_preflight_program_present(BOGUS_PROGRAM), "a bogus program name should not be found");

        // The empty and null cases are refused, not crashed: this is the layer a test can call freely.
        nya_check(!nya_preflight_program_present(""), "an empty name should not be found");
        nya_check(!nya_preflight_program_present(nullptr), "a null name should not be found");
    }

    // ── The os lookup resolves to a runnable absolute path, the way a spawn would find it.
    {
        char path[NYA_OS_PATH_MAX] = { 0 };
        nya_check(nya_os_process_which("sh", path, sizeof(path)), "sh should resolve");
        nya_check(path[0] == '/', "the resolved sh path should be absolute, got '%s'", path);

        // A name spelled with a directory in it is checked as it stands rather than looked up on PATH.
        nya_check(nya_os_process_which(path, nullptr, 0), "the resolved absolute path should itself resolve");
        nya_check(!nya_os_process_which("./" BOGUS_PROGRAM, nullptr, 0), "a bogus relative path should not resolve");
    }

    // ── The library core: a library every Linux box has is present, a bogus soname is not.
    {
#if OS_LINUX
        nya_check(nya_preflight_library_present("libc.so.6"), "the C library should be loadable");
#endif
        nya_check(!nya_preflight_library_present(BOGUS_LIBRARY), "a bogus soname should not be loadable");
        nya_check(!nya_preflight_library_present(""), "an empty soname should not be loadable");
        nya_check(!nya_preflight_library_present(nullptr), "a null soname should not be loadable");
    }

    // ── A present program is required without incident: no crash, execution continues past the call.
    {
        nya_require_program("sh", "the preflight self-test");
        nya_check(true, "requiring a present program returned normally");
    }

    // ── A missing program crashes, and the crash is a PANIC naming the program and the feature.
    {
        nya_expect_crash(nya_require_program(BOGUS_PROGRAM, "the preflight self-test feature"));

        const NYA_CrashInfo* crash = nya_crash_caught();
        nya_check(crash != nullptr, "a missing program should have crashed");

        if (crash != nullptr) {
            nya_check(crash->source == NYA_CRASH_SOURCE_PANIC, "the crash should be a panic, was %d", (s32)crash->source);
            nya_check(strstr((const char*)crash->message, BOGUS_PROGRAM) != nullptr, "the message should name the program: '%s'", (const char*)crash->message);
            nya_check(strstr((const char*)crash->message, "self-test feature") != nullptr, "the message should say what needed it: '%s'", (const char*)crash->message);
            nya_check(strstr((const char*)crash->message, "PATH") != nullptr, "the message should say how it failed: '%s'", (const char*)crash->message);
        }
    }

    // ── A missing library crashes the same way, naming the soname and the feature.
    {
        nya_expect_crash(nya_require_library(BOGUS_LIBRARY, "the preflight library self-test"));

        const NYA_CrashInfo* crash = nya_crash_caught();
        nya_check(crash != nullptr, "a missing library should have crashed");

        if (crash != nullptr) {
            nya_check(crash->source == NYA_CRASH_SOURCE_PANIC, "the crash should be a panic, was %d", (s32)crash->source);
            nya_check(strstr((const char*)crash->message, BOGUS_LIBRARY) != nullptr, "the message should name the library: '%s'", (const char*)crash->message);
            nya_check(strstr((const char*)crash->message, "library self-test") != nullptr, "the message should say what needed it: '%s'", (const char*)crash->message);
        }
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
