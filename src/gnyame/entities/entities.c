#include "gnyame/gnyame.h"

/*
 * One translation unit per kind, gathered here the way gnyame.c gathers the layers.
 *
 * A kind's create, destroy, update and draw belong together and belong away from everything else —
 * layers.c had grown four unrelated jobs, and "what is a crate" was one of them. What stays in this
 * file is only what every kind shares.
 */
#include "gnyame/entities/entity_box.c"
#include "gnyame/entities/entity_camera.c"
#include "gnyame/entities/entity_ledge.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * KIND AND FLAGS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

GNY_EntityKind gny_entity_kind(const NYA_Entity* entity) {
    // Null safe because the usual caller is nya_entity_get on a handle that may no longer resolve,
    // and "this is nothing" is a better answer there than a fault.
    if (entity == nullptr) return GNY_ENTITY_NONE;

    return (GNY_EntityKind)entity->type;
}

GNY_EntityFlags gny_entity_flags(const NYA_Entity* entity) {
    if (entity == nullptr) return GNY_ENTITY_FLAG_NONE;

    return (GNY_EntityFlags)entity->flags;
}

b8 gny_entity_is(const NYA_Entity* entity, GNY_EntityKind kind) {
    return gny_entity_kind(entity) == kind;
}

b8 gny_entity_flag_check(const NYA_Entity* entity, GNY_EntityFlags flags) {
    // Every bit, not any bit. Asking for two flags and getting true for one of them is the sort of
    // thing that reads correctly and behaves wrongly.
    return (gny_entity_flags(entity) & flags) == flags;
}
