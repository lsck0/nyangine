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

    gnyame_init(argc, argv);
    gnyame_run();
    gnyame_deinit();

    nya_backtrace_deinit();

    return EXIT_SUCCESS;
}
#endif // !NYA_CODE_HOT_RELOAD

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
typedef void(gnyame_init_fn)(s32 argc, NYA_CString* argv);
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

NYA_INTERNAL void  dll_load(void);
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

    dll_load();

    gnyame_init(argc, argv);
    nya_app = nya_app_get();

    /*
     * Started after nya_app exists, not before.
     *
     * The watch thread writes nya_app->should_quit the moment it sees the DLL change, and it used to
     * be created ahead of gnyame_init — which opens windows and queues assets and is not quick. Any
     * rebuild finishing inside that window found nya_app still null and dereferenced it, and a
     * rebuild while the game is starting is precisely what this whole path is for.
     *
     * Nothing between the load above and here needs watching: the DLL was just read.
     */
    pthread_t thread;
    ok = pthread_create(&thread, nullptr, dll_watch_thread_fn, nullptr) == 0;
    nya_assert(ok, "Failed to create DLL watch thread.");

    while (!nya_app->should_quit) {
        gnyame_run();

        if (gnyame_dll_reload_requested) {
            dll_unload();

            // give the compiler time to finish writing the new DLL
            // TODO: can we use the asset systems change detection here to wait until the dll is fully written?
            struct timespec ts = { 0 };
            ts.tv_nsec         = 150UL * 1000UL * 1000UL; // 150 ms
            nanosleep(&ts, nullptr);

            dll_load();
            update_callback_pointers();

            gnyame_dll_reload_requested = false;
            nya_app->should_quit        = false;
            nya_log_debug("Reloaded %s.", DLL_PATH);
        }
    }

    gnyame_deinit();

    gnyame_dll_watch_thread_should_exit = true;
    ok                                  = pthread_join(thread, nullptr) == 0;
    nya_assert(ok, "Failed to join DLL watch thread.");

    dll_unload();

    nya_backtrace_deinit();

    return EXIT_SUCCESS;
}

void dll_load(void) {
    u64 gnyame_dll_last_modified_temp;
    NYA_EXPECT(nya_filesystem_last_modified(DLL_PATH, &gnyame_dll_last_modified_temp));
    gnyame_dll_last_modified = gnyame_dll_last_modified_temp;

    gnyame_dll = dlopen(DLL_PATH, RTLD_NOW | RTLD_GLOBAL);
    nya_assert(gnyame_dll, "Failed to load %s: %s.", DLL_PATH, dlerror());

    gnyame_init   = (gnyame_init_fn*)dlsym(gnyame_dll, "gnyame_init");
    gnyame_run    = (gnyame_run_fn*)dlsym(gnyame_dll, "gnyame_run");
    gnyame_deinit = (gnyame_deinit_fn*)dlsym(gnyame_dll, "gnyame_deinit");
    nya_assert(gnyame_init && gnyame_run && gnyame_deinit, "Failed to load symbols from %s: %s.", DLL_PATH, dlerror());
}

void dll_unload(void) {
    nya_assert(gnyame_dll != nullptr);

    b8 ok = dlclose(gnyame_dll) == 0;
    nya_assert(ok, "Failed to unload %s: %s.", DLL_PATH, dlerror());

    gnyame_dll    = nullptr;
    gnyame_init   = nullptr;
    gnyame_run    = nullptr;
    gnyame_deinit = nullptr;
}

void* dll_watch_thread_fn(void* arg) {
    nya_unused(arg);

    while (!gnyame_dll_watch_thread_should_exit) {
        u64       last_modified;
        NYA_Error result = nya_filesystem_last_modified(DLL_PATH, &last_modified);

        if (result.ok && last_modified != gnyame_dll_last_modified && !gnyame_dll_reload_requested) {
            nya_log_debug("%s was changed, requesting reload.", DLL_PATH);
            gnyame_dll_reload_requested = true;
            nya_app->should_quit        = true;
        } else {
            // compilation might've failed and the DLL might be gone because of that
            // dont explode and just wait for a new one to appear
        }

        struct timespec ts = { 0 };
        ts.tv_nsec         = 50UL * 1000UL * 1000UL; // 50 ms
        nanosleep(&ts, nullptr);
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

/**
 * Windows keeps a loaded DLL locked, so the compiler cannot overwrite the file the process is
 * currently running. The reload therefore loads a copy and leaves the original free to be replaced.
 * */
#if NYA_DEVELOPER
#define DLL_LOADED_PATH "./gnyame.dev.loaded.dll"
#else
#define DLL_LOADED_PATH "./gnyame.debug.loaded.dll"
#endif

typedef void(gnyame_init_fn)(s32 argc, NYA_CString* argv);
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

NYA_INTERNAL void         dll_load(void);
NYA_INTERNAL void         dll_unload(void);
NYA_INTERNAL DWORD WINAPI dll_watch_thread_fn(LPVOID arg);
NYA_INTERNAL void         update_callback_pointers(void);

s32 main(s32 argc, NYA_CString* argv) {
    // First thing in the process, and before any thread is spawned.
    nya_backtrace_init();

    // The game DLL resolves engine symbols out of this executable, which exports them via NYA_API.
    nya_symbols = GetModuleHandleA(nullptr);
    nya_assert(nya_symbols, "Failed to get handle to main executable.");

    dll_load();

    gnyame_init(argc, argv);
    nya_app = nya_app_get();

    // Started after nya_app exists. See the note on the Linux path: the watch thread writes
    // nya_app->should_quit, and creating it ahead of gnyame_init left a window in which a rebuild
    // finishing during startup dereferenced a null pointer.
    HANDLE thread = CreateThread(nullptr, 0, dll_watch_thread_fn, nullptr, 0, nullptr);
    nya_assert(thread != nullptr, "Failed to create DLL watch thread.");

    while (!nya_app->should_quit) {
        gnyame_run();

        if (gnyame_dll_reload_requested) {
            dll_unload();

            // give the compiler time to finish writing the new DLL
            Sleep(150);

            dll_load();
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

void dll_load(void) {
    u64 gnyame_dll_last_modified_temp;
    NYA_EXPECT(nya_filesystem_last_modified(DLL_PATH, &gnyame_dll_last_modified_temp));
    gnyame_dll_last_modified = gnyame_dll_last_modified_temp;

    // Load a copy, so the original stays writable while the game is running.
    NYA_EXPECT(nya_filesystem_copy(DLL_PATH, DLL_LOADED_PATH), "while copying the game DLL for loading");

    gnyame_dll = LoadLibraryA(DLL_LOADED_PATH);
    nya_assert(gnyame_dll, "Failed to load %s: error %lu.", DLL_LOADED_PATH, GetLastError());

    gnyame_init   = (gnyame_init_fn*)(void*)GetProcAddress(gnyame_dll, "gnyame_init");
    gnyame_run    = (gnyame_run_fn*)(void*)GetProcAddress(gnyame_dll, "gnyame_run");
    gnyame_deinit = (gnyame_deinit_fn*)(void*)GetProcAddress(gnyame_dll, "gnyame_deinit");
    nya_assert(gnyame_init && gnyame_run && gnyame_deinit, "Failed to load symbols from %s: error %lu.", DLL_LOADED_PATH, GetLastError());
}

void dll_unload(void) {
    nya_assert(gnyame_dll != nullptr);

    b8 ok = FreeLibrary(gnyame_dll) != 0;
    nya_assert(ok, "Failed to unload %s: error %lu.", DLL_LOADED_PATH, GetLastError());

    gnyame_dll    = nullptr;
    gnyame_init   = nullptr;
    gnyame_run    = nullptr;
    gnyame_deinit = nullptr;
}

DWORD WINAPI dll_watch_thread_fn(LPVOID arg) {
    nya_unused(arg);

    while (!gnyame_dll_watch_thread_should_exit) {
        u64       last_modified;
        NYA_Error result = nya_filesystem_last_modified(DLL_PATH, &last_modified);

        if (result.ok && last_modified != gnyame_dll_last_modified && !gnyame_dll_reload_requested) {
            nya_log_debug("%s was changed, requesting reload.", DLL_PATH);
            gnyame_dll_reload_requested = true;
            nya_app->should_quit        = true;
        } else {
            // compilation might've failed and the DLL might be gone because of that
            // dont explode and just wait for a new one to appear
        }

        Sleep(50);
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
