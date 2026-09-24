#include "nyangine/base/base_basic.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RELEASE ENTRY POINT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if !NYA_CODE_HOT_RELOAD
#include "nyangine/nyangine.h"
#include "gnyame/gnyame.h"

#include "nyangine/nyangine.c"
#include "gnyame/gnyame.c"

s32 main(s32 argc, NYA_CString* argv) {
    // First thing in the process: from here on every assertion, panic, thrown error and hardware
    // fault is captured with a stack trace and routed through the central crash sink.
    nya_backtrace_init();

    // Opt-in, and a no-op unless NYA_SUPERVISE is set: lets the crash sink re-exec this process on a
    // fatal rather than let it die. Armed here so a crash during init is already covered.
    nya_supervisor_arm(argc, argv);

    // The same app entry contract the hot-reload host resolves by symbol (core_app_entry.h), called
    // directly here: a shipping build links the default app in rather than loading it, so it names the
    // contract instead of dlopening for it. gnyame's are thin aliases over gnyame_init/run/deinit.
    //
    // False is a command line that said its piece and is done: `--help`, or one that could not be
    // understood. Nothing was brought up, so there is nothing to take down either.
    if (nya_app_entry_init(argc, argv)) {
        nya_app_entry_run();
        nya_app_entry_deinit();
    }

    nya_backtrace_deinit();

    return EXIT_SUCCESS;
}
#endif // !NYA_CODE_HOT_RELOAD

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HOT RELOAD, SHARED
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_CODE_HOT_RELOAD
#include <string.h>

#include "nyangine/base/base_types.h"

/** How often the watch thread looks at the DLL. */
#define DLL_WATCH_INTERVAL_MS 50

/**
 * Polls the modification time has to hold still before a changed DLL is loaded. The linker writes in
 * bursts, so a single changed reading can be a half written file; three polls is 150 ms of quiet.
 * */
#define DLL_SETTLE_POLLS 3

/** How many times a DLL that fails to open is retried, one watch interval apart, before giving up. */
#define DLL_LOAD_ATTEMPTS 40

/*
 * A reloaded DLL is never unloaded. The engine keeps pointers into the game's data, such as string
 * literals passed as names and asset handles, and unmapping the old image would leave them dangling.
 * Each load opens its own copy, since the loader hands back the image it already has for a known path.
 */
#define DLL_LOADED_PATH_MAX 256

/** Where the watch thread is in noticing a new DLL. */
typedef struct {
    u64 candidate_modified;
    u32 stable_polls;
} DllSettle;

/**
 * Feeds one modification time reading in. True once a time different from `loaded_modified` has been
 * read DLL_SETTLE_POLLS times in a row.
 * */
NYA_INTERNAL b8 dll_settled(DllSettle* settle, u64 loaded_modified, u64 modified) {
    if (modified == loaded_modified) {
        *settle = (DllSettle){ 0 };
        return false;
    }

    if (modified != settle->candidate_modified) {
        *settle = (DllSettle){ .candidate_modified = modified, .stable_polls = 1 };
        return false;
    }

    settle->stable_polls++;
    return settle->stable_polls >= DLL_SETTLE_POLLS;
}

/**
 * Which DLL this host loads, worked out from the host's own name.
 *
 * A project builds many app binaries that share this one host, and the app's DLL sits beside the host
 * named after it: `gnyame.debug` loads `gnyame.debug.so`, and a host copied or symlinked to
 * `gnyame-server.debug` loads `gnyame-server.debug.so`. So a shipped or copied binary selects its app
 * purely by argv[0] — nothing about the app is compiled into the host, and the app's own command line
 * (`gnyame serve`, `gnyame export …`) is never mistaken for an app name.
 *
 * The path is the executable's own with the platform's shared-object suffix appended: `strip` drops a
 * trailing `.exe` first on Windows, `append` is `.so` or `.dll`. Truncation is safe; these are short.
 * */
NYA_INTERNAL void dll_path_from_executable(NYA_CString argv0, NYA_ConstCString strip, NYA_ConstCString append, char* out, u64 out_size) {
    u64 length = strlen(argv0);

    if (strip != nullptr) {
        u64 strip_length = strlen(strip);
        if (length >= strip_length && strcmp(argv0 + (length - strip_length), strip) == 0) length -= strip_length;
    }

    (void)snprintf(out, out_size, "%.*s%s", (s32)length, argv0, append);
}
#endif // NYA_CODE_HOT_RELOAD

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LINUX DEBUG ENTRY POINT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_CODE_HOT_RELOAD && OS_LINUX
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

// The app's DLL is the host's own name with `.so` appended, worked out at startup rather than baked in
// so this one host loads whichever app it was named after. See dll_path_from_executable. The debug and
// developer hosts are `gnyame.debug` and `gnyame.dev`, so their DLLs land on `.debug.so`/`.dev.so`
// without a mode switch here: the mode is already in the name the suffix is appended to.
#define DLL_SUFFIX ".so"
NYA_INTERNAL char dll_path[DLL_LOADED_PATH_MAX] = { 0 };

typedef b8(app_entry_init_fn)(s32 argc, NYA_CString* argv);
typedef void(app_entry_run_fn)(void);
typedef void(app_entry_deinit_fn)(void);

NYA_INTERNAL NYA_App*             nya_app                          = nullptr;
NYA_INTERNAL void*                nya_symbols                      = nullptr;
NYA_INTERNAL void*                app_dll                          = nullptr;
NYA_INTERNAL app_entry_init_fn*   app_init                         = nullptr;
NYA_INTERNAL app_entry_run_fn*    app_run                          = nullptr;
NYA_INTERNAL app_entry_deinit_fn* app_deinit                       = nullptr;
NYA_INTERNAL atomic u64           app_dll_last_modified            = 0;
NYA_INTERNAL atomic b8            app_dll_reload_requested         = false;
NYA_INTERNAL atomic b8            app_dll_watch_thread_should_exit = false;
NYA_INTERNAL u32                  app_dll_generation               = 0;

NYA_INTERNAL b8    dll_load(void) __attr_no_discard;
NYA_INTERNAL void  dll_unload(void);
NYA_INTERNAL void* dll_watch_thread_fn(void* arg);
NYA_INTERNAL void  update_callback_pointers(void);

s32 main(s32 argc, NYA_CString* argv) {
    b8 ok;

    // First thing in the process, and before any thread is spawned: libbacktrace wants its state
    // created up front, and the fault handlers should be live for the DLL loading below too.
    nya_backtrace_init();

    // Opt-in, and a no-op unless NYA_SUPERVISE is set: lets the crash sink re-exec this process on a
    // fatal rather than let it die. Armed before any thread, so the environment snapshot is single-threaded.
    nya_supervisor_arm(argc, argv);

    // Which app this host is: its own name plus `.so`, so a binary named for another app loads that
    // app's DLL. Before dll_load, which reads it.
    dll_path_from_executable(argv[0], nullptr, DLL_SUFFIX, dll_path, sizeof(dll_path));

    nya_symbols = dlopen(nullptr, RTLD_NOW | RTLD_GLOBAL);
    nya_assert(nya_symbols, "Failed to open handle to main executable: %s.", dlerror());

    if (!dll_load()) nya_log_panic("Failed to load %s: %s.", dll_path, dlerror());

    // A command line that said its piece — `--help`, or one that could not be understood — leaves
    // nothing running and nothing to take down. See core_app_entry.h.
    if (!app_init(argc, argv)) { // NOLINT(clang-analyzer-core.CallAndMessage): dll_load has succeeded, which sets every entry point
        nya_backtrace_deinit();
        return EXIT_SUCCESS;
    }

    nya_app = nya_app_get();

    /*
     * Started after nya_app exists, not before.
     */
    pthread_t thread;
    ok = pthread_create(&thread, nullptr, dll_watch_thread_fn, nullptr) == 0;
    nya_assert(ok, "Failed to create DLL watch thread.");

    while (!nya_app->should_quit) {
        app_run(); // NOLINT(clang-analyzer-core.CallAndMessage): dll_load has succeeded, which sets every entry point

        if (app_dll_reload_requested) {
            nya_trace_scope(NYA_TRACE_HOT_RELOAD);

            dll_unload();

            // the watch thread only asks once the file has stopped changing, but a linker can still leave
            // it unreadable for a moment, so a failed open is retried rather than fatal.
            b8 loaded = false;
            for (u32 attempt = 0; attempt < DLL_LOAD_ATTEMPTS && !loaded; attempt++) {
                loaded = dll_load();
                if (!loaded) nanosleep(&(struct timespec){ .tv_nsec = DLL_WATCH_INTERVAL_MS * 1000L * 1000L }, nullptr);
            }
            if (!loaded) nya_log_panic("Failed to reload %s after %d attempts: %s.", dll_path, DLL_LOAD_ATTEMPTS, dlerror());

            update_callback_pointers();

            app_dll_reload_requested = false;
            nya_app->should_quit     = false;
            nya_log_debug("Reloaded %s.", dll_path);
        }
    }

    app_deinit(); // NOLINT(clang-analyzer-core.CallAndMessage): dll_load has succeeded, which sets every entry point

    app_dll_watch_thread_should_exit = true;
    ok                               = pthread_join(thread, nullptr) == 0;
    nya_assert(ok, "Failed to join DLL watch thread.");

    dll_unload();

    nya_backtrace_deinit();

    return EXIT_SUCCESS;
}

b8 dll_load(void) {
    nya_assert(app_dll == nullptr, "dll_load without dll_unload.");

    u64       modified = 0;
    NYA_Error result   = nya_filesystem_last_modified(dll_path, &modified);
    if (!result.ok) return false;

    char loaded_path[DLL_LOADED_PATH_MAX];
    (void)snprintf(loaded_path, sizeof(loaded_path), "%s.%u", dll_path, app_dll_generation);

    result = nya_filesystem_copy(dll_path, loaded_path);
    if (!result.ok) return false;

    // local, so the new image's calls to its own functions do not bind to an older generation's. The copy
    // is unlinked straight away; the mapping keeps the file alive.
    void* handle = dlopen(loaded_path, RTLD_NOW | RTLD_LOCAL);
    (void)nya_filesystem_delete(loaded_path);
    if (handle == nullptr) return false;

    app_entry_init_fn*   init   = (app_entry_init_fn*)dlsym(handle, "nya_app_entry_init");
    app_entry_run_fn*    run    = (app_entry_run_fn*)dlsym(handle, "nya_app_entry_run");
    app_entry_deinit_fn* deinit = (app_entry_deinit_fn*)dlsym(handle, "nya_app_entry_deinit");
    if (init == nullptr || run == nullptr || deinit == nullptr) {
        (void)dlclose(handle);
        return false;
    }

    app_dll               = handle;
    app_init              = init;
    app_run               = run;
    app_deinit            = deinit;
    app_dll_last_modified = modified;
    app_dll_generation++;
    return true;
}

void dll_unload(void) {
    nya_assert(app_dll != nullptr);

    // the image stays mapped; see DLL_LOADED_PATH_MAX.
    app_dll    = nullptr;
    app_init   = nullptr;
    app_run    = nullptr;
    app_deinit = nullptr;
}

void* dll_watch_thread_fn(void* arg) {
    nya_unused(arg);

    DllSettle settle = { 0 };

    while (!app_dll_watch_thread_should_exit) {
        // a failed build can leave no DLL at all, which is waited out rather than treated as an error.
        u64       modified = 0;
        NYA_Error result   = nya_filesystem_last_modified(dll_path, &modified);

        if (result.ok && !app_dll_reload_requested && dll_settled(&settle, app_dll_last_modified, modified)) {
            nya_log_debug("%s was changed, requesting reload.", dll_path);
            settle                   = (DllSettle){ 0 };
            app_dll_reload_requested = true;
            nya_app->should_quit     = true;
        }

        nanosleep(&(struct timespec){ .tv_nsec = DLL_WATCH_INTERVAL_MS * 1000L * 1000L }, nullptr);
    }

    return nullptr;
}

void update_callback_pointers(void) {
    nya_assert(nya_app != nullptr);
    nya_assert(app_dll != nullptr);

    NYA_ArrayᐸNYA_Callbackᐳ* callbacks = nya_app->callback_system.callbacks;

    nya_array_foreach (callbacks, callback) {
        if (callback->fn == nullptr || callback->name == nullptr) continue;

        callback->fn = dlsym(app_dll, callback->name);
        if (callback->fn == nullptr) callback->fn = dlsym(nya_symbols, callback->name);

        nya_assert(callback->fn, "Could not find symbol %s in either %s or %s.", callback->name, dll_path, "nyangine");
    }
}

#endif // NYA_CODE_HOT_RELOAD && OS_LINUX

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WINDOWS DEBUG ENTRY POINT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_CODE_HOT_RELOAD && OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

// The app's DLL is the host's own name with `.exe` swapped for `.dll` (see dll_path_from_executable),
// worked out at startup rather than baked in so this one host loads whichever app it was named after.
// The debug and developer hosts are `gnyame.debug.exe`/`gnyame.dev.exe`, so the mode rides along in the
// name the suffix is spliced into and needs no switch here.
#define DLL_STRIP  ".exe"
#define DLL_APPEND ".dll"
NYA_INTERNAL char dll_path[DLL_LOADED_PATH_MAX] = { 0 };

typedef b8(app_entry_init_fn)(s32 argc, NYA_CString* argv);
typedef void(app_entry_run_fn)(void);
typedef void(app_entry_deinit_fn)(void);

NYA_INTERNAL NYA_App*             nya_app                          = nullptr;
NYA_INTERNAL HMODULE              nya_symbols                      = nullptr;
NYA_INTERNAL HMODULE              app_dll                          = nullptr;
NYA_INTERNAL app_entry_init_fn*   app_init                         = nullptr;
NYA_INTERNAL app_entry_run_fn*    app_run                          = nullptr;
NYA_INTERNAL app_entry_deinit_fn* app_deinit                       = nullptr;
NYA_INTERNAL atomic u64           app_dll_last_modified            = 0;
NYA_INTERNAL atomic b8            app_dll_reload_requested         = false;
NYA_INTERNAL atomic b8            app_dll_watch_thread_should_exit = false;
NYA_INTERNAL u32                  app_dll_generation               = 0;

NYA_INTERNAL b8           dll_load(void) __attr_no_discard;
NYA_INTERNAL void         dll_unload(void);
NYA_INTERNAL DWORD WINAPI dll_watch_thread_fn(LPVOID arg);
NYA_INTERNAL void         update_callback_pointers(void);

s32 main(s32 argc, NYA_CString* argv) {
    // First thing in the process, and before any thread is spawned.
    nya_backtrace_init();

    // Opt-in, and a no-op off Linux and unless NYA_SUPERVISE is set: lets the crash sink re-exec this
    // process on a fatal rather than let it die.
    nya_supervisor_arm(argc, argv);

    // Which app this host is: its own name with `.exe` swapped for `.dll`, so a binary named for another
    // app loads that app's DLL. Before dll_load, which reads it.
    dll_path_from_executable(argv[0], DLL_STRIP, DLL_APPEND, dll_path, sizeof(dll_path));

    // The game DLL resolves engine symbols out of this executable, which exports them via NYA_API.
    nya_symbols = GetModuleHandleA(nullptr);
    nya_assert(nya_symbols, "Failed to get handle to main executable.");

    if (!dll_load()) nya_log_panic("Failed to load %s: error %lu.", dll_path, GetLastError());

    // A command line that said its piece — `--help`, or one that could not be understood — leaves
    // nothing running and nothing to take down. See core_app_entry.h.
    if (!app_init(argc, argv)) { // NOLINT(clang-analyzer-core.CallAndMessage): dll_load has succeeded, which sets every entry point
        nya_backtrace_deinit();
        return EXIT_SUCCESS;
    }

    nya_app = nya_app_get();

    // Started after nya_app exists. See the note on the Linux path: the watch thread writes
    // nya_app->should_quit, and creating it ahead of app_init left a window in which a rebuild
    // finishing during startup dereferenced a null pointer.
    HANDLE thread = CreateThread(nullptr, 0, dll_watch_thread_fn, nullptr, 0, nullptr);
    nya_assert(thread != nullptr, "Failed to create DLL watch thread.");

    while (!nya_app->should_quit) {
        app_run();

        if (app_dll_reload_requested) {
            nya_trace_scope(NYA_TRACE_HOT_RELOAD);

            dll_unload();

            // see the Linux path: a failed open is retried rather than fatal.
            b8 loaded = false;
            for (u32 attempt = 0; attempt < DLL_LOAD_ATTEMPTS && !loaded; attempt++) {
                loaded = dll_load();
                if (!loaded) Sleep(DLL_WATCH_INTERVAL_MS);
            }
            if (!loaded) nya_log_panic("Failed to reload %s after %d attempts: error %lu.", dll_path, DLL_LOAD_ATTEMPTS, GetLastError());

            update_callback_pointers();

            app_dll_reload_requested = false;
            nya_app->should_quit     = false;
            nya_log_debug("Reloaded %s.", dll_path);
        }
    }

    app_deinit();

    app_dll_watch_thread_should_exit = true;
    (void)WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);

    dll_unload();

    nya_backtrace_deinit();

    return EXIT_SUCCESS;
}

b8 dll_load(void) {
    nya_assert(app_dll == nullptr, "dll_load without dll_unload.");

    u64       modified = 0;
    NYA_Error result   = nya_filesystem_last_modified(dll_path, &modified);
    if (!result.ok) return false;

    // Windows locks a loaded DLL, so a copy leaves the original free for the linker. The copy is the
    // DLL's own name with the generation spliced in before the extension, e.g.
    // gnyame.debug.dll -> gnyame.debug.loaded.3.dll.
    char loaded_path[DLL_LOADED_PATH_MAX];
    u64  base_length = strlen(dll_path);
    if (base_length >= strlen(DLL_APPEND)) base_length -= strlen(DLL_APPEND);
    (void)snprintf(loaded_path, sizeof(loaded_path), "%.*s.loaded.%u" DLL_APPEND, (s32)base_length, dll_path, app_dll_generation);

    result = nya_filesystem_copy(dll_path, loaded_path);
    if (!result.ok) return false;

    HMODULE handle = LoadLibraryA(loaded_path);
    if (handle == nullptr) return false;

    app_entry_init_fn*   init   = (app_entry_init_fn*)(void*)GetProcAddress(handle, "nya_app_entry_init");
    app_entry_run_fn*    run    = (app_entry_run_fn*)(void*)GetProcAddress(handle, "nya_app_entry_run");
    app_entry_deinit_fn* deinit = (app_entry_deinit_fn*)(void*)GetProcAddress(handle, "nya_app_entry_deinit");
    if (init == nullptr || run == nullptr || deinit == nullptr) {
        (void)FreeLibrary(handle);
        return false;
    }

    app_dll               = handle;
    app_init              = init;
    app_run               = run;
    app_deinit            = deinit;
    app_dll_last_modified = modified;
    app_dll_generation++;
    return true;
}

void dll_unload(void) {
    nya_assert(app_dll != nullptr);

    // the image stays mapped; see DLL_LOADED_PATH_MAX.
    app_dll    = nullptr;
    app_init   = nullptr;
    app_run    = nullptr;
    app_deinit = nullptr;
}

DWORD WINAPI dll_watch_thread_fn(LPVOID arg) {
    nya_unused(arg);

    DllSettle settle = { 0 };

    while (!app_dll_watch_thread_should_exit) {
        u64       modified = 0;
        NYA_Error result   = nya_filesystem_last_modified(dll_path, &modified);

        if (result.ok && !app_dll_reload_requested && dll_settled(&settle, app_dll_last_modified, modified)) {
            nya_log_debug("%s was changed, requesting reload.", dll_path);
            settle                   = (DllSettle){ 0 };
            app_dll_reload_requested = true;
            nya_app->should_quit     = true;
        }

        Sleep(DLL_WATCH_INTERVAL_MS);
    }

    return 0;
}

void update_callback_pointers(void) {
    nya_assert(nya_app != nullptr);
    nya_assert(app_dll != nullptr);

    NYA_ArrayᐸNYA_Callbackᐳ* callbacks = nya_app->callback_system.callbacks;

    nya_array_foreach (callbacks, callback) {
        if (callback->fn == nullptr || callback->name == nullptr) continue;

        callback->fn = (void*)GetProcAddress(app_dll, callback->name);
        if (callback->fn == nullptr) callback->fn = (void*)GetProcAddress(nya_symbols, callback->name);

        nya_assert(callback->fn, "Could not find symbol %s in either %s or %s.", callback->name, dll_path, "nyangine");
    }
}

#endif // NYA_CODE_HOT_RELOAD && OS_WINDOWS
