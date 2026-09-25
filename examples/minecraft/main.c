/**
 * @file examples/minecraft/main.c
 *
 * A tiny voxel sandbox in the flat, stylized look the other 3D demos share. A finite chunk of blocks — a
 * heightmapped 16x16 field, three block kinds told apart by colour — is drawn as cubes through the plain
 * render3d pipeline (nya_render3d_cube), so there is no mesh to register and no shader to add: each visible
 * block is one cube draw, and a block walled in on all six sides is skipped.
 *
 * A free-fly camera looks with the mouse (captured relative) and moves with WASD, with space/shift for up and
 * down. A voxel DDA (Amanatides & Woo) marches the crosshair ray through the grid to the first solid block;
 * that block is outlined, left-click breaks it, and right-click sets a block against the face the ray entered.
 * Keys 1/2/3 pick which kind right-click places. The whole world is one fixed array, so nothing allocates as
 * you dig or build.
 *
 * ```
 * ./build run example minecraft
 * NYA_MINECRAFT_FRAMES=6 ./build run example minecraft   # draw six frames and quit, for a headless run
 * ```
 *
 * It only calls the public renderer, input and window API and registers no engine systems.
 * */
#include "genyarated/assets.h"
#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/* CONSTANTS */

#define WINDOW_TITLE  "nyangine — minecraft"
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/** The layer's id, compared by content so it survives a code reload. */
#define LAYER_ID "minecraft"

/** The chunk's extent in blocks: a 16x16 footprint standing up to WORLD_Y tall. One fixed array, never grown. */
#define WORLD_X 16
#define WORLD_Y 10
#define WORLD_Z 16

/** How far, in blocks, the crosshair ray reaches before it gives up finding a block to target. */
#define REACH_BLOCKS 7.0F

/** How fast the free-fly camera moves, in blocks per second, and how much a pixel of mouse turns it. */
#define MOVE_SPEED   9.0F
#define LOOK_SENS    0.0026F

#define HUD_FONT      NYA_ASSET_FONTS_ALDRICH_TTF
#define HUD_FONT_SIZE 16.0F

/** The block kinds. AIR is the empty cell; the rest index BLOCK_COLORS below. */
enum BlockKind { BLOCK_AIR = 0, BLOCK_GRASS = 1, BLOCK_DIRT = 2, BLOCK_STONE = 3, BLOCK_KIND_COUNT };

/** One flat colour per solid kind (index 0 unused, so BLOCK_GRASS indexes directly). */
NYA_INTERNAL const NYA_Color BLOCK_COLORS[BLOCK_KIND_COUNT] = {
    [BLOCK_AIR]   = { 0.0F, 0.0F, 0.0F, 0.0F },
    [BLOCK_GRASS] = { 0.36F, 0.62F, 0.28F, 1.0F },
    [BLOCK_DIRT]  = { 0.46F, 0.32F, 0.19F, 1.0F },
    [BLOCK_STONE] = { 0.49F, 0.49F, 0.53F, 1.0F },
};

NYA_INTERNAL NYA_ConstCString block_name(u8 kind) {
    switch (kind) {
        case BLOCK_GRASS: return "grass";
        case BLOCK_DIRT:  return "dirt";
        case BLOCK_STONE: return "stone";
        default:          return "air";
    }
}

/* STATE */

typedef struct Minecraft Minecraft;

struct Minecraft {
    NYA_WindowHandle window;

    /** The whole chunk, one byte per cell (a BlockKind). Fixed size, so digging and building never allocate. */
    u8 blocks[WORLD_X][WORLD_Y][WORLD_Z];

    /** The free-fly camera: where the eye is and the yaw/pitch it looks along. */
    f32x3 eye;
    f32   yaw;
    f32   pitch;

    /** Which kind the next right-click places. */
    u8 place_kind;

    /** The block the crosshair is on this frame, and the empty cell against the face the ray entered (for placing). */
    b8  has_target;
    s32 target[3];
    s32 place[3];

    /** When non-zero (from NYA_MINECRAFT_FRAMES), the demo quits after this many frames, for a headless run. */
    u32 max_frames;
    u32 frame_count;

    f32 last_fps;
};

NYA_INTERNAL Minecraft* minecraft(void) {
    return nya_world_user_data();
}

/* WORLD */

/** Whether a cell is inside the chunk. */
NYA_INTERNAL b8 in_bounds(s32 x, s32 y, s32 z) {
    return x >= 0 && x < WORLD_X && y >= 0 && y < WORLD_Y && z >= 0 && z < WORLD_Z;
}

/** The kind at a cell, or AIR for anything outside the chunk (so border blocks always count as exposed). */
NYA_INTERNAL u8 block_at(const Minecraft* state, s32 x, s32 y, s32 z) {
    return in_bounds(x, y, z) ? state->blocks[x][y][z] : (u8)BLOCK_AIR;
}

/** A block with all six neighbours solid is never drawn: it cannot be seen and would only cost a cube. */
NYA_INTERNAL b8 block_exposed(const Minecraft* state, s32 x, s32 y, s32 z) {
    return block_at(state, x - 1, y, z) == BLOCK_AIR || block_at(state, x + 1, y, z) == BLOCK_AIR
        || block_at(state, x, y - 1, z) == BLOCK_AIR || block_at(state, x, y + 1, z) == BLOCK_AIR
        || block_at(state, x, y, z - 1) == BLOCK_AIR || block_at(state, x, y, z + 1) == BLOCK_AIR;
}

/**
 * Fills the chunk from a smooth heightmap: a couple of sine ridges set each column's surface, grass on top, a
 * band of dirt under it, stone below. Pure, so the world is the same on every run.
 * */
NYA_INTERNAL void world_generate(Minecraft* state) {
    for (s32 x = 0; x < WORLD_X; x++) {
        for (s32 z = 0; z < WORLD_Z; z++) {
            f32 ridge  = 4.0F + (1.8F * sinf((f32)x * 0.55F)) + (1.4F * cosf((f32)z * 0.7F));
            s32 height = (s32)nya_clamp(ridge, 1.0F, (f32)(WORLD_Y - 1));

            for (s32 y = 0; y < WORLD_Y; y++) {
                u8 kind = BLOCK_AIR;
                if (y == height - 1) kind = BLOCK_GRASS;
                else if (y >= height - 3 && y < height) kind = BLOCK_DIRT;
                else if (y < height) kind = BLOCK_STONE;

                state->blocks[x][y][z] = kind;
            }
        }
    }
}

/* CAMERA + RAYCAST */

/** The camera's forward from its yaw and pitch. */
NYA_INTERNAL f32x3 camera_forward(const Minecraft* state) {
    return (f32x3){
        cosf(state->pitch) * cosf(state->yaw),
        sinf(state->pitch),
        cosf(state->pitch) * sinf(state->yaw),
    };
}

/**
 * Marches the crosshair ray through the grid a voxel at a time (Amanatides & Woo) until it enters a solid
 * block or runs past REACH_BLOCKS. On a hit it records the block and, in `place`, the empty cell just before
 * it along the axis the ray crossed — the face the ray came through, where a right-click builds.
 * */
NYA_INTERNAL void raycast(Minecraft* state) {
    state->has_target = false;

    f32x3 origin = state->eye;
    f32x3 dir    = camera_forward(state);

    s32 cell[3]  = { (s32)floorf(origin.x), (s32)floorf(origin.y), (s32)floorf(origin.z) };
    f32 d[3]     = { dir.x, dir.y, dir.z };
    f32 o[3]     = { origin.x, origin.y, origin.z };

    s32 step[3];
    f32 t_max[3];
    f32 t_delta[3];

    for (u32 a = 0; a < 3; a++) {
        step[a]    = d[a] > 0.0F ? 1 : -1;
        t_delta[a] = d[a] != 0.0F ? fabsf(1.0F / d[a]) : INFINITY;

        f32 boundary = (f32)cell[a] + (d[a] > 0.0F ? 1.0F : 0.0F);
        t_max[a]     = d[a] != 0.0F ? (boundary - o[a]) / d[a] : INFINITY;
    }

    s32 prev[3] = { cell[0], cell[1], cell[2] };
    f32 travel  = 0.0F;

    while (travel <= REACH_BLOCKS) {
        if (block_at(state, cell[0], cell[1], cell[2]) != BLOCK_AIR) {
            state->has_target = true;
            state->target[0]  = cell[0];
            state->target[1]  = cell[1];
            state->target[2]  = cell[2];
            state->place[0]   = prev[0];
            state->place[1]   = prev[1];
            state->place[2]   = prev[2];
            return;
        }

        // step along whichever axis reaches its next cell boundary first.
        u32 axis = (t_max[0] < t_max[1]) ? (t_max[0] < t_max[2] ? 0 : 2) : (t_max[1] < t_max[2] ? 1 : 2);

        prev[0] = cell[0];
        prev[1] = cell[1];
        prev[2] = cell[2];

        cell[axis] += step[axis];
        travel      = t_max[axis];
        t_max[axis] += t_delta[axis];
    }
}

/* LAYER: CREATE / DESTROY / EVENT */

void minecraft_layer_on_create(NYA_Window* window) {
    nya_assert(window != nullptr);

    Minecraft* state = minecraft();

    // a soft sky-blue behind everything, so nothing renders as a black frame before the sky pass paints over it.
    nya_render_clear_color_set(window, (NYA_Color){ 0.52F, 0.68F, 0.86F, 1.0F });

    world_generate(state);

    // the mouse is captured for a first-person look; escape (in on_event) quits, which releases it on teardown.
    nya_window_set_relative_mouse(state->window, true);
    nya_cursor_visible_set(false);

    // the pillars cast onto the chunk so the block faces read before any shading deepens their seams.
    nya_render3d_shadow_options_set(window, (NYA_Render3DShadowOptions){
                                                .cascades = NYA_RENDER3D_SHADOW_CASCADES,
                                                .map_size = 2048,
                                                .color    = { 0.12F, 0.16F, 0.22F, 0.5F },
                                            });
}

void minecraft_layer_on_destroy(NYA_Window* window) {
    nya_unused(window);

    Minecraft* state = minecraft();

    // hand the mouse back, so a re-enter (a hot reload) or the next app starts with a free, visible cursor.
    nya_window_set_relative_mouse(state->window, false);
    nya_cursor_visible_set(true);

    nya_log_info("minecraft: shutting down after %u frames, last %.1f fps.", state->frame_count, (f64)state->last_fps);
}

void minecraft_layer_on_event(NYA_Window* window, NYA_Event* event) {
    nya_unused(window);
    nya_assert(event != nullptr);

    Minecraft* state = minecraft();

    if (event->type == NYA_EVENT_KEY_DOWN && event->as_key_event.key == NYA_KEY_ESCAPE) {
        nya_app_get()->should_quit = true;
        event->was_handled         = true;
        return;
    }

    // mouse look: turn by the relative motion the captured pointer reports, pitch clamped just shy of vertical.
    if (event->type == NYA_EVENT_MOUSE_MOVED) {
        NYA_MouseMovedEvent* mouse = &event->as_mouse_moved_event;

        state->yaw   += mouse->delta_x * LOOK_SENS;
        state->pitch -= mouse->delta_y * LOOK_SENS;
        state->pitch  = nya_clamp(state->pitch, -1.5F, 1.5F);

        event->was_handled = true;
    }
}

/* LAYER: UPDATE */

void minecraft_layer_on_update(NYA_Window* window, f32 delta_time_s) {
    nya_unused(window);

    Minecraft* state = minecraft();

    // WASD in the camera's ground frame, space/shift straight up and down.
    f32x3 forward = camera_forward(state);
    f32x3 flat    = nya_vector_normalize((f32x3){ forward.x, 0.0F, forward.z });
    f32x3 right   = nya_vector_normalize(nya_vector_cross(flat, (f32x3){ 0.0F, 1.0F, 0.0F }));
    f32   speed   = MOVE_SPEED * delta_time_s;

    if (nya_input_key_pressed(NYA_KEY_W)) state->eye = state->eye + (flat * speed);
    if (nya_input_key_pressed(NYA_KEY_S)) state->eye = state->eye - (flat * speed);
    if (nya_input_key_pressed(NYA_KEY_D)) state->eye = state->eye + (right * speed);
    if (nya_input_key_pressed(NYA_KEY_A)) state->eye = state->eye - (right * speed);
    if (nya_input_key_pressed(NYA_KEY_SPACE)) state->eye.y += speed;
    if (nya_input_key_pressed(NYA_KEY_LSHIFT)) state->eye.y -= speed;

    // pick which kind the next placement uses.
    if (nya_input_key_just_pressed(NYA_KEY_1)) state->place_kind = BLOCK_GRASS;
    if (nya_input_key_just_pressed(NYA_KEY_2)) state->place_kind = BLOCK_DIRT;
    if (nya_input_key_just_pressed(NYA_KEY_3)) state->place_kind = BLOCK_STONE;

    // the crosshair ray, resolved once a frame; on_render outlines whatever it found.
    raycast(state);

    // left-click breaks the targeted block; right-click fills the empty cell against the face it entered.
    if (state->has_target && nya_input_mouse_button_just_pressed(NYA_MOUSE_BUTTON_LEFT)) {
        state->blocks[state->target[0]][state->target[1]][state->target[2]] = BLOCK_AIR;
    }

    if (state->has_target && nya_input_mouse_button_just_pressed(NYA_MOUSE_BUTTON_RIGHT)) {
        s32* p = state->place;
        if (in_bounds(p[0], p[1], p[2]) && state->blocks[p[0]][p[1]][p[2]] == BLOCK_AIR) {
            state->blocks[p[0]][p[1]][p[2]] = state->place_kind;
        }
    }

    // a timed run quits itself once it has drawn its frames, so a headless CI run terminates.
    state->frame_count++;
    if (state->max_frames > 0 && state->frame_count >= state->max_frames) nya_app_get()->should_quit = true;
}

/* LAYER: RENDER */

/** Which way the sun's light travels. One place, so the sky disc and the shading agree. */
NYA_INTERNAL f32x3 sun_travel(void) {
    return nya_vector_normalize((f32x3){ -0.45F, -0.72F, -0.38F });
}

void minecraft_layer_on_render(NYA_Window* window) {
    Minecraft* state = minecraft();

    f32x3 forward = camera_forward(state);

    nya_render3d_begin(window, (NYA_Camera3DPerspective){
                                   .position  = state->eye,
                                   .target    = state->eye + forward,
                                   .far_plane = 300.0F,
                               });

    f32x3 sun = sun_travel();

    // a warm sun over a generous hemispheric ambient, so a block face turned from the sun is still lit, never black.
    nya_render3d_light_set(window, (NYA_Render3DLight){
                                       .direction = sun,
                                       .color     = { 1.0F, 0.96F, 0.86F, 1.0F },
                                       .ambient   = 0.5F,
                                       .intensity = 1.1F,
                                       .sky       = { 0.62F, 0.74F, 0.92F, 1.0F },
                                       .ground    = { 0.34F, 0.30F, 0.26F, 1.0F },
                                   });

    nya_render3d_sky_draw(window, (NYA_Render3DSky){
                                      .zenith        = { 0.32F, 0.52F, 0.82F, 1.0F },
                                      .horizon       = { 0.70F, 0.80F, 0.90F, 1.0F },
                                      .ground        = { 0.24F, 0.28F, 0.30F, 1.0F },
                                      .sun_direction = -sun,
                                      .sun_color     = { 1.0F, 0.92F, 0.74F, 1.0F },
                                      .sun_intensity = 1.3F,
                                  });

    // a matte material for the whole chunk: no metal highlight, soft bands — the flat, chalky voxel look.
    nya_render3d_material_set(window, (NYA_Render3DMaterial){ .roughness = 0.9F });

    // every exposed block, one cube centred on its cell. A walled-in block is skipped (block_exposed), which
    // keeps the draw count to the surface of the chunk rather than its volume.
    u32 drawn = 0;
    for (s32 x = 0; x < WORLD_X; x++) {
        for (s32 y = 0; y < WORLD_Y; y++) {
            for (s32 z = 0; z < WORLD_Z; z++) {
                u8 kind = state->blocks[x][y][z];
                if (kind == BLOCK_AIR || !block_exposed(state, x, y, z)) continue;

                f32x3 center = { (f32)x + 0.5F, (f32)y + 0.5F, (f32)z + 0.5F };
                nya_render3d_cube(window, center, (f32x3){ 1.0F, 1.0F, 1.0F }, nya_quaternion_identity, BLOCK_COLORS[kind]);
                drawn++;
            }
        }
    }

    // the block under the crosshair, outlined so it is clear what a click will act on.
    if (state->has_target) {
        f32x3 center = { (f32)state->target[0] + 0.5F, (f32)state->target[1] + 0.5F, (f32)state->target[2] + 0.5F };
        nya_render3d_cube_outline(window, center, (f32x3){ 1.02F, 1.02F, 1.02F }, nya_quaternion_identity, 0.03F, NYA_COLOR_BLACK);
    }

    nya_render3d_end(window);

    const NYA_FrameStats* frame = &nya_app_get()->frame_stats;
    state->last_fps             = frame->fps;

    // a crosshair at the exact centre of the target, two thin bars, over the flushed scene.
    u32 target_width = 0, target_height = 0;
    nya_render2d_target_size(window, &target_width, &target_height);

    f32 cx = (f32)target_width * 0.5F;
    f32 cy = (f32)target_height * 0.5F;
    nya_render2d_rect(window, cx - 7.0F, cy - 1.0F, 14.0F, 2.0F, NYA_COLOR_WHITE);
    nya_render2d_rect(window, cx - 1.0F, cy - 7.0F, 2.0F, 14.0F, NYA_COLOR_WHITE);

    // the HUD: fps, controls, and which kind a right-click will place.
    f32 y    = 12.0F;
    f32 step = HUD_FONT_SIZE * 1.35F;

    nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, 12.0F, y, NYA_COLOR_WHITE,
                                 "minecraft   %.1f fps   %u blocks drawn", (f64)frame->fps, drawn);
    y += step;
    nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, 12.0F, y, NYA_COLOR_LIGHT_GRAY,
                                 "WASD move · space/shift up/down · mouse look · L break · R place");
    y += step;
    nya_render2d_textf_with_font(window, HUD_FONT, HUD_FONT_SIZE, 12.0F, y, NYA_COLOR_LIGHT_GRAY,
                                 "1 grass · 2 dirt · 3 stone   (placing %s)", block_name(state->place_kind));
}

/* MAIN */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);

    nya_backtrace_init();

    NYA_EXPECT(nya_app_init(.app_id = "minecraft"), "while starting the engine");

    Minecraft* state = nya_arena_alloc(nya_world()->allocator, sizeof(Minecraft));
    *state           = (Minecraft){ .window = NYA_WINDOW_HANDLE_NONE, .place_kind = BLOCK_STONE };

    // a start high and back, looking down at the middle of the chunk; the yaw/pitch match that view.
    state->eye        = (f32x3){ (f32)WORLD_X * 0.5F, (f32)WORLD_Y + 4.0F, (f32)WORLD_Z + 8.0F };
    f32x3 to_center   = nya_vector_normalize((f32x3){ 0.0F, (f32)WORLD_Y * 0.5F, (f32)WORLD_Z * 0.5F } - state->eye);
    state->yaw        = atan2f(to_center.z, to_center.x);
    state->pitch      = asinf(nya_clamp(to_center.y, -1.0F, 1.0F));

    // an optional frame budget, so a headless or CI run draws a fixed number of frames and then quits.
    NYA_ConstCString frames = getenv("NYA_MINECRAFT_FRAMES");
    if (frames != nullptr) state->max_frames = (u32)strtoul(frames, nullptr, 10);

    nya_world_user_data_set(state);

    state->window = nya_window_create(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT, NYA_WINDOW_RESIZABLE);
    nya_assert(nya_window_is_valid(state->window), "the window could not be created");

    nya_layer_push(state->window, nya_layer_of(minecraft_layer, LAYER_ID));

    nya_app_run();

    nya_app_deinit();

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
