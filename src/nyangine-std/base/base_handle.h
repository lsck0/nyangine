/**
 * @file base_handle.h
 *
 * A handle is an index into a table and the generation that slot was on when the handle was made.
 * Comparing the generation on every lookup is what turns a reference to something that has since been
 * freed into "no longer there" instead of whichever object next took the slot.
 *
 * Handles live here when two modules above base name the same one and neither is below the other:
 * base is the lowest place both can see, so the type is a shared vocabulary rather than one module
 * reaching across into another. `physics` and `core` are the pair that put NYA_EntityHandle here —
 * physics answers a raycast with an entity, core owns the table that entity is in, and neither has to
 * know the other's header to say so. A handle whose table has exactly one owner stays with that owner;
 * NYA_WindowHandle is in core_types.h for that reason.
 * */
#pragma once

#include "nyangine-std/base/base_types.h"

// TYPES

typedef struct NYA_EntityHandle NYA_EntityHandle;

/**
 * Identifies an entity for as long as it lives.
 *
 * Entities refer to each other constantly, and deferred simulation commands hold references across a
 * barrier, so bumping `generation` on despawn is what keeps a stale reference from silently addressing
 * whichever entity next occupied the slot.
 * */
struct NYA_EntityHandle {
    u32 index;
    u32 generation;
};

#define NYA_ENTITY_HANDLE_NONE ((NYA_EntityHandle){ .index = 0, .generation = 0 })
