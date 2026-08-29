/**
 * Core systems: the tween pool, string hashing, and arena allocation.
 *
 * Hashing is here because `perf.data` put `nya_hash_fnv1a` at 0.62% and `nya_asset_get` at 0.66% —
 * every asset lookup hashes a string handle. Whether that is worth replacing with integer ids emitted
 * by the asset codegen is an open question, and this is the number the answer depends on.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#define TWEENS 256
#define HASHES 1024

static f32 targets[TWEENS];

/* Handles of the shape the asset index generates: a path, hashed on every lookup. */
static NYA_ConstCString handles[] = {
    "./assets/textures/puff.png",       "./assets/models/bender.fbx",      "./assets/sounds/hit.wav",
    "./assets/music/bgm.wav",           "./assets/maps/demo_topdown.tmj",  "./assets/shader/source/mesh3d.frag.hlsl",
    "./assets/fonts/inter.ttf",         "./assets/icons/maps-arrow.svg",
};

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    /*
     * Brought up once for the whole file.
     *
     * An earlier version re-initialised the callback system inside the asset section while the outer
     * one was still up, and tore it down there as well — so the outer teardown ran against a registry
     * that had already been freed, and the process died with SIGSEGV after main returned. Nested
     * lifetimes of a process-wide system are not a thing; one bring-up, one teardown.
     */
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_asset_init();
    nya_system_tween_init();

    defer nya_system_tween_deinit();
    defer nya_system_asset_deinit();
    defer nya_system_events_deinit();
    defer nya_system_callback_deinit();

    nya_bench_begin("tween system");

    // Starting and cancelling, which is what a UI does on every screen change.
    nya_bench("start + cancel 256", TWEENS, {
        for (u32 i = 0; i < TWEENS; i++) (void)nya_tween_f32(&targets[i], 1.0F, 1.0F);
        nya_tween_cancel_all();
        nya_bench_keep(nya_tween_count());
    });

    // The per-frame cost, which is what actually has to fit in a budget.
    for (u32 i = 0; i < TWEENS; i++) (void)nya_tween_f32(&targets[i], 1.0F, 1000.0F, .ease = NYA_EASE_CUBIC_IN_OUT);

    nya_bench("update 256 running", TWEENS, {
        nya_system_tween_update(1.0F / 60.0F);
        nya_bench_keep(targets[0]);
    });

    // An empty pool still walks its slots, so this is the floor a game pays for having the system up.
    nya_tween_cancel_all();
    nya_bench("update, pool empty", 0, {
        nya_system_tween_update(1.0F / 60.0F);
        nya_bench_keep(nya_tween_count());
    });

    if (nya_bench_end() != 0) return 1;

    nya_bench_begin("hashing (what every asset lookup pays)");

    nya_bench("fnv1a asset handle x1024", HASHES, {
        u64 mixed = 0;
        for (u32 i = 0; i < HASHES; i++) mixed ^= nya_hash_fnv1a(handles[i % nya_carray_length(handles)]);
        nya_bench_keep(mixed);
    });

    // The comparison that matters for the integer-id question: what a lookup would cost if the handle
    // were already a number.
    nya_bench("integer mix x1024", HASHES, {
        u64 mixed = 0;
        // Widened intent made explicit: this is a mixing step, and wraparound is what it is for.
        for (u32 i = 0; i < HASHES; i++) mixed ^= ((u64)i * 0x9E3779B97F4A7C15ull) & 0xFFFFFFFFFFFFFFFFull;
        nya_bench_keep(mixed);
    });

    if (nya_bench_end() != 0) return 1;

    // ── The lookup the memo actually sits in front of ──
    {
        /*
         * ⚠ These two numbers do NOT demonstrate the memo, and are kept only to show the call is cheap.
         *
         * Nothing is loaded here, so the dictionary is empty and nya_dict_get short-circuits before it
         * hashes anything — which makes the "miss" case as fast as the hit and the comparison
         * meaningless. Populating it needs real asset loads, which are queued and land at end of frame,
         * so a bench with no frame loop cannot do it.
         *
         * What the memo is worth is argued from the hash it removes: nya_hash_fnv1a over a real asset
         * path measures ~63 ns above, against ~0.3 ns for an integer compare. That is the cost a
         * populated dictionary pays per lookup and the memo does not.
         */
        nya_bench_begin("asset lookup (empty dictionary — see the note in the source)");

        // The common case: the same generated #define, so the same pointer, every frame.
        nya_bench("nya_asset_get x1024, same handles", HASHES, {
            NYA_Asset* last = nullptr;
            for (u32 i = 0; i < HASHES; i++) last = nya_asset_get((NYA_AssetHandle)handles[i % nya_carray_length(handles)]);
            nya_bench_keep(last);
        });

        // The worst case for a pointer-keyed memo: distinct buffers holding identical text, so every
        // lookup misses and falls through to the dictionary.
        static char copies[8][64];
        for (u32 i = 0; i < nya_carray_length(handles); i++) (void)snprintf(copies[i], sizeof(copies[i]), "%s", handles[i]);

        nya_bench("nya_asset_get x1024, copied text", HASHES, {
            NYA_Asset* last = nullptr;
            for (u32 i = 0; i < HASHES; i++) last = nya_asset_get((NYA_AssetHandle)copies[i % nya_carray_length(handles)]);
            nya_bench_keep(last);
        });

        if (nya_bench_end() != 0) return 1;
    }

    nya_bench_begin("arena");

    nya_bench("create, 4096 allocs, destroy", 4096, {
        NYA_Arena* arena = nya_arena_create(.name = "bench_arena");
        for (u32 i = 0; i < 4096; i++) {
            void* block = nya_arena_alloc(arena, 64);
            nya_bench_keep(block);
        }
        nya_arena_destroy(arena);
    });

    return nya_bench_end();
}
