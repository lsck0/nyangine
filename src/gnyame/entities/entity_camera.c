/**
 * @file entity_camera.c
 *
 * Cameras as entities. The primary one draws the world into the window; others draw into their own texture
 * and are composited as insets. A camera can follow another entity.
 * */
#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The shared half of both create functions: the entity, its transform, and its view. */
NYA_INTERNAL NYA_EntityHandle _gny_entity_camera_spawn(f32x2 position, f32 zoom, u64 flags);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_EntityHandle gny_entity_camera_create(f32x2 position, f32 zoom) {
    // Driven by the keys until something is followed. The flag is what the movement system reads;
    // hand it to a crate instead and the crate is what the keys push.
    NYA_EntityHandle camera = _gny_entity_camera_spawn(position, zoom, GNY_ENTITY_FLAG_PLAYER_CONTROLLED);

    gny_entity_camera_primary_set(camera);

    return camera;
}

NYA_EntityHandle gny_entity_camera_create_view(f32x2 position, f32 zoom, NYA_Rectf viewport) {
    // No GNY_ENTITY_FLAG_PLAYER_CONTROLLED: a secondary view is pointed at something, not steered.
    NYA_EntityHandle camera = _gny_entity_camera_spawn(position, zoom, 0);

    GNY_CameraView* view = gny_entity_camera_view(nya_entity_get(camera));
    if (view != nullptr) view->viewport = viewport;

    return camera;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * UPDATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_entity_camera_on_update(NYA_Entity* entity, f32 delta_time_s) {
    nya_unused(delta_time_s);

    /*
     * The ear, and only for the primary camera.
     */
    if (!gny_entity_flag_check(entity, GNY_ENTITY_FLAG_CAMERA_PRIMARY)) return;

    nya_audio_listener_set((NYA_AudioListener){
        .position           = (f32x2){ entity->position.x, entity->position.y },
        .reference_distance = GNY_CAMERA_EAR_DISTANCE,
        .plane              = NYA_AUDIO_PLANE_SIDE,
    });
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * READING AND STEERING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

GNY_CameraView* gny_entity_camera_view(const NYA_Entity* entity) {
    if (entity == nullptr) return nullptr;

    return entity->user_data;
}

NYA_Camera2DTopDown gny_entity_camera_of(const NYA_Entity* entity) {
    /* The identity camera when there is none, as in the main menu before the game layer creates one. */
    if (entity == nullptr) return (NYA_Camera2DTopDown){ .zoom = 1.0F };

    // between ticks like everything it looks at, or the world would judder against the view.
    return (NYA_Camera2DTopDown){
        .position = nya_entity_render_position(entity).xy,
        .zoom     = entity->scale.x > 0.0F ? entity->scale.x : 1.0F,
        .rotation = 0.0F,
    };
}

NYA_Camera2DTopDown gny_entity_camera_get(void) {
    return gny_entity_camera_of(nya_entity_get(gny_entity_camera_primary()));
}

NYA_EntityHandle gny_entity_camera_primary(void) {
    nya_entity_foreach_flags (GNY_ENTITY_FLAG_CAMERA_PRIMARY, entity) {
        return entity->handle;
    }

    return NYA_ENTITY_HANDLE_NONE;
}

void gny_entity_camera_primary_set(NYA_EntityHandle camera) {
    // Cleared from everything first, so exactly one survives however this is called. Two primary
    // cameras would both draw fullscreen, and the second would simply paint over the first.
    nya_entity_foreach_flags (GNY_ENTITY_FLAG_CAMERA_PRIMARY, entity) {
        nya_entity_flag_disable(entity, GNY_ENTITY_FLAG_CAMERA_PRIMARY);
    }

    NYA_Entity* entity = nya_entity_get(camera);
    if (entity == nullptr) return;

    nya_entity_flag_enable(entity, GNY_ENTITY_FLAG_CAMERA_PRIMARY);

    // The world keeps the primary handle too, purely so gny_world_clear knows what to despawn.
    GNY_World* world = gny_world();
    if (world != nullptr) world->camera = camera;
}

void gny_entity_camera_zoom_by(f32 factor) {
    NYA_Entity* entity = nya_entity_get(gny_entity_camera_primary());
    if (entity == nullptr) return;

    f32 zoom = nya_clamp(entity->scale.x * factor, GNY_CAMERA_ZOOM_MIN, GNY_CAMERA_ZOOM_MAX);

    // Both axes, so nothing that reads scale.y disagrees with what the camera is actually doing.
    entity->scale.x = zoom;
    entity->scale.y = zoom;
}

NYA_EntityHandle gny_entity_camera_target(NYA_EntityHandle camera) {
    GNY_CameraView* view = gny_entity_camera_view(nya_entity_get(camera));
    if (view == nullptr) return NYA_ENTITY_HANDLE_NONE;

    return view->follow;
}

void gny_entity_camera_follow(NYA_EntityHandle camera, NYA_EntityHandle target) {
    GNY_CameraView* view = gny_entity_camera_view(nya_entity_get(camera));
    if (view == nullptr) return;

    /*
     * Both halves written here, and only here.
     */
    NYA_Entity* previous = nya_entity_get(view->follow);

    if (previous != nullptr) {
        // Only clear the marker if no *other* camera is still watching it.
        b8 watched_elsewhere = false;

        nya_entity_foreach_kind (GNY_ENTITY_CAMERA, other) {
            if (other->handle.index == camera.index) continue;

            GNY_CameraView* other_view = gny_entity_camera_view(other);
            if (other_view != nullptr && other_view->follow.index == previous->handle.index) watched_elsewhere = true;
        }

        if (!watched_elsewhere) nya_entity_flag_disable(previous, GNY_ENTITY_FLAG_CAMERA_TARGET);
    }

    view->follow = target;

    NYA_Entity* entity = nya_entity_get(target);
    nya_entity_flag_enable(entity, GNY_ENTITY_FLAG_CAMERA_TARGET);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_EntityHandle _gny_entity_camera_spawn(f32x2 position, f32 zoom, u64 flags) {
    GNY_World* world = gny_world();
    nya_assert(world != nullptr, "a camera cannot be created before the world exists.");

    if (zoom <= 0.0F) zoom = 1.0F;

    // From the world's arena, so it lives exactly as long as the world does. There are a handful of
    // cameras rather than hundreds, so one allocation each is not the trade it would be for crates.
    GNY_CameraView* view = nya_arena_alloc(world->allocator, sizeof(GNY_CameraView));
    *view                = (GNY_CameraView){ .follow = NYA_ENTITY_HANDLE_NONE };

    NYA_EntityHandle camera = nya_entity_spawn(
        .name = "camera",
        .type = GNY_ENTITY_CAMERA,

        // active so it updates, but not visible: it draws nothing and would only cost a render walk.
        .state = NYA_ENTITY_STATE_ACTIVE,

        .position = { position.x, position.y, 0.0F },

        // Zoom lives in the transform's scale, uniformly on x and y so nothing that reads either gets
        // a different answer. z is left at one; there is no third axis to zoom.
        .scale = { zoom, zoom, 1.0F },

        .flags     = flags,
        .user_data = view,
        .on_update = nya_callback(gny_entity_camera_on_update),
    );

    nya_assert(nya_entity_is_valid(camera), "Failed to spawn a camera entity.");

    return camera;
}
