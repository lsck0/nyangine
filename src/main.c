#include "nyangine/base/base_basic.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RELEASE ENTRY POINT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if !NYA_DEBUG
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "gnyame/gnyame.c"
#include "gnyame/gnyame.h"

s32 main(s32 argc, NYA_CString* argv) {
    gnyame_init(argc, argv);
    gnyame_run();
    gnyame_deinit();

    return EXIT_SUCCESS;
}
#endif // !NYA_DEBUG

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LINUX DEBUG ENTRY POINT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_DEBUG && OS_LINUX
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define DLL_PATH "./gnyame.debug.so"
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

    nya_symbols = dlopen(nullptr, RTLD_NOW | RTLD_GLOBAL);
    nya_assert(nya_symbols, "Failed to open handle to main executable: %s.", dlerror());

    dll_load();

    pthread_t thread;
    ok = pthread_create(&thread, nullptr, dll_watch_thread_fn, nullptr) == 0;
    nya_assert(ok, "Failed to create DLL watch thread.");

    gnyame_init(argc, argv);
    nya_app = nya_app_get();

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
            nya_debug("Reloaded %s.", DLL_PATH);
        }
    }

    gnyame_deinit();

    gnyame_dll_watch_thread_should_exit = true;
    ok                                  = pthread_join(thread, nullptr) == 0;
    nya_assert(ok, "Failed to join DLL watch thread.");

    dll_unload();

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
        u64        last_modified;
        NYA_Result result = nya_filesystem_last_modified(DLL_PATH, &last_modified);

        if (result.error == NYA_ERROR_NONE && last_modified != gnyame_dll_last_modified && !gnyame_dll_reload_requested) {
            nya_debug("%s was changed, requesting reload.", DLL_PATH);
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

    NYA_CallbackArray* callbacks = nya_app->callback_system.callbacks;

    nya_array_foreach (callbacks, callback) {
        if (callback->fn == nullptr || callback->name == nullptr) continue;

        callback->fn = dlsym(gnyame_dll, callback->name);
        if (callback->fn == nullptr) callback->fn = dlsym(nya_symbols, callback->name);

        nya_assert(callback->fn, "Could not find symbol %s in either %s or %s.", callback->name, DLL_PATH, "nyangine");
    }
}

#endif // NYA_DEBUG && OS_LINUX

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WINDOWS DEBUG ENTRY POINT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_DEBUG && OS_WINDOWS
#endif // NYA_DEBUG && OS_WINDOWS
