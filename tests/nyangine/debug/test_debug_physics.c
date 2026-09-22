/**
 * The collision shape overlay: nya_debug_physics3d_draw and nya_debug_physics2d_draw.
 *
 * What a test can hold to account is which bodies are drawn and which are not, which is what the count
 * each of them returns says. Counting the geometry instead would need a GPU: the draw calls reach
 * _nya_render3d_object_begin, which cannot reserve room in a batch that has no buffers, so on a runner
 * with no device every shape records nothing and every count is zero.
 *
 * So this does not say the shapes are the right shapes, and nothing headless can. That is what the
 * switchboard's hitbox toggle in the 3D scene is for.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** A window with a known size, as in test_render3d.c: a headless build has none to learn one from. */
static NYA_Window* make_window(void) {
  NYA_WindowHandle handle = nya_window_create("hitboxes", 800, 600, NYA_WINDOW_NONE);
  nya_assert(nya_window_is_valid(handle), "the window was created");

  NYA_Window* window = nya_window_get(handle);
  nya_assert(window != nullptr);

  window->screen_width  = 800;
  window->screen_height = 600;

  return window;
}

/** Bodies the 3D overlay draws for the world as it stands. */
static u32 drawn_3d(NYA_Window* window) {
  nya_render3d_begin(window, (NYA_Camera3DPerspective){ .position = { 0.0F, 4.0F, 8.0F }, .target = { 0.0F, 0.0F, 0.0F } });

  return nya_debug_physics3d_draw(window);
}

s32 main(void) {
  // no display on CI, and nothing here is ever presented: the offscreen driver gives it a window anywhere.
  SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_window_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);
  defer nya_system_window_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  NYA_Window* window = make_window();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an empty world draws nothing, and so does a world of entities that
  //       carry no body
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_check(drawn_3d(window) == 0, "an empty world draws no hitboxes");

    NYA_EntityHandle bare = nya_entity_spawn(.name = "no body", .position = { 0.0F, 0.0F, 0.0F });
    nya_check(drawn_3d(window) == 0, "an entity with no body draws no hitbox");

    nya_entity_despawn(bare);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: each drawable shape produces geometry, and more bodies produce more
  // ─────────────────────────────────────────────────────────────────────────────
  u32 box_only = 0;
  {
    NYA_EntityHandle box = nya_entity_spawn(.name = "box", .position = { 0.0F, 0.0F, 0.0F });
    nya_assert(nya_physics3d_body_attach(box, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1.0F, 1.0F, 1.0F }));

    box_only = drawn_3d(window);
    nya_check(box_only == 1, "a box body draws one hitbox, got " FMTu32, box_only);

    NYA_EntityHandle sphere = nya_entity_spawn(.name = "sphere", .position = { 2.0F, 0.0F, 0.0F });
    nya_assert(nya_physics3d_body_attach(sphere, .shape = NYA_PHYSICS3D_SHAPE_SPHERE, .radius = 0.5F));

    const u32 with_sphere = drawn_3d(window);
    nya_check(with_sphere == 2, "a sphere adds to it, got " FMTu32, with_sphere);

    NYA_EntityHandle capsule = nya_entity_spawn(.name = "capsule", .position = { -2.0F, 0.0F, 0.0F });
    nya_assert(nya_physics3d_body_attach(capsule, .type = NYA_PHYSICS_BODY_KINEMATIC, .shape = NYA_PHYSICS3D_SHAPE_CAPSULE, .radius = 0.3F,
                                         .length = 1.0F));

    const u32 with_capsule = drawn_3d(window);
    nya_check(with_capsule == 3, "and so does a capsule, got " FMTu32, with_capsule);

    nya_entity_despawn(capsule);
    nya_entity_despawn(sphere);

    nya_check(drawn_3d(window) == box_only, "and despawning them puts the count back to the box alone");

    nya_entity_despawn(box);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a heightfield is skipped rather than drawn. A terrain's outline is the
  //       terrain, which would hide the scene it is there to explain.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Flat and tiny. The overlay never reads the heights; what matters is that the body exists.
    static const f32 heights[16] = { 0 };

    NYA_EntityHandle ground = nya_entity_spawn(.name = "ground", .position = { 0.0F, -2.0F, 0.0F });
    nya_assert(nya_physics3d_body_attach(ground, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_HEIGHTFIELD, .heights = heights,
                                         .height_count_x = 4, .height_count_z = 4, .height_cell_size = { 1.0F, 1.0F }));

    nya_check(drawn_3d(window) == 0, "a heightfield body draws no hitbox, got " FMTu32, drawn_3d(window));

    nya_entity_despawn(ground);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the 2D overlay walks the 2D solver's bodies and nobody else's
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // A 3D body is not a 2D one: the two solvers share an entity table and neither draws the other's.
    NYA_EntityHandle solid = nya_entity_spawn(.name = "3d box", .position = { 0.0F, 0.0F, 0.0F });
    nya_assert(nya_physics3d_body_attach(solid, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1.0F, 1.0F, 1.0F }));

    nya_check(nya_debug_physics2d_draw(window) == 0, "the 2D overlay does not draw a 3D body");

    NYA_EntityHandle crate = nya_entity_spawn(.name = "crate", .position = { 100.0F, 100.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(crate, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 32.0F, 32.0F }));

    NYA_EntityHandle ball = nya_entity_spawn(.name = "ball", .position = { 160.0F, 100.0F, 0.0F });
    nya_assert(nya_physics2d_body_attach(ball, .shape = NYA_PHYSICS2D_SHAPE_CIRCLE, .radius = 8.0F));

    const u32 drawn = nya_debug_physics2d_draw(window);
    nya_check(drawn == 2, "and draws the box and the circle, got " FMTu32, drawn);

    nya_entity_despawn(ball);
    nya_entity_despawn(crate);
    nya_entity_despawn(solid);

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_debug_physics");

  return nya_check_failures() == 0 ? 0 : 1;
}
