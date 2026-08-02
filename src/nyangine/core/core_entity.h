#pragma once

#include "nyangine/core/core_event.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_EntityFlag     NYA_EntityFlag;
typedef struct NYA_Entity       NYA_Entity;
typedef struct NYA_EntitySystem NYA_EntitySystem;

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM STRUCT
 * ─────────────────────────────────────────────────────────
 */

struct NYA_EntitySystem {
    NYA_Arena* allocator;
};

/*
 * ─────────────────────────────────────────────────────────
 * ENTITY STRUCT
 * ─────────────────────────────────────────────────────────
 */

enum NYA_EntityFlag {
    NYA_ENTITY_FLAG_NONE = 0,
};

struct NYA_Entity {
    NYA_EntityFlag flags;

    void (*on_create)(void);
    void (*on_destroy)(void);
    void (*on_update)(f32 delta_time_s);
    void (*on_event)(NYA_Event* event);
    void (*on_render)(void);
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_API void nya_system_entity_init(void);
NYA_API void nya_system_entity_deinit(void);

/*
 * ─────────────────────────────────────────────────────────
 * ENTITY FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */
