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
#define CFLAGS        "-std=c2y", "-mavx", "-mavx2", "-mfma", "-mf16c", "-fdefer-ts", "-fenable-matrix", "-ggdb"
// -Wframe-larger-than: a Windows thread gets 1 MB of stack against Linux's 8, so a frame that fits here can
// overflow there, and only CI on Windows would say so. A quarter of that stays one frame's share. Unoptimized
// builds count compound literal temporaries too, so `*big = (T){ 0 }` of a large T is caught; memset it instead.
#define WARNINGS      "-Werror", "-Wall", "-Wextra", "-Wstrict-prototypes", "-Wswitch", "-Wswitch-default", "-Wimplicit-fallthrough", "-Wframe-larger-than=262144", "-Wno-gnu", "-Wno-gcc-compat", "-Wno-initializer-overrides", "-Wno-keyword-macro"
/* A rule that compiles with `-c` takes only compile flags: under -Werror clang rejects a linker flag it cannot use. Link only are LINKER_FLAGS, every *_LINK macro, FLAGS_LINUX_X86_64, FLAGS_WINDOWS_X86_64 and the FLAGS_*_WINDOWS_X86_64 and FLAGS_DEBUG_LINUX_X86_64 linker setups. A link repeats the mode flags, since optimisation, LTO, sanitizers and coverage all need them there too, and clang ignores the preprocessor flags among them. */

// Only the project's own paths. Everything a third party dependency needs lives on its
// NYA_VendorRule instead, so this does not grow as dependencies are added.
#define INCLUDE_PATHS "-I./", "-I./src/"
#define LINKER_FLAGS  "-lm", "-pthread"

/*
 * Which engine modules are optional today, resolved per target rather than per host: `tls` and HTTP
 * compression are on for a Linux target and off for a Windows one, and the build tool that assembles
 * these lists is itself a Linux binary when it cross compiles the Windows target — so a plain
 * `#if OS_WINDOWS` would hand a cross compiled Windows build the Linux module set and look for OpenSSL
 * headers a mingw sysroot does not have. The two target sets are spelled out, and FLAGS_MODULES below
 * picks the host's own for the rules built to run here (the tests, the build tool, the checks).
 *
 * `db` wants sqlite on the include line, which a host tool build does not have, so it is switched on
 * here rather than compiled unconditionally by nyangine.h. The component system in TODO.md's roadmap
 * replaces this with a component list.
 *
 * `tls` links the system's OpenSSL, which a Windows build has no copy of: curl reaches TLS through
 * Schannel there, so that link line carries no libssl. See tls.h for why the library is the system's
 * rather than vendored, and why nya_tls_available answering false is the honest Windows story for now.
 *
 * HTTP response compression reaches the system's zlib and brotli, which a Windows build here has no
 * copy of on the link line — so the feature is Linux only for now and compiles to a no-op elsewhere,
 * the same shape as `tls`. The libraries themselves are added to the Linux link in FLAGS_LINUX_X86_64,
 * next to the rpath, since they are a system dependency and not a vendored archive. NYA_HTTP_COMPRESSION
 * turns on gzip and deflate; NYA_HTTP_COMPRESSION_BROTLI adds `br`.
 *
 * The compression macro is a leading comma rather than a trailing one, with no comma before it in the
 * module lists: that is how a macro that may expand to nothing joins a list without leaving a double
 * comma behind on the target where it is empty, the same trick FLAGS_TARGET_WINDOWS_X86_64 uses.
 */
#define FLAGS_MODULE_TLS_LINUX_X86_64           "-DNYA_MODULE_TLS"
#define FLAGS_MODULE_TLS_WINDOWS_X86_64         "-DNYA_NO_TLS"
#define FLAGS_MODULE_COMPRESSION_LINUX_X86_64   , "-DNYA_HTTP_COMPRESSION", "-DNYA_HTTP_COMPRESSION_BROTLI"
#define FLAGS_MODULE_COMPRESSION_WINDOWS_X86_64

#define FLAGS_MODULES_LINUX_X86_64   "-DNYA_MODULE_DB", FLAGS_MODULE_TLS_LINUX_X86_64 FLAGS_MODULE_COMPRESSION_LINUX_X86_64
#define FLAGS_MODULES_WINDOWS_X86_64 "-DNYA_MODULE_DB", FLAGS_MODULE_TLS_WINDOWS_X86_64 FLAGS_MODULE_COMPRESSION_WINDOWS_X86_64

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
#define FLAGS_PLUGIN_LIST "-DNYA_PLUGIN_CURL", "-DNYA_PLUGIN_DISCORD", "-DNYA_PLUGIN_LUA", "-DNYA_PLUGIN_ACME", FLAGS_PLUGIN_PERMISSIONS
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
// seam nyangine.h and nyangine.c both cut around crypto/tls/net/acme/http, so those compile while core
// and the renderer do not. The module set is the server's — db, tls and HTTP compression, exactly what
// NYA_SERVER_VENDORS_LINUX_X86_64 has libraries for — but deliberately not FLAGS_PLUGIN_LIST: the Lua
// plugin reaches nya_app_get, which is core, so a plugin build would drag the wall back in. An example
// opts into this with a `.headless` marker beside its main.c; see build_headless_server_example in
// example.c and docs/layering-core-split.md, steps 6–7.
#define FLAGS_SERVER_HEADLESS "-DNYA_NO_SDL", "-DNYA_SERVER", "-DNYA_MODULE_TLS" FLAGS_MODULE_COMPRESSION_LINUX_X86_64

/*
 * The WebAssembly target, the seed of the CSR path. Off the critical path and its own command like
 * `./build vendor`: emcc is not part of the default toolchain and not every checkout has it, so
 * nothing here is reached by a native build or by `./build check`. See wasm_runner.
 *
 * A set of flags of its own rather than CFLAGS: CFLAGS carries -mavx/-mfma/-mf16c, which are x86 and
 * which emcc rejects. What the demo needs is the wasm target's own list below — which does share two
 * language flags with CFLAGS, -fdefer-ts (the engine's deferred `defer` statement) and -fenable-matrix
 * (the matrix_type extension the math headers declare types with), since the engine source these now
 * pull in uses both and emcc's clang accepts both.
 */
#define EMCC                   "emcc"
#define WASM_OUTPUT_DIRECTORY  "./web"
#define WASM_DEMO_SOURCE       "./src/web/wasm_demo.c"
// emcc derives the .wasm from the .js stem, so naming the .js names both; the verifier checks each.
#define WASM_JS_OUTPUT         WASM_OUTPUT_DIRECTORY "/nyangine.js"
#define WASM_WASM_OUTPUT       WASM_OUTPUT_DIRECTORY "/nyangine.wasm"
// The one C symbol the page calls. Named here so the -sEXPORTED_FUNCTIONS below and the verifier that
// greps the loader for it cannot drift apart.
#define WASM_EXPORTED_SYMBOL   "nyangine_demo"
// The platform/web seam's proof-of-life export, beside the serde demo: it drives the clock, CSPRNG,
// storage and the fetch/WebSocket seams and hands their results back. See nyangine_web_probe.
#define WASM_WEB_PROBE_SYMBOL  "nyangine_web_probe"

// MODULARIZE so the loader is a factory the page instantiates when it chooses, and ENVIRONMENT=web,node
// so the same .js both loads in a browser and runs under node, which is how `./build wasm` verifies it.
// ALLOW_MEMORY_GROWTH because the arena grows its regions with malloc and the module should not fail a
// larger document later.
//
// NYA_WASM_WITH_ENGINE switches wasm_demo.c from its stand-in to the real path: the engine's own arena,
// NYA_Object and JSON serde, compiled from the leaf translation units that file includes. -fdefer-ts
// and -fenable-matrix are the two CFLAGS language flags that engine source needs (see the comment
// above). NYA_NO_SDL is set inside wasm_demo.c, before it pulls the headers, so it is not repeated here.
#define FLAGS_WASM                                                     \
    "-std=c2y", "-O2", "-fdefer-ts", "-fenable-matrix",                \
    "-DNYA_WASM_WITH_ENGINE",                                          \
    /* the web profile: only DTO headers cross to the client, and a model or so header refuses to */ \
    /* compile here through base_web_profile.h, so the server storage layout cannot reach wasm. */ \
    "-DNYA_WEB_PROFILE",                                              \
    /* the same warning suppressions CFLAGS carries: the engine source */ \
    /* the wasm path now compiles trips these exactly as the native build does. */ \
    "-Wno-gcc-compat", "-Wno-initializer-overrides", "-Wno-keyword-macro", \
    /* wasm32's size_t is a 32-bit unsigned long, where the LP64 native build's is 64-bit, so a %zu */ \
    /* paired with a u64 in an engine format string is exact natively but mismatched here. The call */ \
    /* sites are vetted on the native -Werror build; the difference is ABI, not a bug, so silence it. */ \
    "-Wno-format",                                                    \
    "-sEXPORTED_FUNCTIONS=_" WASM_EXPORTED_SYMBOL ",_" WASM_WEB_PROBE_SYMBOL, \
    "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString",             \
    "-sMODULARIZE=1", "-sEXPORT_NAME=createNyangineModule",            \
    "-sENVIRONMENT=web,node", "-sALLOW_MEMORY_GROWTH=1"

/*
 * The second WebAssembly target, the CSR bridge: the immediate-mode UI component compiled to wasm and
 * driven from the DOM, no server round trip. Its own command and rule beside `./build wasm`, for the same
 * reason — emcc is off the default toolchain — and its own source, outputs and exports so the two never
 * collide. See wasm_ui_runner and src/web/wasm_ui.c.
 */
#define WASM_UI_SOURCE     "./src/web/wasm_ui.c"
#define WASM_UI_JS_OUTPUT  WASM_OUTPUT_DIRECTORY "/nyangine_ui.js"
#define WASM_UI_WASM_OUTPUT WASM_OUTPUT_DIRECTORY "/nyangine_ui.wasm"
// The two C symbols the page calls, named here so the -sEXPORTED_FUNCTIONS below and the verifier that
// greps the loader cannot drift. render() draws one pass to HTML; event() feeds a click back in.
#define WASM_UI_RENDER_SYMBOL "nyangine_ui_render"
#define WASM_UI_EVENT_SYMBOL  "nyangine_ui_event"

/*
 * Unlike the headless wasm_demo, the UI reads its state through NYA_App, whose type embeds the renderer,
 * asset, physics and world systems by value — so the whole engine header graph has to *parse*, which
 * needs the vendored SDL3/box2d/box3d/ufbx/SDL_ttf/image/mixer headers on the include line. Nothing from
 * those libraries is compiled or linked: wasm_ui.c includes only the leaf .c files the ui path reaches,
 * and the little SDL those two core files carry is gated off under OS_WASM. See the file's own comment.
 */
#define WASM_UI_VENDOR_INCLUDES                                        \
    "-I./vendor/sdl/include", "-I./vendor/box2d/include",             \
    "-I./vendor/box3d/include", "-I./vendor/ufbx",                    \
    "-I./vendor/sdl-ttf/include", "-I./vendor/sdl-image/include",     \
    "-I./vendor/sdl-mixer/include"

// The same base as FLAGS_WASM (language flags, warning suppressions, MODULARIZE, memory growth), but with
// the two UI exports and a factory name of its own, and without -DNYA_WASM_WITH_ENGINE, which is
// wasm_demo.c's stand-in/real switch and means nothing here. NYA_HEADLESS is set inside wasm_ui.c.
#define FLAGS_WASM_UI                                                  \
    "-std=c2y", "-O2", "-fdefer-ts", "-fenable-matrix",                \
    "-DNYA_WEB_PROFILE",                                              \
    "-Wno-gcc-compat", "-Wno-initializer-overrides", "-Wno-keyword-macro", "-Wno-format", \
    WASM_UI_VENDOR_INCLUDES,                                           \
    "-sEXPORTED_FUNCTIONS=_" WASM_UI_RENDER_SYMBOL ",_" WASM_UI_EVENT_SYMBOL, \
    "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString",             \
    "-sMODULARIZE=1", "-sEXPORT_NAME=createNyangineUiModule",          \
    "-sENVIRONMENT=web,node", "-sALLOW_MEMORY_GROWTH=1"

/*
 * The third WebAssembly target, the GAME slice: the engine's 2D render path — a cleared background and a
 * textured sprite — drawn to a real WebGL2 canvas through the SDL_GPU → GLES3 shim in
 * src/nyangine/renderer/gpu_gles, driven by emscripten_set_main_loop. Its own command, source, outputs
 * and exports beside `./build wasm` and `./build wasm-ui`, for the same reason the other two stand apart:
 * emcc is off the default toolchain. See wasm_game_runner and src/web/wasm_game.c.
 */
#define WASM_GAME_SOURCE      "./src/web/wasm_game.c"
#define WASM_GAME_JS_OUTPUT   WASM_OUTPUT_DIRECTORY "/nyangine_game.js"
#define WASM_GAME_WASM_OUTPUT WASM_OUTPUT_DIRECTORY "/nyangine_game.wasm"
// The self-check export the loader must name (the frame-sequence assertion, callable from node). main()
// runs setup + the browser main loop; this is what a headless node run calls to prove the shim ran.
#define WASM_GAME_SYMBOL      "nyangine_game_selfcheck"
// The 3D self-check export: the off-screen depth/MSAA/resolve frame's call-sequence assertion, callable from
// node beside the 2D one. See nyangine_game3d_selfcheck in src/web/wasm_game.c.
#define WASM_GAME_SYMBOL_3D   "nyangine_game3d_selfcheck"
// The live-scene self-check export: one deterministic frame of the moving, interactive scene (the orbit ring
// plus the player, across both 2D pipelines), asserting the batch's own vertex/index/draw-call counts.
#define WASM_GAME_SYMBOL_SCENE "nyangine_game_scene_selfcheck"

/*
 * The two compiled GLSL ES 300 shaders the 2D path needs (batch2d vertex + textured fragment), baked into
 * the module's virtual filesystem at build time with --embed-file. The shim compiles this source at
 * runtime with glShaderSource/glCompileShader; nothing reads a shader from the host at run time. The
 * "src@dst" form maps each onto a short virtual path the demo opens.
 */
#define WASM_GAME_SHADER_EMBEDS                                                                    \
    "--embed-file", "assets/shader/compiled/batch2d.vert.glsl@shaders/batch2d.vert.glsl",          \
    "--embed-file", "assets/shader/compiled/textured.frag.glsl@shaders/textured.frag.glsl",        \
    "--embed-file", "assets/shader/compiled/shape.frag.glsl@shaders/shape.frag.glsl"

// The same base as FLAGS_WASM_UI (language flags, warning suppressions, MODULARIZE, memory growth, the
// vendored header roots NYA_App's graph needs to parse), plus the WebGL2/GLES3 switches the shim's
// context and glDrawElements path require, the two shaders embedded, and this target's own export and
// factory name. NYA_HEADLESS is set inside wasm_game.c (no GPU *device path in the headers*, since the
// device the demo makes is the shim's, not SDL's). NYA_PERF_FORCE_NODEBUG + NYA_TRACE_FORCE_DISABLED
// compile the timers and trace scopes out: render2d.c's flush opens both, and a wasm slice measures
// nothing, so this drops the perf/trace runtime leaves rather than dragging them into the shim TU.
#define FLAGS_WASM_GAME                                                 \
    "-std=c2y", "-O2", "-fdefer-ts", "-fenable-matrix",                \
    "-DNYA_WEB_PROFILE",                                              \
    "-DNYA_PERF_FORCE_NODEBUG", "-DNYA_TRACE_FORCE_DISABLED",          \
    "-Wno-gcc-compat", "-Wno-initializer-overrides", "-Wno-keyword-macro", "-Wno-format", \
    WASM_UI_VENDOR_INCLUDES,                                           \
    "-sUSE_WEBGL2=1", "-sFULL_ES3=1", "-sMIN_WEBGL_VERSION=2", "-sMAX_WEBGL_VERSION=2", \
    WASM_GAME_SHADER_EMBEDS,                                           \
    "-sEXPORTED_FUNCTIONS=_main,_" WASM_GAME_SYMBOL ",_" WASM_GAME_SYMBOL_3D ",_" WASM_GAME_SYMBOL_SCENE, \
    "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString",             \
    "-sMODULARIZE=1", "-sEXPORT_NAME=createNyangineGameModule",        \
    "-sENVIRONMENT=web,node", "-sALLOW_MEMORY_GROWTH=1"

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
// Deliberately not under assets/: that tree is walked by the asset indexer and embedded into
// assets.c, which would put the private key inside the shipped binary.
#define SIGNING_PFX_PATH      "./.signing/sample.pfx"
#define SIGNING_PFX_PASSWORD  "nyangine-sample-certificate"
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
    "ASAN_OPTIONS=suppressions=./.sanitizers/asan.supp:detect_leaks=1:strict_string_checks=1:halt_on_error=1",                                       \
        "LSAN_OPTIONS=suppressions=./.sanitizers/lsan.supp", "TSAN_OPTIONS=suppressions=./.sanitizers/tsan.supp",                                    \
        "UBSAN_OPTIONS=suppressions=./.sanitizers/ubsan.supp:print_stacktrace=1:halt_on_error=1"

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
