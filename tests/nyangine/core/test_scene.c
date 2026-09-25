/**
 * Scene persistence: a whole world to a document and back, and the same through a save file.
 *
 * The round trip is stated as a law rather than as a list of fields: a world written, read back and
 * written again has to produce the same document, byte for byte. That covers every field of
 * NYA_SceneEntity without naming one, so a field added to the record is covered the day it is added.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_init.h"

#include <stdlib.h>

/** Entities the generated world holds. Enough for hierarchies and sparse slots, small enough to read a failure. */
#define WORLD_ENTITY_COUNT 64

/** How many of them are given a parent. */
#define WORLD_PARENTED_COUNT 20

/** Names are held by the test, not by the entities, so they outlive every world the test builds. */
static char entity_names[WORLD_ENTITY_COUNT][NYA_SCENE_NAME_MAX];

/**
 * A counter hashed rather than an RNG pulled, so a failure is the same failure on the next run. The
 * engine's own hash, rather than a second one written here, so the test has nothing of its own to be
 * wrong about.
 * */
static u64 mix(u64 value) {
    return nya_hash_wyhash(&value, sizeof(value));
}

/** A number in [0, 1) from the counter, for the fields where any value does. */
static f32 unit(u64 counter) {
    return (f32)(mix(counter) >> 40U) / (f32)(1U << 24U);
}

static NYA_String* document_text(NYA_Arena* arena, const NYA_Object* object) {
    NYA_String* text = nya_serialize(arena, object, NYA_SERDE_FORMAT_NYA, NYA_SERDE_PRETTY);
    nya_assert(text != nullptr);

    return text;
}

static b8 same_text(const NYA_String* a, const NYA_String* b) {
    if (a->length != b->length) return false;

    return nya_memcmp(a->items, b->items, a->length) == 0;
}

/**
 * Fills the current world with entities that exercise every part of a record: names, types, flags,
 * transforms, motion, all three visual kinds and a light.
 * */
static void build_world(void) {
    NYA_EntityHandle spawned[WORLD_ENTITY_COUNT];

    for (u32 i = 0; i < WORLD_ENTITY_COUNT; i++) {
        (void)snprintf(entity_names[i], sizeof(entity_names[i]), "entity_" FMTu32, i);

        NYA_EntityVisualKind kind = (NYA_EntityVisualKind)(mix(i) % NYA_ENTITY_VISUAL_KIND_COUNT);

        NYA_EntityVisual visual = {
            .kind = kind,

            .sprite = {
                .texture       = kind == NYA_ENTITY_VISUAL_NONE ? nullptr : "textures/test.png",
                .source_x      = unit(i + 1) * 64.0F,
                .source_y      = unit(i + 2) * 64.0F,
                .source_width  = 16.0F,
                .source_height = 16.0F,
                .origin        = { 0.5F, 0.5F },
                .scale         = { unit(i + 3) + 0.5F, 1.0F },
                .rotation      = unit(i + 4),
                .flip_x        = (mix(i + 5) & 1U) != 0,
                .flip_y        = (mix(i + 6) & 1U) != 0,
                .tint          = { unit(i + 7), unit(i + 8), unit(i + 9), 1.0F },
            },

            .atlas = {
                .texture      = kind == NYA_ENTITY_VISUAL_ANIMATION ? "textures/sheet.png" : nullptr,
                .frame_width  = 16,
                .frame_height = 16,
                .columns      = 8,
                .rows         = 4,
            },

            .size          = { 1.0F, 2.0F, 3.0F },
            .color         = { 0.2F, 0.4F, 0.6F, 1.0F },
            .z_order       = unit(i + 10) * 10.0F,
            .y_sorted      = (mix(i + 11) & 1U) != 0,
            .y_sort_anchor = unit(i + 12),
        };

        spawned[i] = nya_entity_spawn(
            .name     = entity_names[i],
            .type     = (u32)(mix(i + 13) % 7),
            .flags    = mix(i + 14),
            .position = { unit(i + 15) * 100.0F, unit(i + 16) * 100.0F, unit(i + 17) * 100.0F },
            .scale    = { 1.0F, 1.0F, 1.0F },
            .velocity = { unit(i + 18), unit(i + 19), unit(i + 20) },
            .visual   = visual,

            // Every third entity emits, so both the zeroed light and a real one are covered.
            .light = i % 3 == 0 ? (NYA_Light2D){ .radius = unit(i + 21) * 50.0F, .intensity = unit(i + 22), .color = { 1, 1, 0.8F, 1 } }
                                : (NYA_Light2D){ 0 }
        );

        nya_assert(nya_entity_is_valid(spawned[i]));
    }

    // Parents picked from earlier entities only, so the hierarchy is a forest rather than a cycle.
    for (u32 i = 1; i <= WORLD_PARENTED_COUNT; i++) {
        u32 child  = (u32)(mix(i + 100) % (WORLD_ENTITY_COUNT - 1)) + 1;
        u32 parent = (u32)(mix(i + 200) % child);

        if (nya_entity_is_ancestor(spawned[child], spawned[parent])) continue;

        (void)nya_entity_parent_set(spawned[child], spawned[parent]);
    }
}

/** The entity called `name` in the current world, or null. */
static NYA_Entity* find_by_name(NYA_ConstCString name) {
    nya_entity_foreach (entity) {
        if (entity->name != nullptr && nya_string_equals(entity->name, name)) return entity;
    }

    return nullptr;
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    NYA_Arena* arena = nya_arena_create(.name = "test_scene");
    defer      nya_arena_destroy(arena);

    /* Both variables, since Linux reads XDG_DATA_HOME and Windows APPDATA. Pointed at a scratch directory so a save does not land in the developer's real data directory. Same reasoning as test_settings.c. */
    NYA_String* temp_root = nullptr;
    NYA_EXPECT(nya_filesystem_temp_directory(arena, &temp_root));

    NYA_String* save_home         = nya_path_join(arena, nya_string_to_cstring(arena, temp_root), "nyangine-test-scene");
    NYA_CString save_home_cstring = nya_string_to_cstring(arena, save_home);

    (void)nya_filesystem_delete_recursive(save_home_cstring);

    nya_assert(nya_host_environment_add("XDG_DATA_HOME", save_home_cstring));
    nya_assert(nya_host_environment_add("APPDATA", save_home_cstring));

    NYA_EXPECT(nya_system_save_init());
    defer nya_system_save_deinit();

    NYA_World* origin = nya_world_create();
    (void)nya_world_set(origin);
    defer nya_world_destroy(origin);

    build_world();

    // TEST: a world written, read back and written again is the same document
    printf("TEST: scene round trip\n");

    NYA_World* replica = nya_world_create();
    defer      nya_world_destroy(replica);

    {
        NYA_Object* first = nya_scene_to_object(arena, origin);
        nya_check(first != nullptr, "a world should produce a document");

        NYA_EXPECT(nya_scene_from_object(replica, first));

        NYA_Object* second = nya_scene_to_object(arena, replica);

        nya_check(same_text(document_text(arena, first), document_text(arena, second)),
                  "a world read back and written again should be the same document");

        // The checksum is over the tree rather than the text, so this is the same law stated against the other half of the format.
        nya_check(nya_serde_nya_checksum(first) == nya_serde_nya_checksum(second), "and should hash the same");

        printf("  PASSED\n");
    }

    // TEST: the hierarchy survives, by name rather than by handle
    printf("TEST: hierarchy round trip\n");
    {
        NYA_World* previous = nya_world_set(origin);

        u32 parented_here = 0;

        nya_entity_foreach (entity) {
            if (!nya_entity_is_valid(entity->parent)) continue;

            parented_here++;

            NYA_ConstCString child_name  = entity->name;
            NYA_ConstCString parent_name = nya_entity_get(entity->parent)->name;

            (void)nya_world_set(replica);

            NYA_Entity* child = find_by_name(child_name);

            nya_check(child != nullptr, "'%s' should have come back", child_name);

            if (child != nullptr) {
                NYA_Entity* parent = nya_entity_get(child->parent);

                nya_check(parent != nullptr && nya_string_equals(parent->name, parent_name), "'%s' should still be under '%s'",
                          child_name, parent_name);
            }

            (void)nya_world_set(origin);
        }

        nya_check(parented_here > 0, "the generated world should have parented something to test");

        (void)nya_world_set(previous);
        printf("  PASSED\n");
    }

    // TEST: sparse slots, which is what a world that has despawned looks like
    printf("TEST: sparse slots round trip\n");
    {
        (void)nya_world_set(origin);

        // Despawns leave holes in the slot table, so the entities are no longer in slots 0..n. That is exactly what the document's own numbering has to be immune to. A childless entity is picked on purpose: despawning a parent takes its subtree with it.
        for (u32 i = 1; i < WORLD_ENTITY_COUNT; i += 7) {
            NYA_Entity* victim = find_by_name(entity_names[i]);
            if (victim == nullptr || victim->child_count > 0) continue;

            nya_entity_despawn(victim->handle);
        }

        NYA_Object* first = nya_scene_to_object(arena, origin);

        NYA_World* fresh = nya_world_create();
        defer      nya_world_destroy(fresh);

        NYA_EXPECT(nya_scene_from_object(fresh, first));

        NYA_Object* second = nya_scene_to_object(arena, fresh);

        nya_check(same_text(document_text(arena, first), document_text(arena, second)),
                  "a world with holes in its table should still write the same document after a round trip");

        u32 expected = 0;
        (void)nya_world_set(origin);
        nya_entity_foreach (entity) {
            nya_unused(entity);
            expected++;
        }

        u32 actual = 0;
        (void)nya_world_set(fresh);
        nya_entity_foreach (entity) {
            nya_unused(entity);
            actual++;
        }

        nya_check(actual == expected, "every entity should have come back, got " FMTu32 " of " FMTu32, actual, expected);

        // A world that has already held entities spawns into slots it freed earlier, so the order of the list that comes back out follows the table rather than the file. What it holds is the same either way, which is the part that matters and the part asserted here.
        NYA_EXPECT(nya_scene_from_object(replica, first));

        u32 reused = 0;
        (void)nya_world_set(replica);
        nya_entity_foreach (entity) {
            nya_unused(entity);
            reused++;
        }

        nya_check(reused == expected, "and into a reused world too, got " FMTu32 " of " FMTu32, reused, expected);

        (void)nya_world_set(origin);
        printf("  PASSED\n");
    }

    // TEST: a pending despawn is not a thing a scene holds
    printf("TEST: despawning is not persisted\n");
    {
        (void)nya_world_set(origin);

        NYA_Entity* victim = find_by_name(entity_names[0]);
        nya_check(victim != nullptr, "entity_0 should still be here");

        nya_entity_despawn_deferred(victim->handle);

        NYA_SceneEntity record = { 0 };
        nya_scene_entity_from(&record, victim);

        nya_check((record.state & NYA_ENTITY_STATE_DESPAWNING) == 0, "a record should not carry a pending despawn");
        nya_check((victim->state & NYA_ENTITY_STATE_DESPAWNING) != 0, "and the live entity should be unchanged");

        printf("  PASSED\n");
    }

    // TEST: a name longer than a record holds is cut rather than lost
    printf("TEST: over-long names\n");
    {
        (void)nya_world_set(replica);
        nya_entity_clear();

        static char long_name[NYA_SCENE_NAME_MAX * 2];
        for (u64 i = 0; i < sizeof(long_name) - 1; i++) long_name[i] = 'a';
        long_name[sizeof(long_name) - 1] = '\0';

        NYA_EntityHandle handle = nya_entity_spawn(.name = long_name, .scale = { 1, 1, 1 });

        NYA_SceneEntity record = { 0 };
        nya_scene_entity_from(&record, nya_entity_get(handle));

        nya_check(strlen(record.name) == NYA_SCENE_NAME_MAX - 1, "the name should have been cut to fit, got " FMTu64,
                  (u64)strlen(record.name));

        (void)nya_world_set(origin);
        printf("  PASSED\n");
    }

    // TEST: through a save file, which is the pair a game actually calls
    printf("TEST: nya_scene_save and nya_scene_load\n");
    {
        NYA_EXPECT(nya_scene_save(origin, "scenes/slot0.nya", NYA_SAVE_FLAGS_DATA));

        NYA_World* loaded = nya_world_create();
        defer      nya_world_destroy(loaded);

        NYA_EXPECT(nya_scene_load(loaded, "scenes/slot0.nya", NYA_SAVE_FLAGS_DATA));

        nya_check(same_text(document_text(arena, nya_scene_to_object(arena, origin)),
                            document_text(arena, nya_scene_to_object(arena, loaded))),
                  "a scene through a file should be the same world");

        // A slot that was never written is the ordinary first run, and has to be told apart from a slot that is there and broken.
        NYA_Error missing = nya_scene_load(loaded, "scenes/slot9.nya", NYA_SAVE_FLAGS_DATA);
        nya_check(!missing.ok && missing.kind == NYA_ERROR_NOT_FOUND, "a missing slot should answer NOT_FOUND");

        printf("  PASSED\n");
    }

    // TEST: save data is not meant to be edited, and says so when it has been
    printf("TEST: a tampered save is refused\n");
    {
        NYA_String* path = nya_save_path(arena, "scenes/slot0.nya");
        nya_check(path != nullptr, "the save root should resolve");

        NYA_CString path_cstring = nya_string_to_cstring(arena, path);

        NYA_String* contents = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(path_cstring, contents));

        // One byte in the middle of the body, past the header the format opens with.
        nya_check(contents->length > 64, "a scene file should not be this short");
        contents->items[contents->length / 2] ^= 0x2A;

        NYA_EXPECT(nya_file_write(path_cstring, contents));

        NYA_World* victim = nya_world_create();
        (void)nya_world_set(victim);
        defer nya_world_destroy(victim);

        NYA_EntityHandle survivor = nya_entity_spawn(.name = "survivor", .scale = { 1, 1, 1 });

        NYA_Error loaded = nya_scene_load(victim, "scenes/slot0.nya", NYA_SAVE_FLAGS_DATA);

        nya_check(!loaded.ok, "an altered save should not load");

        // And the world it was going to be loaded into is untouched, which is the whole reason the file is read before anything is despawned.
        nya_check(nya_entity_is_valid(survivor), "a failed load should leave the world alone");

        (void)nya_world_set(origin);
        printf("  PASSED\n");
    }

    printf(nya_check_failures() == 0 ? "PASSED: test_scene\n" : "FAILED: test_scene\n");

    return nya_check_failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
