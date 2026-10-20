/**
 * @file flags.h
 *
 * Every name, path and compiler flag the build system uses, and which of them this host picks.
 *
 * Three sections, in the order a reader needs them: what the artifacts are called, what they are
 * compiled with, and, at the bottom, the HOST_* selection that resolves the pairs above into the one
 * the machine doing the building actually uses. That last part used to be a host.h of its own, which
 * only ever selected between macros defined here and so had to be included in exactly the right place
 * relative to this file. One file, one order, nothing to get wrong.
 * */
#pragma once

#include "nyangine-std/base/base_basic.h"

// Which host is doing the building decides the tool names every rule uses. First, because the flags below and the vendor rules both expand them.
#if OS_WINDOWS
#include "nyangine-build/on_windows/toolchain.h"
#else
#include "nyangine-build/on_linux/toolchain.h"
#endif

// clang-format off

/* ARTIFACTS */

#define PROJECT_NAME "gnyame"

/**
 * The one definition of the version. Everything else derives from it: the artifact names below, the
 * `-DVERSION` the compile hooks pass, the parser's `--help` banner, and `./build version`, which is
 * what the packaging scripts and the release workflow read instead of parsing this file.
 * */
#define VERSION      "0.0.0"

/**
 * The example that selects the terminal backend, and so the only translation unit in the tree that
 * compiles render2d_terminal.c. `./build check --strict` analyses it for exactly that reason.
 * */
#define TERMINAL_SOURCE_PATH "./examples/tui_dashboard/main.c"

#define BINARY_SOURCE_PATH "./src/main.c"
#define DLL_SOURCE_PATH    "./src/gnyame/gnyame.c"

/*
 * ─────────────────────────────────────────────────────────
 * APPS
 * ─────────────────────────────────────────────────────────
 *
 * A project builds many app binaries, not one. src/main.c is a generic host — it loads a DLL and
 * reloads it on change, and picks which by its own name (argv[0]), see dll_path_from_executable — so
 * every app is one more DLL beside that host and a host relinked under the app's name. gnyame is the
 * default app, built from PROJECT_NAME above and the rules that shipped; the list here is the rest.
 *
 * Each app is a name and the DLL translation unit it is built from. The artifact names fall out of the
 * name the same way gnyame's fall out of PROJECT_NAME: `<name>.debug` is the host and `<name>.debug.so`
 * the DLL it loads. To add an app, add its two macros here, a set of debug rules in on_linux/build_linux.h
 * modelled on gnyame-cli's, and a `run`/`build` subcommand in cli.c naming them. Release, Windows and
 * Steam still build the default app from PROJECT_NAME; a second shipped app extends those the same way.
 */
#define APP_GNYAME_CLI_NAME       "gnyame-cli"
#define APP_GNYAME_CLI_DLL_SOURCE "./src/gnyame_cli/gnyame_cli.c"

#define LINUX_X86_64_GNYAME_CLI_DEBUG_BINARY APP_GNYAME_CLI_NAME ".debug"
#define LINUX_X86_64_GNYAME_CLI_DEBUG_DLL    APP_GNYAME_CLI_NAME ".debug.so"

#define LINUX_X86_64_DEBUG_BINARY   PROJECT_NAME ".debug"
#define LINUX_X86_64_DEBUG_DLL      PROJECT_NAME ".debug.so"
#define LINUX_X86_64_DEV_BINARY     PROJECT_NAME ".dev"
#define LINUX_X86_64_DEV_DLL        PROJECT_NAME ".dev.so"
#define LINUX_X86_64_BINARY         PROJECT_NAME "." VERSION ".linux-x86_64"
#define LINUX_X86_64_TEST_BINARY    PROJECT_NAME ".test"
#define WINDOWS_X86_64_DEBUG_BINARY PROJECT_NAME ".debug.exe"
#define WINDOWS_X86_64_DEBUG_DLL    PROJECT_NAME ".debug.dll"
#define WINDOWS_X86_64_DEBUG_IMPLIB PROJECT_NAME ".debug.lib"
#define WINDOWS_X86_64_DEV_BINARY   PROJECT_NAME ".dev.exe"
#define WINDOWS_X86_64_DEV_DLL      PROJECT_NAME ".dev.dll"
#define WINDOWS_X86_64_DEV_IMPLIB   PROJECT_NAME ".dev.lib"
#define WINDOWS_X86_64_BINARY       PROJECT_NAME "." VERSION ".windows-x86_64.exe"

/* A Steam build is a directory, since the Steamworks library ships beside the executable. Its contents are the depot. */
#define STEAM_LINUX_X86_64_DIRECTORY   PROJECT_NAME "." VERSION ".steam-linux-x86_64"
#define STEAM_LINUX_X86_64_BINARY      STEAM_LINUX_X86_64_DIRECTORY "/" PROJECT_NAME
#define STEAM_LINUX_X86_64_LIBRARY     STEAM_LINUX_X86_64_DIRECTORY "/libsteam_api.so"
#define STEAM_WINDOWS_X86_64_DIRECTORY PROJECT_NAME "." VERSION ".steam-windows-x86_64"
#define STEAM_WINDOWS_X86_64_BINARY    STEAM_WINDOWS_X86_64_DIRECTORY "/" PROJECT_NAME ".exe"
#define STEAM_WINDOWS_X86_64_LIBRARY   STEAM_WINDOWS_X86_64_DIRECTORY "/steam_api64.dll"

/*
 * Where split rules compile to before linking, one object per artifact. Nothing reads an object from a
 * previous build: every compile runs, and a compiler cache is what makes an unchanged one cheap.
 */
#define OBJECT_DIRECTORY "./.objects"
#define OBJECT_SUFFIX    ".o"

/* COMPILER FLAGS */

// CC and NPROCS come from build/vendor/vendor.h.
// -mfma is not implied by -mavx2, and nn/nn_simd.h falls back to separate multiply and add without it.
// -mf16c turns the half float casts in NYA_Vertex3D into one instruction instead of a libgcc call per channel.
// Every AVX2 CPU has FMA3 and F16C (all came with Haswell), so neither adds a requirement.
// -Wno-psabi: clang 23 warns that a 256-bit f128 vector passed by value changes the ABI without AVX-512. Every
// translation unit here is built with the same -m flags, so no call crosses that boundary.
#define CFLAGS        "-std=c2y", "-mavx", "-mavx2", "-mfma", "-mf16c", "-Wno-psabi", "-fdefer-ts", "-fenable-matrix", "-ggdb"
// -Wframe-larger-than: a Windows thread gets 1 MB of stack against Linux's 8, so a frame that fits here can
// overflow there, and only CI on Windows would say so. A quarter of that stays one frame's share. Unoptimized
// builds count compound literal temporaries too, so `*big = (T){ 0 }` of a large T is caught; memset it instead.
// -Wno-psabi: clang 23 warns that an f128 vector passed by value has a different ABI without AVX-512. The engine
// is one translation unit built with one set of flags, so no such vector ever crosses a boundary compiled otherwise.
#define WARNINGS      "-Werror", "-Wall", "-Wextra", "-Wstrict-prototypes", "-Wswitch", "-Wswitch-default", "-Wimplicit-fallthrough", "-Wframe-larger-than=262144", "-Wno-gnu", "-Wno-gcc-compat", "-Wno-initializer-overrides", "-Wno-keyword-macro", "-Wno-psabi"
/* A rule that compiles with `-c` takes only compile flags: under -Werror clang rejects a linker flag it cannot use. Link only are LINKER_FLAGS, every *_LINK macro, FLAGS_LINUX_X86_64, FLAGS_WINDOWS_X86_64 and the FLAGS_*_WINDOWS_X86_64 and FLAGS_DEBUG_LINUX_X86_64 linker setups. A link repeats the mode flags, since optimisation, LTO, sanitizers and coverage all need them there too, and clang ignores the preprocessor flags among them. */

// Only the project's own paths. Everything a third party dependency needs lives on its
// NYA_VendorRule instead, so this does not grow as dependencies are added.
#define INCLUDE_PATHS "-I./", "-I./src/"
#define LINKER_FLAGS  "-lm", "-pthread"

/*
 * Which engine modules are optional today, resolved per target rather than per host: HTTP compression
 * is on for a Linux target and off for a Windows one, and the build tool that assembles these lists is
 * itself a Linux binary when it cross compiles the Windows target — so a plain `#if OS_WINDOWS` would
 * hand a cross compiled Windows build the Linux module set and look for zlib and brotli headers a
 * mingw sysroot does not have. The two target sets are spelled out, and FLAGS_MODULES below
 * picks the host's own for the rules built to run here (the tests, the build tool, the checks).
 *
 * `db` wants sqlite on the include line, which a host tool build does not have, so it is switched on
 * here rather than compiled unconditionally by nyangine.h. The component system in TODO.md's roadmap
 * replaces this with a component list.
 *
 * There is no `tls` module: TLS terminates on the platform an app is deployed to, and the one outbound
 * TLS link (curl, and mail through it) uses curl's own backend per target.
 *
 * HTTP response compression reaches the system's zlib and brotli, which a Windows build here has no
 * copy of on the link line — so the feature is Linux only for now and compiles to a no-op elsewhere.
 * The libraries themselves are added to the Linux link in FLAGS_LINUX_X86_64, next to the rpath, since
 * they are a system dependency and not a vendored archive. NYA_HTTP_COMPRESSION turns on gzip and
 * deflate; NYA_HTTP_COMPRESSION_BROTLI adds `br`.
 *
 * The compression macro is a leading comma rather than a trailing one, with no comma before it in the
 * module lists: that is how a macro that may expand to nothing joins a list without leaving a double
 * comma behind on the target where it is empty, the same trick FLAGS_TARGET_WINDOWS_X86_64 uses.
 */
#define FLAGS_MODULE_COMPRESSION_LINUX_X86_64   , "-DNYA_HTTP_COMPRESSION", "-DNYA_HTTP_COMPRESSION_BROTLI"
#define FLAGS_MODULE_COMPRESSION_WINDOWS_X86_64

#define FLAGS_MODULES_LINUX_X86_64   "-DNYA_MODULE_DB" FLAGS_MODULE_COMPRESSION_LINUX_X86_64
#define FLAGS_MODULES_WINDOWS_X86_64 "-DNYA_MODULE_DB" FLAGS_MODULE_COMPRESSION_WINDOWS_X86_64

// The module set for a rule built to run on this host: the tests, the build tool and `./build check`. A Windows host never cross compiles a Linux target, so a host gate is right for the native side.
#if OS_WINDOWS
#define FLAGS_MODULES FLAGS_MODULES_WINDOWS_X86_64
#else
#define FLAGS_MODULES FLAGS_MODULES_LINUX_X86_64
#endif

/*
 * Which optional plugins the *project* compiles. See src/nyangine/plugins/plugins.h. The plugin list
 * itself is target independent; only the modules that ride along with it differ, so the Windows target
 * rules pass FLAGS_PLUGINS_WINDOWS_X86_64 to pin the Windows module set no matter the building host.
 */
#define FLAGS_PLUGIN_LIST "-DNYA_PLUGIN_CURL", "-DNYA_PLUGIN_DISCORD", "-DNYA_PLUGIN_LUA", FLAGS_PLUGIN_PERMISSIONS
#define FLAGS_PLUGINS               FLAGS_MODULES, FLAGS_PLUGIN_LIST
#define FLAGS_PLUGINS_WINDOWS_X86_64 FLAGS_MODULES_WINDOWS_X86_64, FLAGS_PLUGIN_LIST

/*
 * The game's one decision about what a Lua plugin may touch, fixed here and nowhere else: nothing at
 * runtime widens it, and a plugin whose manifest asks for more is refused at load. `2` is
 * NYA_PLUGIN_PERMISSION_PROFILE_GAMEPLAY — UI, input, key bindings, entities, audio and the shipped
 * assets — and deliberately not `3`, which would add the filesystem and the network.
 *
 * Spelled as the number rather than the name because this is a `-D` on a command line and the name is
 * a macro defined in core_plugin.h, which the preprocessor has not read yet when it reads this.
 */
#define FLAGS_PLUGIN_PERMISSIONS "-DNYA_PLUGIN_PERMISSION_PROFILE=2"

/* Every mode sets NYA_EXECUTION_MODE explicitly. It defaults to 0, which means debug, not unset. */
#define FLAGS_DEBUG     "-DNYA_EXECUTION_MODE=0", "-DDEBUG=true", "-O0", "-DNYA_ASSET_HOT_RELOAD"

// Developer: hot reload like debug, but optimized and without sanitizers. For actually playing the
// game while iterating on it, where debug is too slow to feel right.
#define FLAGS_DEVELOPER "-DNYA_EXECUTION_MODE=1", "-O2", "-DNYA_ASSET_HOT_RELOAD"

// Test: assertions live, nya_expect_crash compiled in so a test can survive a deliberate panic, and
// headless so it needs no GPU. Statically linked game code, no hot reload DLL to find.
#define FLAGS_TEST      "-DNYA_EXECUTION_MODE=4", "-O0", "-DNYA_TESTING", "-DNYA_HEADLESS"

/**
 * What a benchmark is compiled with: optimised, headless, and *without* sanitizers.
 * */
#define FLAGS_BENCH     "-DNYA_EXECUTION_MODE=2", "-O2", "-DNYA_HEADLESS", "-fno-omit-frame-pointer"

/**
 * Source based coverage instrumentation, for `./build coverage`. Added only to that build, never to
 * check, debug or release: a counter in every region is overhead the shipping binary should not carry,
 * and a profile file it should not write. See test.c.
 * */
#define FLAGS_COVERAGE  "-fprofile-instr-generate", "-fcoverage-mapping"

/** Where a coverage run puts the raw profiles, the merged profile, the HTML listing and the instrumented binaries. */
#define COVERAGE_DIRECTORY      "./.coverage"
#define COVERAGE_PROFILE_DATA   COVERAGE_DIRECTORY "/merged.profdata"
#define COVERAGE_HTML_DIRECTORY COVERAGE_DIRECTORY "/html"

/**
 * The floor `./build coverage --fail-under` defaults to: below it the command exits non-zero. A low
 * bar on purpose, a floor to ratchet up as the tests grow rather than one set where they are today.
 * */
#define COVERAGE_DEFAULT_FAIL_UNDER 45

// The build system is a host tool. It needs base, math, platform and serde and nothing that opens
// a window, so core and renderer are compiled out rather than linked and left unused. Without this
// the tool would need SDL on the link line to build SDL, which is a bootstrap it cannot satisfy.
#define FLAGS_BUILD_TOOL "-DNYA_NO_SDL"

// A headless server: no SDL and no core, the http/net half of the engine only. NYA_SERVER opens the
// seam nyangine.h and nyangine.c both cut around crypto/net/http, so those compile while core
// and the renderer do not. The module set is the server's — db and HTTP compression, exactly what
// NYA_SERVER_VENDORS_LINUX_X86_64 has libraries for — but deliberately not FLAGS_PLUGIN_LIST: the Lua
// plugin reaches nya_app_get, which is core, so a plugin build would drag the wall back in. An example
// opts into this with a `.headless` marker beside its main.c; see build_headless_server_example in
// example.c and docs/layering-core-split.md, steps 6–7.
#define FLAGS_SERVER_HEADLESS "-DNYA_NO_SDL", "-DNYA_SERVER" FLAGS_MODULE_COMPRESSION_LINUX_X86_64

// Runs the engine with the drawing compiled out. Everything else still runs, so a test exercises
// the real frame loop; there is just no GPU device to create, which is what CI cannot provide.
#define FLAGS_HEADLESS "-DNYA_HEADLESS"

// Draws into a terminal instead of a swapchain: nyangine.c compiles render2d_terminal.c in place of
// render2d.c. Implies NYA_HEADLESS, so it is that plus a backend and never combined with it.
#define FLAGS_TERMINAL "-DNYA_TERMINAL"

#define FLAGS_DLL_COMPILE "-fPIC"
#define FLAGS_DLL_LINK    "-shared"
#define FLAGS_SANITIZE "-fno-omit-frame-pointer", "-fno-optimize-sibling-calls", "-fno-sanitize-recover=all", "-fsanitize=address,leak,undefined,signed-integer-overflow,unsigned-integer-overflow,shift,float-cast-overflow,float-divide-by-zero,pointer-overflow"

// -g1 so libbacktrace can print lines in shipped crash reports. The debug sections are covered by the
// integrity CRC, so never strip after hook_insert_integrity_hash.
//
// The hardening here is what both the Linux and the mingw Windows toolchains accept: stack canaries
// (-fstack-protector-strong) and control-flow-integrity landing pads (-fcf-protection=full). The two
// mitigations that differ by target — the _FORTIFY_SOURCE level and -fstack-clash-protection — live in
// the FLAGS_HARDEN_* groups below and are added to the per-target compile rules, not here: a single
// _FORTIFY_SOURCE=N belongs on the command line (a second one is a -Werror macro redefinition), and
// clang rejects -fstack-clash-protection for the Windows target.
#define FLAGS_SHIPPING "-O3", "-flto", "-fPIE", "-g1", "-DNYA_ASSET_PREFER_BLOB", "-fcf-protection=full", "-fstack-protector-strong", "-fno-omit-frame-pointer"

// Compile-time hardening that varies by target, added to the release/dist (and Steam) compile rules
// beside FLAGS_SHIPPING. glibc's _FORTIFY_SOURCE needs an optimisation level, which FLAGS_SHIPPING's -O3
// gives it; level 3 adds the dynamic-object-size fortify checks (__*_chk) over level 2's constant ones.
// -fstack-clash-protection probes each newly touched stack page so a frame larger than the guard page
// cannot leap over it into the heap — an x86/Linux codegen flag.
#define FLAGS_HARDEN_LINUX_X86_64   "-D_FORTIFY_SOURCE=3", "-fstack-clash-protection"
// mingw-w64 carries its own _FORTIFY_SOURCE; keep the level 2 the Windows build already shipped. No
// -fstack-clash-protection: clang does not support it for the x86_64-w64 PE target (unsupported-option
// error), and the ELF RELRO/BIND_NOW/NX linker flags below are likewise meaningless for a PE image.
#define FLAGS_HARDEN_WINDOWS_X86_64 "-D_FORTIFY_SOURCE=2"

// -DNYA_EXECUTION_MODE=2 is required: NYA_DEBUG is (NYA_EXECUTION_MODE == 0) and the default is 0, so
// without it a release binary compiles the hot reload entry point and skips the integrity check.
#define FLAGS_RELEASE  "-DNYA_EXECUTION_MODE=2", FLAGS_SHIPPING

// --gc-sections drops the vendor functions nothing reaches, which NYA_VENDOR_OPTIMIZE put in sections of their own.
#define FLAGS_RELEASE_LINK "-fuse-ld=lld", "-Wl,--gc-sections"

// no local symbols, and no COFF symbol table: libbacktrace names frames from the -g1 debug info and never reads them.
//
// ELF hardening, Linux only (not in the shared FLAGS_RELEASE_LINK, which the PE link also uses): -z relro
// plus -z now is full RELRO — the GOT and other relocated data are mapped read-only after the loader has
// bound every symbol at startup, so a later write cannot repoint a call. -z noexecstack sets the NX bit on
// the stack (a non-executable PT_GNU_STACK). hook_verify_hardening asserts all three are present on the
// produced binary, so a toolchain that quietly dropped one fails the build rather than shipping soft.
#define FLAGS_RELEASE_LINK_LINUX_X86_64   "-Wl,--discard-all", "-Wl,-z,relro", "-Wl,-z,now", "-Wl,-z,noexecstack"
#define FLAGS_RELEASE_LINK_WINDOWS_X86_64 "-Xlinker", "-Xlink=-debug:dwarf,nosymtab"

/*
 * Reproducible builds: the same commit must give the same bytes on any machine, which is what lets a
 * signed release be checked by someone who did not build it. See TODO.md and `./build reproduce`.
 *
 * -ffile-prefix-map rewrites the one absolute path clang would otherwise bake into a release binary: the
 * compilation directory (DWARF DW_AT_comp_dir), which is wherever the build happened to run. Mapping it
 * to `.` makes a build under /home/alice and one under /build/ci come out identical. The flag also covers
 * -fdebug-prefix-map and -fmacro-prefix-map, but the project's own source and include paths are already
 * relative ("./src/..."), so only comp_dir actually changes. The %CWD% token is expanded to the absolute
 * working directory by hook_expand_cwd, so every rule that carries this must register that hook.
 *
 * The compile half rides both the compile and the link: under -flto codegen happens at link time, so the
 * map has to be on the link command too or the debug info generated there keeps the absolute path. The
 * link half additionally pins the build-id off — lld is otherwise free to stamp a note that need not be
 * reproducible, and the shipped binary carries its own integrity CRC and inline -g1 DWARF, so nothing
 * here reads a build-id. ELF only: build-id is meaningless for the mingw PE target, so it is not added to
 * the Windows link, and -ffile-prefix-map is the whole of what the Windows rules can share here.
 *
 * No SOURCE_DATE_EPOCH is needed for the binary: it embeds no build timestamp by design — base_version.c
 * reads the executable's own mtime at runtime rather than baking __DATE__ in. SOURCE_DATE_EPOCH governs
 * the archive mtimes instead; see dist.c.
 */
#define FLAGS_REPRODUCIBLE_COMPILE "-ffile-prefix-map=%CWD%=."
#define FLAGS_REPRODUCIBLE_LINK    "-ffile-prefix-map=%CWD%=.", "-Wl,--build-id=none"

/*
 * Steam is release plus the Steamworks plugin. Same shipping flags, a mode of its own, so the mode can gate overlay and
 * achievements without a second set of build rules.
 */
#define FLAGS_STEAM "-DNYA_EXECUTION_MODE=3", "-DNYA_PLUGIN_STEAM", FLAGS_SHIPPING

// -lz and the brotli encoder are the system libraries HTTP response compression reaches (see
// FLAGS_MODULE_COMPRESSION); they ride the Linux link line only, and are harmless on a binary that does
// not call them. libbrotlienc needs libbrotlicommon behind it.
#define FLAGS_LINUX_X86_64   "-Wl,-rpath,$ORIGIN", "-lz", "-lbrotlienc", "-lbrotlicommon"
#define FLAGS_WINDOWS_X86_64 "-Wl,-subsystem,windows", "-static"

// mold is Linux only, and -rdynamic is what lets the hot reloaded game DLL resolve engine symbols
// out of the executable via dlopen(nullptr).
#define FLAGS_DEBUG_LINUX_X86_64   "-fuse-ld=mold", "-rdynamic"
// the PE equivalent: the executable exports its symbols and produces an import library for the game
// DLL, since Windows has no dlopen(nullptr).
// The import library name is per artifact, or a dev DLL would link against the debug executable's
// symbols and misbehave at runtime.
#define FLAGS_HOTRELOAD_WINDOWS_X86_64 "-fuse-ld=lld", "-Wl,--export-all-symbols"
#define FLAGS_DEBUG_WINDOWS_X86_64     FLAGS_HOTRELOAD_WINDOWS_X86_64, "-Wl,--out-implib," WINDOWS_X86_64_DEBUG_IMPLIB
#define FLAGS_DEV_WINDOWS_X86_64       FLAGS_HOTRELOAD_WINDOWS_X86_64, "-Wl,--out-implib," WINDOWS_X86_64_DEV_IMPLIB

/* Authenticode signing of the shipped .exe. See hook_sign_windows_executable. */
// No certificate is bundled: the real one lives encrypted in secrets/signing.pfx.enc, which CI decrypts
// to a temporary file and names through NYA_SIGNING_PFX below. With neither the env var nor a file here,
// hook_sign_windows_executable leaves the binary unsigned rather than failing — visibly unsigned beats
// silently unsigned. See secrets/README.md.
#define SIGNING_PFX_PATH      ""
#define SIGNING_PFX_PASSWORD  ""
#define SIGNING_TIMESTAMP_URL "http://timestamp.digicert.com"

/** Overrides for the three above, so CI can sign with a real certificate without editing this file. */
#define SIGNING_PFX_PATH_ENV      "NYA_SIGNING_PFX"
#define SIGNING_PFX_PASSWORD_ENV  "NYA_SIGNING_PASSWORD"
#define SIGNING_TIMESTAMP_URL_ENV "NYA_SIGNING_TIMESTAMP_URL"

/*
 * The compiler cache split compile rules launch through. Unset, ccache is used when it runs; empty,
 * "0" or "off" never uses one; anything else names the launcher, which then has to exist.
 */
#define COMPILER_CACHE_PROGRAM "ccache"
#define COMPILER_CACHE_ENV     "NYA_CCACHE"

/* HOST TARGETS */

/* Running always targets the host. Cross compiling to Windows from Linux is a build time convenience; there is nothing sensible to do with the resulting .exe here, so `run` picks the native artifact and the rules it names are selected by host rather than exposed as a choice. These are aliases, not references: a macro body is only looked up where it expands, so naming a build rule here does not require the header that defines it to have been seen. build.h decides that order, once. */

#if OS_WINDOWS
#define HOST_DEBUG_BINARY   WINDOWS_X86_64_DEBUG_BINARY
#define HOST_DEV_BINARY     WINDOWS_X86_64_DEV_BINARY
#define HOST_RELEASE_BINARY WINDOWS_X86_64_BINARY
#define host_build_debug    build_project_debug_windows
#define host_build_dev      build_project_dev_windows
#define host_build_release  build_project_windows_x86_64
#else
#define HOST_DEBUG_BINARY   LINUX_X86_64_DEBUG_BINARY
#define HOST_DEV_BINARY     LINUX_X86_64_DEV_BINARY
#define HOST_RELEASE_BINARY LINUX_X86_64_BINARY
#define host_build_debug    build_project_debug_linux
#define host_build_dev      build_project_dev_linux
#define host_build_release  build_project_linux_x86_64
#endif

/** Sanitizer configuration shared by everything that runs an instrumented binary. */
#define SANITIZER_ENVIRONMENT                                                                                                                        \
    "ASAN_OPTIONS=suppressions=./src/nyangine-build/sanitizers/asan.supp:detect_leaks=1:strict_string_checks=1:halt_on_error=1",                     \
        "LSAN_OPTIONS=suppressions=./src/nyangine-build/sanitizers/lsan.supp", "TSAN_OPTIONS=suppressions=./src/nyangine-build/sanitizers/tsan.supp", \
        "UBSAN_OPTIONS=suppressions=./src/nyangine-build/sanitizers/ubsan.supp:print_stacktrace=1:halt_on_error=1"

/* HOST NATIVE ARTIFACTS */

/* For the two things built to run on this machine right now rather than to be shipped anywhere: the build tool, which compiles itself, and the test binary, which the test runner then executes. */
#if OS_WINDOWS

#define BUILD_TOOL_BINARY "build.exe"

/** What the linker appends to an executable here, so rules name the file it actually writes. */
#define HOST_EXECUTABLE_SUFFIX ".exe"

/*
 * platform/random calls BCryptGenRandom, so everything linking platform needs bcrypt, the build tool
 * included. `#pragma comment(lib)` would keep this beside the call, but clang ignores it for mingw.
 */
#define PLATFORM_LINK_WINDOWS "-lbcrypt"

/*
 * No sanitizers on a Windows host: -fsanitize=leak has no Windows implementation and asan under mingw
 * is not usable. lld because mold is Linux only.
 */
#define FLAGS_HOST_NATIVE       "-fuse-ld=lld", PLATFORM_LINK_WINDOWS

/** FLAGS_HOST_NATIVE split for a compile and a link. Expands to nothing, comma included, like FLAGS_TARGET_WINDOWS_X86_64. */
#define FLAGS_HOST_NATIVE_COMPILE
#define FLAGS_HOST_NATIVE_LINK "-fuse-ld=lld", PLATFORM_LINK_WINDOWS

/** The same host flags without the sanitizers. See the Linux definition for why this exists. */
#define FLAGS_HOST_NATIVE_BENCH "-fuse-ld=lld", PLATFORM_LINK_WINDOWS

#define BACKTRACE_A_HOST        BACKTRACE_A_WINDOWS_X86_64
#define BACKTRACE_INCLUDES_HOST BACKTRACE_INCLUDES_WINDOWS_X86_64

#else

#define BUILD_TOOL_BINARY "build"

#define HOST_EXECUTABLE_SUFFIX ""

#define FLAGS_HOST_NATIVE       FLAGS_DEBUG_LINUX_X86_64, FLAGS_SANITIZE, FLAGS_LINUX_X86_64

/** FLAGS_HOST_NATIVE split for a compile and a link. The compile half carries its own trailing comma, since on Windows it is empty. */
#define FLAGS_HOST_NATIVE_COMPILE FLAGS_SANITIZE,
#define FLAGS_HOST_NATIVE_LINK    FLAGS_DEBUG_LINUX_X86_64, FLAGS_SANITIZE, FLAGS_LINUX_X86_64

/**
 * The same, minus FLAGS_SANITIZE. What a benchmark is built with.
 * */
#define FLAGS_HOST_NATIVE_BENCH FLAGS_DEBUG_LINUX_X86_64, FLAGS_LINUX_X86_64

#define BACKTRACE_A_HOST        BACKTRACE_A_LINUX_X86_64
#define BACKTRACE_INCLUDES_HOST BACKTRACE_INCLUDES_LINUX_X86_64

#endif

// clang-format on
