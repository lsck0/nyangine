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

    // False is a command line that said its piece and is done: `--help`, or one that could not be
    // understood. Nothing was brought up, so there is nothing to take down either.
    if (gnyame_init(argc, argv)) {
        gnyame_run();
        gnyame_deinit();
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

// Debug and developer builds both hot reload, and both are often on disk at once, so they cannot
// share a filename or one would pick up the other's DLL.
#if NYA_DEVELOPER
#define DLL_PATH "./gnyame.dev.so"
#else
#define DLL_PATH "./gnyame.debug.so"
#endif
typedef b8(gnyame_init_fn)(s32 argc, NYA_CString* argv);
typedef void(gnyame_run_fn)(void);
typedef void(gnyame_deinit_fn)(void);

NYA_INTERNAL NYA_App*          nya_app                             = nullptr;
NYA_INTERNAL void*             nya_symbols                         = nullptr;
NYA_INTERNAL void*             gnyame_dll                          = nullptr;
NYA_INTERNAL gnyame_init_fn*   gnyame_init                         = nullptr;
NYA_INTERNAL gnyame_run_fn*    gnyame_run                          = nullptr;
NYA_INTERNAL gnyame_deinit_fn* gnyame_deinit                       = nullptr;
NYA_INTERNAL atomic u64        gnyame_dll_last_modified            = 0;
NYA_INTERNAL atomic b8         gnyame_dll_reload_requested         = false;
NYA_INTERNAL atomic b8         gnyame_dll_watch_thread_should_exit = false;
NYA_INTERNAL u32               gnyame_dll_generation               = 0;

NYA_INTERNAL b8    dll_load(void) __attr_no_discard;
NYA_INTERNAL void  dll_unload(void);
NYA_INTERNAL void* dll_watch_thread_fn(void* arg);
NYA_INTERNAL void  update_callback_pointers(void);

s32 main(s32 argc, NYA_CString* argv) {
    b8 ok;

    // First thing in the process, and before any thread is spawned: libbacktrace wants its state
    // created up front, and the fault handlers should be live for the DLL loading below too.
    nya_backtrace_init();

    nya_symbols = dlopen(nullptr, RTLD_NOW | RTLD_GLOBAL);
    nya_assert(nya_symbols, "Failed to open handle to main executable: %s.", dlerror());

    if (!dll_load()) nya_log_panic("Failed to load %s: %s.", DLL_PATH, dlerror());

    // A command line that said its piece — `--help`, or one that could not be understood — leaves
    // nothing running and nothing to take down. See gnyame.h.
    if (!gnyame_init(argc, argv)) {
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
        gnyame_run(); // NOLINT(clang-analyzer-core.CallAndMessage): dll_load has succeeded, which sets every entry point

        if (gnyame_dll_reload_requested) {
            nya_trace_scope(NYA_TRACE_HOT_RELOAD);

            dll_unload();

            // the watch thread only asks once the file has stopped changing, but a linker can still leave
            // it unreadable for a moment, so a failed open is retried rather than fatal.
            b8 loaded = false;
            for (u32 attempt = 0; attempt < DLL_LOAD_ATTEMPTS && !loaded; attempt++) {
                loaded = dll_load();
                if (!loaded) nanosleep(&(struct timespec){ .tv_nsec = DLL_WATCH_INTERVAL_MS * 1000L * 1000L }, nullptr);
            }
            if (!loaded) nya_log_panic("Failed to reload %s after %d attempts: %s.", DLL_PATH, DLL_LOAD_ATTEMPTS, dlerror());

            update_callback_pointers();

            gnyame_dll_reload_requested = false;
            nya_app->should_quit        = false;
            nya_log_debug("Reloaded %s.", DLL_PATH);
        }
    }

    gnyame_deinit(); // NOLINT(clang-analyzer-core.CallAndMessage): dll_load has succeeded, which sets every entry point

    gnyame_dll_watch_thread_should_exit = true;
    ok                                  = pthread_join(thread, nullptr) == 0;
    nya_assert(ok, "Failed to join DLL watch thread.");

    dll_unload();

    nya_backtrace_deinit();

    return EXIT_SUCCESS;
}

b8 dll_load(void) {
    nya_assert(gnyame_dll == nullptr, "dll_load without dll_unload.");

    u64       modified = 0;
    NYA_Error result   = nya_filesystem_last_modified(DLL_PATH, &modified);
    if (!result.ok) return false;

    char loaded_path[DLL_LOADED_PATH_MAX];
    (void)snprintf(loaded_path, sizeof(loaded_path), "%s.%u", DLL_PATH, gnyame_dll_generation);

    result = nya_filesystem_copy(DLL_PATH, loaded_path);
    if (!result.ok) return false;

    // local, so the new image's calls to its own functions do not bind to an older generation's. The copy
    // is unlinked straight away; the mapping keeps the file alive.
    void* handle = dlopen(loaded_path, RTLD_NOW | RTLD_LOCAL);
    (void)nya_filesystem_delete(loaded_path);
    if (handle == nullptr) return false;

    gnyame_init_fn*   init   = (gnyame_init_fn*)dlsym(handle, "gnyame_init");
    gnyame_run_fn*    run    = (gnyame_run_fn*)dlsym(handle, "gnyame_run");
    gnyame_deinit_fn* deinit = (gnyame_deinit_fn*)dlsym(handle, "gnyame_deinit");
    if (init == nullptr || run == nullptr || deinit == nullptr) {
        (void)dlclose(handle);
        return false;
    }

    gnyame_dll               = handle;
    gnyame_init              = init;
    gnyame_run               = run;
    gnyame_deinit            = deinit;
    gnyame_dll_last_modified = modified;
    gnyame_dll_generation++;
    return true;
}

void dll_unload(void) {
    nya_assert(gnyame_dll != nullptr);

    // the image stays mapped; see DLL_LOADED_PATH_MAX.
    gnyame_dll    = nullptr;
    gnyame_init   = nullptr;
    gnyame_run    = nullptr;
    gnyame_deinit = nullptr;
}

void* dll_watch_thread_fn(void* arg) {
    nya_unused(arg);

    DllSettle settle = { 0 };

    while (!gnyame_dll_watch_thread_should_exit) {
        // a failed build can leave no DLL at all, which is waited out rather than treated as an error.
        u64       modified = 0;
        NYA_Error result   = nya_filesystem_last_modified(DLL_PATH, &modified);

        if (result.ok && !gnyame_dll_reload_requested && dll_settled(&settle, gnyame_dll_last_modified, modified)) {
            nya_log_debug("%s was changed, requesting reload.", DLL_PATH);
            settle                      = (DllSettle){ 0 };
            gnyame_dll_reload_requested = true;
            nya_app->should_quit        = true;
        }

        nanosleep(&(struct timespec){ .tv_nsec = DLL_WATCH_INTERVAL_MS * 1000L * 1000L }, nullptr);
    }

    return nullptr;
}

void update_callback_pointers(void) {
    nya_assert(nya_app != nullptr);
    nya_assert(gnyame_dll != nullptr);

    NYA_ArrayᐸNYA_Callbackᐳ* callbacks = nya_app->callback_system.callbacks;

    nya_array_foreach (callbacks, callback) {
        if (callback->fn == nullptr || callback->name == nullptr) continue;

        callback->fn = dlsym(gnyame_dll, callback->name);
        if (callback->fn == nullptr) callback->fn = dlsym(nya_symbols, callback->name);

        nya_assert(callback->fn, "Could not find symbol %s in either %s or %s.", callback->name, DLL_PATH, "nyangine");
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

#if NYA_DEVELOPER
#define DLL_PATH "./gnyame.dev.dll"
#else
#define DLL_PATH "./gnyame.debug.dll"
#endif

/** Windows locks a loaded DLL, so loading a copy also leaves the original free for the linker. */
#if NYA_DEVELOPER
#define DLL_LOADED_PATH_FORMAT "./gnyame.dev.loaded.%u.dll"
#else
#define DLL_LOADED_PATH_FORMAT "./gnyame.debug.loaded.%u.dll"
#endif

typedef b8(gnyame_init_fn)(s32 argc, NYA_CString* argv);
typedef void(gnyame_run_fn)(void);
typedef void(gnyame_deinit_fn)(void);

NYA_INTERNAL NYA_App*          nya_app                             = nullptr;
NYA_INTERNAL HMODULE           nya_symbols                         = nullptr;
NYA_INTERNAL HMODULE           gnyame_dll                          = nullptr;
NYA_INTERNAL gnyame_init_fn*   gnyame_init                         = nullptr;
NYA_INTERNAL gnyame_run_fn*    gnyame_run                          = nullptr;
NYA_INTERNAL gnyame_deinit_fn* gnyame_deinit                       = nullptr;
NYA_INTERNAL atomic u64        gnyame_dll_last_modified            = 0;
NYA_INTERNAL atomic b8         gnyame_dll_reload_requested         = false;
NYA_INTERNAL atomic b8         gnyame_dll_watch_thread_should_exit = false;
NYA_INTERNAL u32               gnyame_dll_generation               = 0;

NYA_INTERNAL b8           dll_load(void) __attr_no_discard;
NYA_INTERNAL void         dll_unload(void);
NYA_INTERNAL DWORD WINAPI dll_watch_thread_fn(LPVOID arg);
NYA_INTERNAL void         update_callback_pointers(void);

s32 main(s32 argc, NYA_CString* argv) {
    // First thing in the process, and before any thread is spawned.
    nya_backtrace_init();

    // The game DLL resolves engine symbols out of this executable, which exports them via NYA_API.
    nya_symbols = GetModuleHandleA(nullptr);
    nya_assert(nya_symbols, "Failed to get handle to main executable.");

    if (!dll_load()) nya_log_panic("Failed to load %s: error %lu.", DLL_PATH, GetLastError());

    // A command line that said its piece — `--help`, or one that could not be understood — leaves
    // nothing running and nothing to take down. See gnyame.h.
    if (!gnyame_init(argc, argv)) {
        nya_backtrace_deinit();
        return EXIT_SUCCESS;
    }

    nya_app = nya_app_get();

    // Started after nya_app exists. See the note on the Linux path: the watch thread writes
    // nya_app->should_quit, and creating it ahead of gnyame_init left a window in which a rebuild
    // finishing during startup dereferenced a null pointer.
    HANDLE thread = CreateThread(nullptr, 0, dll_watch_thread_fn, nullptr, 0, nullptr);
    nya_assert(thread != nullptr, "Failed to create DLL watch thread.");

    while (!nya_app->should_quit) {
        gnyame_run();

        if (gnyame_dll_reload_requested) {
            nya_trace_scope(NYA_TRACE_HOT_RELOAD);

            dll_unload();

            // see the Linux path: a failed open is retried rather than fatal.
            b8 loaded = false;
            for (u32 attempt = 0; attempt < DLL_LOAD_ATTEMPTS && !loaded; attempt++) {
                loaded = dll_load();
                if (!loaded) Sleep(DLL_WATCH_INTERVAL_MS);
            }
            if (!loaded) nya_log_panic("Failed to reload %s after %d attempts: error %lu.", DLL_PATH, DLL_LOAD_ATTEMPTS, GetLastError());

            update_callback_pointers();

            gnyame_dll_reload_requested = false;
            nya_app->should_quit        = false;
            nya_log_debug("Reloaded %s.", DLL_PATH);
        }
    }

    gnyame_deinit();

    gnyame_dll_watch_thread_should_exit = true;
    (void)WaitForSingleObject(thread, INFINITE);
    (void)CloseHandle(thread);

    dll_unload();

    nya_backtrace_deinit();

    return EXIT_SUCCESS;
}

b8 dll_load(void) {
    nya_assert(gnyame_dll == nullptr, "dll_load without dll_unload.");

    u64       modified = 0;
    NYA_Error result   = nya_filesystem_last_modified(DLL_PATH, &modified);
    if (!result.ok) return false;

    char loaded_path[DLL_LOADED_PATH_MAX];
    (void)snprintf(loaded_path, sizeof(loaded_path), DLL_LOADED_PATH_FORMAT, gnyame_dll_generation);

    result = nya_filesystem_copy(DLL_PATH, loaded_path);
    if (!result.ok) return false;

    HMODULE handle = LoadLibraryA(loaded_path);
    if (handle == nullptr) return false;

    gnyame_init_fn*   init   = (gnyame_init_fn*)(void*)GetProcAddress(handle, "gnyame_init");
    gnyame_run_fn*    run    = (gnyame_run_fn*)(void*)GetProcAddress(handle, "gnyame_run");
    gnyame_deinit_fn* deinit = (gnyame_deinit_fn*)(void*)GetProcAddress(handle, "gnyame_deinit");
    if (init == nullptr || run == nullptr || deinit == nullptr) {
        (void)FreeLibrary(handle);
        return false;
    }

    gnyame_dll               = handle;
    gnyame_init              = init;
    gnyame_run               = run;
    gnyame_deinit            = deinit;
    gnyame_dll_last_modified = modified;
    gnyame_dll_generation++;
    return true;
}

void dll_unload(void) {
    nya_assert(gnyame_dll != nullptr);

    // the image stays mapped; see DLL_LOADED_PATH_MAX.
    gnyame_dll    = nullptr;
    gnyame_init   = nullptr;
    gnyame_run    = nullptr;
    gnyame_deinit = nullptr;
}

DWORD WINAPI dll_watch_thread_fn(LPVOID arg) {
    nya_unused(arg);

    DllSettle settle = { 0 };

    while (!gnyame_dll_watch_thread_should_exit) {
        u64       modified = 0;
        NYA_Error result   = nya_filesystem_last_modified(DLL_PATH, &modified);

        if (result.ok && !gnyame_dll_reload_requested && dll_settled(&settle, gnyame_dll_last_modified, modified)) {
            nya_log_debug("%s was changed, requesting reload.", DLL_PATH);
            settle                      = (DllSettle){ 0 };
            gnyame_dll_reload_requested = true;
            nya_app->should_quit        = true;
        }

        Sleep(DLL_WATCH_INTERVAL_MS);
    }

    return 0;
}

void update_callback_pointers(void) {
    nya_assert(nya_app != nullptr);
    nya_assert(gnyame_dll != nullptr);

    NYA_ArrayᐸNYA_Callbackᐳ* callbacks = nya_app->callback_system.callbacks;

    nya_array_foreach (callbacks, callback) {
        if (callback->fn == nullptr || callback->name == nullptr) continue;

        callback->fn = (void*)GetProcAddress(gnyame_dll, callback->name);
        if (callback->fn == nullptr) callback->fn = (void*)GetProcAddress(nya_symbols, callback->name);

        nya_assert(callback->fn, "Could not find symbol %s in either %s or %s.", callback->name, DLL_PATH, "nyangine");
    }
}

#endif // NYA_CODE_HOT_RELOAD && OS_WINDOWS
