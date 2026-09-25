/**
 * @file examples/pinball3d/main.c
 *
 * A pinball table: the Box3D solver through `physics3d.h`, flat shaded 3D drawing, and a sound on
 * every impact.
 *
 * ```
 * ./build run example pinball3d
 * ```
 *
 * `a` and `d` (or the left and right arrows) work the flippers, `space` launches a new ball,
 * `escape` quits.
 *
 * ## The shape of it
 *
 * Physics is a property of an entity: spawn one, attach a body, and the solver writes the entity's
 * transform every tick. Nothing here steps the world or reads Box3D — `nya_system_physics3d_update`
 * runs at the top of every tick, before any layer's `on_update`, and a body's entity is where the
 * result shows up.
 *
 * The table is tilted by rotating the whole playfield rather than by tilting gravity, so the walls,
 * the flippers and the slope agree with each other by construction. Gravity stays pointing down.
 * */
#include "genyarated/assets.h"
#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/* CONSTANTS */

#define WINDOW_TITLE  "nyangine — pinball"
#define WINDOW_WIDTH  720
#define WINDOW_HEIGHT 900

/** The layer's id, compared by content so it survives a code reload. */
#define LAYER_ID "pinball"

/** The playfield, in world units (one unit is one metre; see NYA_PHYSICS3D_UNITS_PER_METER). */
#define TABLE_HALF_WIDTH  1.2F
#define TABLE_HALF_LENGTH 2.0F

/** How far the far end is lifted. About six degrees, which is a real table's slope. */
#define TABLE_TILT_RADIANS 0.105F

#define WALL_HEIGHT    0.30F
#define WALL_THICKNESS 0.10F

#define BALL_RADIUS 0.075F

/** Steel. Box3D takes kilograms per cubic metre, so this is what makes the ball feel heavy. */
#define BALL_DENSITY 7800.0F

/** How much of its closing speed the ball keeps off a wall, a bumper, and the playfield. */
#define WALL_RESTITUTION   0.45F
#define BUMPER_RESTITUTION 1.15F
#define TABLE_RESTITUTION  0.20F

/** The push a bumper gives, in newton-seconds. Tuned so one bumper alone cannot hold the ball. */
#define BUMPER_IMPULSE 0.9F

#define BUMPER_RADIUS 0.18F
#define BUMPER_COUNT  3

/** The flippers, as boxes pivoting about their inner end. */
#define FLIPPER_LENGTH    0.55F
#define FLIPPER_THICKNESS 0.09F
#define FLIPPER_HEIGHT    0.12F

/** Rest and raised angles, about the table's up axis. */
#define FLIPPER_REST_RADIANS   (-0.45F)
#define FLIPPER_RAISED_RADIANS 0.55F

/** Radians per second a flipper swings at. Fast enough to throw the ball, slow enough to see. */
#define FLIPPER_SPEED 14.0F

/** Where the flippers pivot, in flat table coordinates. Positive z is toward the drain. */
#define FLIPPER_PIVOT_X 0.42F
#define FLIPPER_PIVOT_Z 1.45F

/** A flipper's centre height, so it sits on the playfield rather than through it. */
#define FLIPPER_PIVOT_Y (FLIPPER_HEIGHT * 0.5F)

/** Past this the ball has drained and a new one is served. */
#define DRAIN_Z (TABLE_HALF_LENGTH + 0.6F)

/**
 * Where a new ball appears, in table space.
 *
 * Left of the centre line and well up the playfield, rather than the right hand launch lane a real
 * table serves from: the ball meets the bumpers on its way down instead of being fired past them,
 * which is what there is to look at in a demo.
 *
 * `x` runs across the table and `z` down it, so a smaller `z` is further up, away from the flippers.
 * */
#define SERVE_X (-TABLE_HALF_WIDTH + 0.50F)
#define SERVE_Z (TABLE_HALF_LENGTH - 1.60F)

/** The launch, in newton-seconds up the table. */
#define SERVE_IMPULSE 2.6F

/** Closing speed a contact needs before it is reported as a hit, in world units per second. */
#define HIT_THRESHOLD 0.8F

/** Impact speed that plays a sound at full gain. Above it the gain is clamped. */
#define HIT_LOUD_SPEED 4.0F

/** Where the camera sits and looks, in world space. Behind and above the near end. */
#define CAMERA_POSITION ((f32x3){ 0.0F, 2.6F, 4.0F })
#define CAMERA_TARGET   ((f32x3){ 0.0F, 0.2F, -0.2F })

#define HUD_POINT_SIZE 18.0F

/** The room around the table. Blue rather than black, so the dark green playfield's edge still reads against it. */
#define ROOM_COLOR ((NYA_Color){ 0.07F, 0.08F, 0.13F, 1.0F })

/* TYPES */

typedef enum {
    PINBALL_ENTITY_NONE = 0,
    PINBALL_ENTITY_TABLE,
    PINBALL_ENTITY_WALL,
    PINBALL_ENTITY_BUMPER,
    PINBALL_ENTITY_FLIPPER,
    PINBALL_ENTITY_BALL,

    PINBALL_ENTITY_KIND_COUNT,
} PinballEntityKind;

/**
 * This example's own actions, continuing the engine's numbering from NYA_INPUT_ACTION_USER.
 *
 * Unnamed on purpose: these are NYA_InputAction values, and a named enum of its own would make every
 * nya_input_action_* call a conversion between two enumeration types, which the build rejects.
 * */
enum {
    PINBALL_ACTION_LEFT_FLIPPER = NYA_INPUT_ACTION_USER,
    PINBALL_ACTION_RIGHT_FLIPPER,
    PINBALL_ACTION_SERVE,
};

/** One flipper: where it pivots, which way it swings, and where it is right now. */
typedef struct {
    NYA_EntityHandle entity;

    /** In table space, before the tilt. */
    f32x3 pivot;

    /** +1 for the left flipper, which swings clockwise seen from above; -1 for the right. */
    f32 swing;

    /** Radians from rest, driven toward the target by FLIPPER_SPEED. */
    f32 angle;

    /**
     * Set once the flipper has stopped and been put exactly where `angle` says.
     *
     * The solver moves this body by integrating the velocity it is given, which lands close to the
     * pose but not on it. The error is nothing within one swing and would collect over a game, so
     * the first still tick corrects it. Only the first: a correction is a teleport, and a teleport
     * every tick would keep the flipper and whatever rests on it from ever sleeping.
     * */
    b8 settled;
} Flipper;

typedef struct {
    NYA_WindowHandle window;

    NYA_EntityHandle ball;
    Flipper          flippers[2];

    u32 score;
    u32 balls_drained;

    /** Whether the hit sound loaded. A missing sound must not stop the table working. */
    b8 sound_ready;
} Pinball;

NYA_INTERNAL Pinball* pinball(void) {
    return nya_world_user_data();
}

/* TABLE SPACE */

/* The playfield lies in the xz plane with -z up the table, and the whole thing is then tilted about x so the far end is higher. Everything bolted to the table is placed in that flat space and rotated through the two functions below, which is why nothing here has to think about the slope. */

/** The tilt, as a rotation about the x axis. */
NYA_INTERNAL NYA_Quaternion table_rotation(void) {
    return nya_quaternion_from_axis_angle((f32x3){ 1.0F, 0.0F, 0.0F }, TABLE_TILT_RADIANS);
}

/** A point on the flat playfield, in world space. */
NYA_INTERNAL f32x3 table_point(f32x3 flat) {
    return nya_quaternion_rotate(table_rotation(), flat);
}

/* BUILDING THE TABLE */

/** A static box bolted to the playfield. Walls and the playfield itself. */
NYA_INTERNAL NYA_EntityHandle solid_create(NYA_ConstCString name, u32 kind, f32x3 flat_center, f32x3 size, f32 restitution) {
    NYA_EntityHandle handle = nya_entity_spawn(
        .name     = name,
        .type     = kind,
        .position = table_point(flat_center),
        .rotation = table_rotation(),
    );

    b8 attached = nya_physics3d_body_attach(handle,
        .type        = NYA_PHYSICS_BODY_STATIC,
        .shape       = NYA_PHYSICS3D_SHAPE_BOX,
        .size        = size,
        .friction    = 0.15F,
        .restitution = restitution,
    );

    nya_assert(attached, "a static body could not be attached to '%s'", name);

    return handle;
}

/** A bumper: a static sphere that reports its impacts and is answered with an impulse. */
NYA_INTERNAL NYA_EntityHandle bumper_create(f32x3 flat_center) {
    NYA_EntityHandle handle = nya_entity_spawn(
        .name     = "bumper",
        .type     = PINBALL_ENTITY_BUMPER,
        .position = table_point(flat_center),
    );

    b8 attached = nya_physics3d_body_attach(handle,
        .type        = NYA_PHYSICS_BODY_STATIC,
        .shape       = NYA_PHYSICS3D_SHAPE_SPHERE,
        .radius      = BUMPER_RADIUS,
        .restitution = BUMPER_RESTITUTION,
    );

    nya_assert(attached, "a bumper body could not be attached");

    return handle;
}

/** Where a flipper's centre sits for a given angle, and which way it points. */
NYA_INTERNAL void flipper_pose(const Flipper* flipper, OUT f32x3* out_position, OUT NYA_Quaternion* out_rotation) {
    nya_assert(flipper != nullptr);
    nya_assert(out_position != nullptr);
    nya_assert(out_rotation != nullptr);

    // About the table's own up axis, before the tilt: the flipper sweeps across the playfield, not through it.
    NYA_Quaternion swing = nya_quaternion_from_axis_angle((f32x3){ 0.0F, 1.0F, 0.0F }, flipper->angle * flipper->swing);

    // The body's origin is its centre, so it sits half a length out along the arm from the pivot.
    f32x3 arm = nya_quaternion_rotate(swing, (f32x3){ -flipper->swing * FLIPPER_LENGTH * 0.5F, 0.0F, 0.0F });

    f32x3 flat = { flipper->pivot.x + arm.x, flipper->pivot.y + arm.y, flipper->pivot.z + arm.z };

    *out_position = table_point(flat);
    *out_rotation = nya_quaternion_multiply(table_rotation(), swing);
}

NYA_INTERNAL void flipper_create(Flipper* flipper, f32 pivot_x, f32 swing) {
    nya_assert(flipper != nullptr);

    *flipper = (Flipper){
        .pivot = { pivot_x, FLIPPER_PIVOT_Y, FLIPPER_PIVOT_Z },
        .swing = swing,
        .angle = FLIPPER_REST_RADIANS,
    };

    f32x3          position = { 0 };
    NYA_Quaternion rotation = { 0 };
    flipper_pose(flipper, &position, &rotation);

    flipper->entity = nya_entity_spawn(
        .name     = "flipper",
        .type     = PINBALL_ENTITY_FLIPPER,
        .position = position,
        .rotation = rotation,
    );

    // Kinematic: it moves where it is told and pushes the ball without the ball pushing back, which is what a solenoid driven flipper is.
    b8 attached = nya_physics3d_body_attach(flipper->entity,
        .type        = NYA_PHYSICS_BODY_KINEMATIC,
        .shape       = NYA_PHYSICS3D_SHAPE_BOX,
        .size        = { FLIPPER_LENGTH, FLIPPER_HEIGHT, FLIPPER_THICKNESS },
        .restitution = 0.35F,
    );

    nya_assert(attached, "a flipper body could not be attached");
}

/** Puts the ball back in the launch lane and fires it up the table. */
NYA_INTERNAL void ball_serve(void) {
    NYA_Entity* ball = nya_entity_get(pinball()->ball);
    if (ball == nullptr) return;

    f32x3 at = table_point((f32x3){ SERVE_X, BALL_RADIUS + 0.02F, SERVE_Z });

    // Teleport, not a write to position: the solver owns a body's transform, and moving the entity behind its back leaves the two disagreeing until the next contact.
    nya_physics3d_teleport(ball, at, (NYA_Quaternion){ 0.0F, 0.0F, 0.0F, 1.0F });
    nya_physics3d_velocity_set(ball, (f32x3){ 0.0F, 0.0F, 0.0F });
    nya_physics3d_angular_velocity_set(ball, (f32x3){ 0.0F, 0.0F, 0.0F });

    nya_physics3d_apply_impulse(ball, table_point((f32x3){ 0.0F, 0.0F, -SERVE_IMPULSE }));
}

/* THE LAYER */

void pinball_layer_on_create(NYA_Window* window) {
    nya_assert(window != nullptr);

    // nothing is drawn behind the table, so what the window clears to is the room it stands in.
    nya_render_clear_color_set(window, ROOM_COLOR);

    nya_input_action_bind(PINBALL_ACTION_LEFT_FLIPPER, NYA_KEY_A);
    nya_input_action_bind(PINBALL_ACTION_LEFT_FLIPPER, NYA_KEY_LEFT);
    nya_input_action_bind(PINBALL_ACTION_RIGHT_FLIPPER, NYA_KEY_D);
    nya_input_action_bind(PINBALL_ACTION_RIGHT_FLIPPER, NYA_KEY_RIGHT);
    nya_input_action_bind(PINBALL_ACTION_SERVE, NYA_KEY_SPACE);

    // Queued, not loaded: the asset system resolves it at the end of the frame. Predecoded because it is short and played often, and decoding at the moment of an impact is when a hitch shows.
    NYA_Error sound = nya_asset_load((NYA_AssetLoadParameters){
        .type     = NYA_ASSET_TYPE_SOUND,
        .handle   = NYA_ASSET_SOUNDS_HIT_WAV,
        .as_sound = { .predecode = true },
    });

    // Not fatal. A machine with no audio device still plays the table.
    pinball()->sound_ready = sound.ok;
    if (!sound.ok) nya_log_warn("%s", (NYA_ConstCString)sound.message);

    // Impacts below this are the ball rolling, which would play a sound every tick.
    nya_physics3d_hit_threshold_set(HIT_THRESHOLD);

    // the playfield
    (void)solid_create("playfield", PINBALL_ENTITY_TABLE, (f32x3){ 0.0F, -0.05F, 0.0F },
                       (f32x3){ TABLE_HALF_WIDTH * 2.0F, 0.1F, TABLE_HALF_LENGTH * 2.0F }, TABLE_RESTITUTION);

    f32 wall_center_y = WALL_HEIGHT * 0.5F;

    (void)solid_create("wall_left", PINBALL_ENTITY_WALL, (f32x3){ -TABLE_HALF_WIDTH, wall_center_y, 0.0F },
                       (f32x3){ WALL_THICKNESS, WALL_HEIGHT, TABLE_HALF_LENGTH * 2.0F }, WALL_RESTITUTION);

    (void)solid_create("wall_right", PINBALL_ENTITY_WALL, (f32x3){ TABLE_HALF_WIDTH, wall_center_y, 0.0F },
                       (f32x3){ WALL_THICKNESS, WALL_HEIGHT, TABLE_HALF_LENGTH * 2.0F }, WALL_RESTITUTION);

    (void)solid_create("wall_top", PINBALL_ENTITY_WALL, (f32x3){ 0.0F, wall_center_y, -TABLE_HALF_LENGTH },
                       (f32x3){ TABLE_HALF_WIDTH * 2.0F, WALL_HEIGHT, WALL_THICKNESS }, WALL_RESTITUTION);

    // the bumpers
    f32x3 bumper_positions[BUMPER_COUNT] = {
        { -0.45F, BUMPER_RADIUS, -0.85F },
        { 0.45F, BUMPER_RADIUS, -0.85F },
        { 0.0F, BUMPER_RADIUS, -1.35F },
    };

    for (u32 i = 0; i < BUMPER_COUNT; i++) (void)bumper_create(bumper_positions[i]);

    // the flippers
    flipper_create(&pinball()->flippers[0], -FLIPPER_PIVOT_X, 1.0F);
    flipper_create(&pinball()->flippers[1], FLIPPER_PIVOT_X, -1.0F);

    // the ball
    pinball()->ball = nya_entity_spawn(
        .name     = "ball",
        .type     = PINBALL_ENTITY_BALL,
        .position = table_point((f32x3){ SERVE_X, BALL_RADIUS, SERVE_Z }),
    );

    b8 attached = nya_physics3d_body_attach(pinball()->ball,
        .type        = NYA_PHYSICS_BODY_DYNAMIC,
        .shape       = NYA_PHYSICS3D_SHAPE_SPHERE,
        .radius      = BALL_RADIUS,
        .density     = BALL_DENSITY,
        .restitution = 0.25F,
        // Small, fast and steel: without continuous collision it tunnels through the walls.
        .is_bullet   = true,
    );

    nya_assert(attached, "the ball's body could not be attached");

    ball_serve();
}

void pinball_layer_on_destroy(NYA_Window* window) {
    nya_unused(window);

    // The world owns the entities and their bodies, and tears both down with itself. The pair exists so callers already pair it the day this has something of its own to release.
}

void pinball_layer_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);
    nya_assert(event != nullptr);

    if (event->type != NYA_EVENT_KEY_DOWN) return;

    if (event->as_key_event.key == NYA_KEY_ESCAPE) {
        nya_app_get()->should_quit = true;
        event->was_handled         = true;
    }
}

void pinball_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window);

    Pinball* state = pinball();

    // the flippers
    b8 held[2] = {
        nya_input_action_pressed(PINBALL_ACTION_LEFT_FLIPPER),
        nya_input_action_pressed(PINBALL_ACTION_RIGHT_FLIPPER),
    };

    for (u32 i = 0; i < 2; i++) {
        Flipper* flipper = &state->flippers[i];

        NYA_Entity* entity = nya_entity_get(flipper->entity);
        if (entity == nullptr) continue;

        const f32 target = held[i] ? FLIPPER_RAISED_RADIANS : FLIPPER_REST_RADIANS;
        const f32 step   = FLIPPER_SPEED * delta_time_s;

        // Toward the target at a fixed rate rather than snapping: the speed of the sweep is what throws the ball, and a snap would move through it without ever touching it.
        const f32 was  = flipper->angle;
        f32       next = was;
        if (next < target) next = nya_min(next + step, target);
        if (next > target) next = nya_max(next - step, target);

        f32x3          from_position = { 0 };
        NYA_Quaternion from_rotation = { 0 };
        flipper_pose(flipper, &from_position, &from_rotation);

        flipper->angle = next;

        f32x3          to_position = { 0 };
        NYA_Quaternion to_rotation = { 0 };
        flipper_pose(flipper, &to_position, &to_rotation);

        if (next == was) {
            nya_physics3d_velocity_set(entity, f32x3_zero);
            nya_physics3d_angular_velocity_set(entity, f32x3_zero);

            if (!flipper->settled) {
                nya_physics3d_teleport(entity, to_position, to_rotation);
                flipper->settled = true;
            }

            continue;
        }

        flipper->settled = false;

        /* Driven by its velocity rather than by nya_physics3d_teleport, which sets the transform without a sweep and so leaves the solver reading a body that never moved: the ball would be pushed out from inside the flipper rather than thrown by it. Given a velocity instead, the solver steps the flipper itself and the contact carries that velocity into the ball, which is the whole of what a flipper does. Layers tick after the solver and with the same fixed delta, so a velocity set here is exactly one step of integration away from the pose it was measured against. */
        nya_physics3d_velocity_set(entity, (to_position - from_position) / delta_time_s);

        // The swing is about the table's own up axis, which the tilt has carried off vertical.
        const f32x3 axis = nya_quaternion_rotate(table_rotation(), (f32x3){ 0.0F, 1.0F, 0.0F });
        nya_physics3d_angular_velocity_set(entity, axis * ((next - was) * flipper->swing / delta_time_s));
    }

    // the impacts from the step at the top of this tick
    u32                   hit_count = 0;
    const NYA_PhysicsHit* hits      = nya_physics3d_hits(&hit_count);

    for (u32 i = 0; i < hit_count; i++) {
        const NYA_PhysicsHit* hit = &hits[i];
        if (hit->dimension != NYA_PHYSICS_3D || hit->kind != NYA_PHYSICS_HIT_IMPACT) continue;

        NYA_Entity* a = nya_entity_get(hit->a);
        NYA_Entity* b = nya_entity_get(hit->b);
        if (a == nullptr || b == nullptr) continue;

        // The two sides of an impact are not ordered, so the ball may be either of them.
        NYA_Entity* ball  = a->type == PINBALL_ENTITY_BALL ? a : b;
        NYA_Entity* other = ball == a ? b : a;
        if (ball->type != PINBALL_ENTITY_BALL) continue;

        // Louder the harder it lands, which is most of what makes a physical hit read as physical.
        if (state->sound_ready) {
            f32 gain = nya_clamp(hit->approach_speed / HIT_LOUD_SPEED, 0.15F, 1.0F);
            (void)nya_audio_play_sound_varied(NYA_ASSET_SOUNDS_HIT_WAV, gain);
        }

        // A bumper answers a contact with a push along the normal, which is what makes it a bumper rather than a bollard. Restitution alone cannot add energy.
        if (other->type == PINBALL_ENTITY_BUMPER) {
            f32x3 away = nya_vector_normalize((f32x3){
                ball->position.x - other->position.x,
                ball->position.y - other->position.y,
                ball->position.z - other->position.z,
            });

            nya_physics3d_apply_impulse(ball, (f32x3){ away.x * BUMPER_IMPULSE, away.y * BUMPER_IMPULSE, away.z * BUMPER_IMPULSE });

            state->score += 100;
        }
    }

    // the drain
    NYA_Entity* ball = nya_entity_get(state->ball);
    if (ball == nullptr) return;

    if (nya_input_action_just_pressed(PINBALL_ACTION_SERVE)) {
        ball_serve();
        return;
    }

    // Measured in world space, so a ball that has fallen off the side counts too.
    if (ball->position.z > DRAIN_Z || ball->position.y < -2.0F) {
        state->balls_drained++;
        ball_serve();
    }
}

/* DRAWING */

/** The flat colour each kind draws in. Indexed by PinballEntityKind. */
NYA_INTERNAL NYA_Color kind_color(u32 kind) {
    switch (kind) {
        case PINBALL_ENTITY_TABLE: return (NYA_Color){ 0.09F, 0.20F, 0.16F, 1.0F };
        case PINBALL_ENTITY_WALL: return (NYA_Color){ 0.32F, 0.33F, 0.38F, 1.0F };
        case PINBALL_ENTITY_BUMPER: return NYA_COLOR_CORAL;
        case PINBALL_ENTITY_FLIPPER: return NYA_COLOR_GOLD;
        case PINBALL_ENTITY_BALL: return NYA_COLOR_LIGHT_GRAY;

        // Every kind is listed above, so reaching this means a kind was added and not drawn.
        default: nya_log_panic("no colour for entity kind %u", kind);
    }
}

void pinball_layer_on_render(NYA_Window* window) {
    nya_assert(window != nullptr);

    nya_render3d_begin(window, (NYA_Camera3DPerspective){ .position = CAMERA_POSITION, .target = CAMERA_TARGET });

    nya_render3d_light_set(window, (NYA_Render3DLight){
        .direction = nya_vector_normalize((f32x3){ -0.4F, -1.0F, -0.3F }),
        .color     = NYA_COLOR_WHITE,
        .ambient   = 0.35F,
        .intensity = 1.0F,
    });

    // nya_entity_render_position and _rotation, not the entity's own: a frame can land between two ticks, and these are where the body is at this frame's time.
    nya_entity_foreach (entity) {
        if (entity->type == PINBALL_ENTITY_NONE) continue;

        f32x3          at       = nya_entity_render_position(entity);
        NYA_Quaternion rotation = nya_entity_render_rotation(entity);
        NYA_Color      color    = kind_color(entity->type);

        if (entity->type == PINBALL_ENTITY_BALL) {
            nya_render3d_sphere(window, at, BALL_RADIUS, color);
            continue;
        }

        if (entity->type == PINBALL_ENTITY_BUMPER) {
            nya_render3d_sphere(window, at, BUMPER_RADIUS, color);
            continue;
        }

        // Every other body is a box, and the body remembers its own full extents.
        nya_render3d_cube(window, at, entity->physics3d.size, rotation, color);
    }

    nya_render3d_end(window);

    nya_render2d_textf_with_font(window, NYA_ASSET_FONTS_ALDRICH_TTF, HUD_POINT_SIZE, 16.0F, 16.0F, NYA_COLOR_WHITE, "score %u", pinball()->score);

    nya_render2d_textf_with_font(window, NYA_ASSET_FONTS_ALDRICH_TTF, HUD_POINT_SIZE, 16.0F, 16.0F + (HUD_POINT_SIZE * 1.4F), NYA_COLOR_GRAY,
                                 "drained %u · a/d flippers · space serves", pinball()->balls_drained);
}

/* MAIN */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    NYA_EXPECT(nya_app_init(.app_id = "pinball3d"), "while starting the engine");

    // The game's root pointer lives on the world, so it shares the world's arena and lifetime.
    Pinball* state = nya_arena_alloc(nya_world()->allocator, sizeof(Pinball));
    *state         = (Pinball){ .window = NYA_WINDOW_HANDLE_NONE, .ball = NYA_ENTITY_HANDLE_NONE };
    nya_world_user_data_set(state);

    state->window = nya_window_create(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_RESIZABLE);
    nya_assert(nya_window_is_valid(state->window), "the window could not be created");

    // Pushing the layer runs its on_create, which is what builds the table.
    nya_layer_push(state->window, nya_layer_of(pinball_layer, LAYER_ID));

    nya_app_run();

    nya_app_deinit();

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
