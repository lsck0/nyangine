/**
 * @file base_ceiling.h
 *
 * ```c
 * // once, wherever the pool's own counter first becomes meaningful:
 * nya_ceiling_register("tweens", NYA_TWEEN_MAX, &_nya_tween_system.count);
 * // a few times a frame, from a HUD row:
 * for (u32 i = 0; i < nya_ceiling_count(); i++) {
 *     printf("%s: %u/%u\n", nya_ceiling_name_at(i), nya_ceiling_live_at(i), nya_ceiling_capacity_at(i));
 * }
 *
 * // a running byte count with no fixed capacity to be full against:
 * nya_gauge_register("gpu_textures", &_nya_gpu_memory.bytes[NYA_GPU_MEMORY_TEXTURE]);
 * ```
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How many ceilings can register, ever.
 * */
#define NYA_CEILING_REGISTRY_MAX 48

/**
 * How many byte gauges can register, ever. A gauge is a subsystem's total, so there are only a few.
 * */
#ifndef NYA_GAUGE_REGISTRY_MAX
#define NYA_GAUGE_REGISTRY_MAX 16
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Registers a ceiling for visibility: a name, its fixed capacity, and a pointer to the existing live
 * counter. `live` must outlive the registration, so it is usually a static counter.
 * */
NYA_API void nya_ceiling_register(NYA_ConstCString name, u32 capacity, const u32* live);

/**
 * Removes the ceiling registered under `name`, so a subsystem retracts it before the counter it points
 * at is freed — otherwise the registry keeps a dangling `live` a later query would read. A name that is
 * not registered is a no-op. If a name was registered more than once, one registration is removed.
 * */
NYA_API void nya_ceiling_unregister(NYA_ConstCString name);

/** How many ceilings are registered. */
NYA_API u32 nya_ceiling_count(void) __attr_no_discard;

/** Sorted by fullness (live/capacity), fullest first. */
NYA_API NYA_ConstCString nya_ceiling_name_at(u32 index) __attr_no_discard;
NYA_API u32              nya_ceiling_capacity_at(u32 index) __attr_no_discard;
NYA_API u32              nya_ceiling_live_at(u32 index) __attr_no_discard;

/**
 * Registers a byte gauge: a name and a pointer to an existing running byte count. It has no capacity,
 * so it is shown rather than ranked. `bytes` must outlive the registration, like a ceiling's counter.
 * */
NYA_API void nya_gauge_register(NYA_ConstCString name, const u64* bytes);

/** How many gauges are registered. */
NYA_API u32 nya_gauge_count(void) __attr_no_discard;

/** In registration order, so a HUD row does not move. */
NYA_API NYA_ConstCString nya_gauge_name_at(u32 index) __attr_no_discard;
NYA_API u64              nya_gauge_bytes_at(u32 index) __attr_no_discard;

#ifdef NYA_TESTING
/** Returns the registry to its just-linked state: no ceilings and no gauges. Test-only: registration is
 *  one way, so a test that fills the registry would otherwise poison every test after it. */
NYA_INTERNAL void _nya_ceiling_registry_reset_for_test(void);
#endif
