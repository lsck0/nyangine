#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    b8               used;
    NYA_ConstCString base;

    NYA_ConstCString handles[NYA_RENDER3D_LOD_LEVELS];

    /** Squared at registration, so selection never takes a square root. */
    f32 max_distance_squared[NYA_RENDER3D_LOD_LEVELS];

    u32 level_count;
} _NYA_LodChain;

NYA_INTERNAL _NYA_LodChain _nya_lod_chains[NYA_RENDER3D_LOD_CHAINS] = { 0 };
NYA_INTERNAL u32           _nya_lod_count                           = 0;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The chain for `base`, or null.
 *
 * Compared by string rather than by pointer: an asset handle is usually a generated `#define`, so two
 * call sites naming the same asset may well hold two different pointers to identical text.
 */
NYA_INTERNAL _NYA_LodChain* _nya_lod_find(NYA_ConstCString base) {
    if (base == nullptr) return nullptr;

    for (u32 i = 0; i < NYA_RENDER3D_LOD_CHAINS; i++) {
        _NYA_LodChain* chain = &_nya_lod_chains[i];
        if (!chain->used) continue;
        if (chain->base == base || nya_string_equals(chain->base, base)) return chain;
    }

    return nullptr;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_render3d_lod_register(NYA_ConstCString base_handle, const NYA_Render3DLodLevel* levels, u32 level_count) {
    // See nya_font_register's identical comment: no dedicated init exists for this registry, so
    // this call is where the count first becomes meaningful, guarded against re-registering on
    // every one of what is normally many calls.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("lod_chains", NYA_RENDER3D_LOD_CHAINS, &_nya_lod_count);
        ceiling_registered = true;
    }

    if (base_handle == nullptr || levels == nullptr) return false;
    if (level_count == 0 || level_count > NYA_RENDER3D_LOD_LEVELS) return false;

    /*
     * Refused rather than sorted.
     *
     * Levels out of order would select the wrong rung silently, and a caller that wrote them in the
     * wrong order has almost certainly also got the distances wrong — sorting would hide the mistake
     * rather than fix it.
     */
    f32 previous = 0.0F;
    for (u32 i = 0; i < level_count; i++) {
        if (levels[i].handle == nullptr) return false;
        if (levels[i].max_distance <= previous) return false;

        previous = levels[i].max_distance;
    }

    _NYA_LodChain* chain = _nya_lod_find(base_handle);

    if (chain == nullptr) {
        for (u32 i = 0; i < NYA_RENDER3D_LOD_CHAINS; i++) {
            if (_nya_lod_chains[i].used) continue;

            chain = &_nya_lod_chains[i];
            _nya_lod_count++;
            break;
        }
    }

    if (chain == nullptr) {
        nya_log_warn("No free LOD chain slot for '%s'; " FMTu32 " are in use.", base_handle, (u32)NYA_RENDER3D_LOD_CHAINS);
        return false;
    }

    *chain = (_NYA_LodChain){ .used = true, .base = base_handle, .level_count = level_count };

    for (u32 i = 0; i < level_count; i++) {
        chain->handles[i]              = levels[i].handle;
        chain->max_distance_squared[i] = levels[i].max_distance * levels[i].max_distance;
    }

    return true;
}

void nya_render3d_lod_unregister(NYA_ConstCString base_handle) {
    _NYA_LodChain* chain = _nya_lod_find(base_handle);
    if (chain == nullptr) return;

    *chain = (_NYA_LodChain){ 0 };
    _nya_lod_count--;
}

void nya_render3d_lod_clear(void) {
    for (u32 i = 0; i < NYA_RENDER3D_LOD_CHAINS; i++) _nya_lod_chains[i] = (_NYA_LodChain){ 0 };
    _nya_lod_count = 0;
}

u32 nya_render3d_lod_count(void) {
    return _nya_lod_count;
}

b8 nya_render3d_lod_registered(NYA_ConstCString base_handle) {
    return _nya_lod_find(base_handle) != nullptr;
}

NYA_ConstCString nya_render3d_lod_select_squared(NYA_ConstCString base_handle, f32 distance_squared) {
    _NYA_LodChain* chain = _nya_lod_find(base_handle);

    // No chain means no opinion: the caller gets back exactly what it asked to draw, so every draw can
    // be routed through this without checking first.
    if (chain == nullptr) return base_handle;

    for (u32 i = 0; i < chain->level_count; i++) {
        if (distance_squared <= chain->max_distance_squared[i]) return chain->handles[i];
    }

    // Past the last rung. The final level's distance doubles as the draw distance.
    return nullptr;
}

NYA_ConstCString nya_render3d_lod_select(NYA_ConstCString base_handle, f32 distance) {
    return nya_render3d_lod_select_squared(base_handle, distance * distance);
}

u32 nya_render3d_lod_level_at(NYA_ConstCString base_handle, f32 distance) {
    _NYA_LodChain* chain = _nya_lod_find(base_handle);
    if (chain == nullptr) return 0;

    f32 squared = distance * distance;

    for (u32 i = 0; i < chain->level_count; i++) {
        if (squared <= chain->max_distance_squared[i]) return i;
    }

    return NYA_RENDER3D_LOD_LEVELS;
}
