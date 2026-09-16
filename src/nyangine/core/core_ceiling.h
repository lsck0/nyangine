/**
 * @file core_ceiling.h
 *
 * ```c
 * // once, wherever the pool's own counter first becomes meaningful:
 * nya_ceiling_register("tweens", NYA_TWEEN_MAX, &_nya_tween_system.count);
 * // a few times a frame, from a HUD row:
 * for (u32 i = 0; i < nya_ceiling_count(); i++) {
 *     printf("%s: %u/%u\n", nya_ceiling_name_at(i), nya_ceiling_live_at(i), nya_ceiling_capacity_at(i));
 * }
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

/** How many ceilings are registered. */
NYA_API u32 nya_ceiling_count(void) __attr_no_discard;

/** Sorted by fullness (live/capacity), fullest first. */
NYA_API NYA_ConstCString nya_ceiling_name_at(u32 index) __attr_no_discard;
NYA_API u32              nya_ceiling_capacity_at(u32 index) __attr_no_discard;
NYA_API u32              nya_ceiling_live_at(u32 index) __attr_no_discard;

#ifdef NYA_TESTING
/** Returns the registry to its just-linked state: no entries. Test-only, same reasoning as
 *  _nya_system_registry_reset_for_test in core_system.h. */
NYA_INTERNAL void _nya_ceiling_registry_reset_for_test(void);
#endif
