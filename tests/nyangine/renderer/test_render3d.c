/**
 * 3D picking: the path a click actually takes, end to end.
 *
 * A click goes screen pixel → nya_render3d_screen_ray → nya_physics3d_raycast → entity, and every
 * one of those steps is arithmetic nobody can check by eye. This walks it with a camera in a known
 * place and a cube at a known point, which is the only way the answer is verifiable.
 *
 * The regression it exists for: screen_ray used to require nya_render3d_begin to be *currently* open.
 * A click arrives during on_event and a camera is set during on_render, one phase later — so the ray
 * fell back to the origin pointing along -z, hit nothing, and clicking the cube silently did nothing.
 *
 * Headless: render3d_headless.c does the ray arithmetic for real, precisely so this test means
 * something. Only the drawing is stubbed.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

enum { KIND_CUBE = 3 };

/** A window with a known size, so screen coordinates in this file are checkable by hand. */
static NYA_Window* make_window(void) {
  NYA_WindowHandle handle = nya_window_create("picking", 800, 600, NYA_WINDOW_NONE);
  nya_assert(nya_window_is_valid(handle), "the window was created");

  NYA_Window* window = nya_window_get(handle);
  nya_assert(window != nullptr);

  /*
   * Set by hand, because a headless build creates no SDL window and therefore learns no size — a
   * real one takes it from the swapchain. Every screen coordinate in this file is relative to these
   * two numbers, so they have to be something rather than zero: a target of no size has no centre,
   * and screen_ray correctly refuses to invent one.
   */
  window->screen_width  = 800;
  window->screen_height = 600;

  return window;
}

s32 main(void) {
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


  // Four units back along +z, looking at the origin. The camera's own -z therefore points along the
  // world's -z, which makes every expectation below signable by hand.
  NYA_Camera3DPerspective camera = {
    .position = { 0.0F, 0.0F, 4.0F },
    .target   = { 0.0F, 0.0F, 0.0F },
  };

  f32x2 center = { 400.0F, 300.0F };

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: with no camera ever set, a ray hits nothing rather than something wrong
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Render3DRay ray = nya_render3d_screen_ray(window, center);

    nya_assert(ray.direction.z == -1.0F, "the fallback points along -z");
    nya_assert(ray.origin.x == 0.0F && ray.origin.y == 0.0F && ray.origin.z == 0.0F, "from the origin");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the ray survives nya_render3d_end
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_render3d_begin(window, camera);
    nya_render3d_end(window);

    /*
     * The regression. `active` is false here — the scene is closed, exactly as it is when the next
     * frame's on_event runs — and the ray still has to be the one the player was looking along.
     */
    nya_assert(!nya_render3d_active(window), "the scene is closed, which is the state a click arrives in");

    NYA_Render3DRay ray = nya_render3d_screen_ray(window, center);

    nya_assert(ray.origin.z == 4.0F, "the ray starts at the camera, got %f", (f64)ray.origin.z);
    nya_assert(ray.direction.z < -0.99F, "and points into the scene, got %f", (f64)ray.direction.z);
    nya_assert(fabsf(ray.direction.x) < 0.001F && fabsf(ray.direction.y) < 0.001F, "dead centre means straight ahead");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: off-centre pixels aim off-centre, the right way round
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_render3d_begin(window, camera);
    nya_render3d_end(window);

    // Screen y grows downward and clip space y grows up, so a pixel *below* centre has to aim
    // *down* — a missing flip here is the classic inverted picker.
    NYA_Render3DRay lower = nya_render3d_screen_ray(window, (f32x2){ 400.0F, 500.0F });
    nya_assert(lower.direction.y < 0.0F, "a pixel below centre aims downward, got %f", (f64)lower.direction.y);

    NYA_Render3DRay upper = nya_render3d_screen_ray(window, (f32x2){ 400.0F, 100.0F });
    nya_assert(upper.direction.y > 0.0F, "and one above aims upward, got %f", (f64)upper.direction.y);

    /*
     * Standing at +z looking at the origin with +y up, the world's +x is on your right — the camera
     * basis is `right = forward x up`, and forward here is (0, 0, -1), which crosses with (0, 1, 0)
     * to give (1, 0, 0).
     *
     * Worth signing by hand rather than assuming, because a transposed basis mirrors picking about
     * the screen centre and looks almost right: the cube is still selectable, and everything either
     * side of it selects its neighbour.
     */
    NYA_Render3DRay right = nya_render3d_screen_ray(window, (f32x2){ 700.0F, 300.0F });
    nya_assert(right.direction.x > 0.0F, "right of centre aims along +x from this camera, got %f", (f64)right.direction.x);

    // Always unit length, so a caller can scale it by a distance and mean it.
    nya_assert(fabsf(nya_vector_length(right.direction) - 1.0F) < 0.001F, "the direction is normalized");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the whole click path finds the cube and misses when it should
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EntityHandle cube = nya_entity_spawn(.name = "cube", .type = KIND_CUBE, .position = { 0.0F, 0.0F, 0.0F });

    nya_assert(nya_physics3d_body_attach(cube, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 1.0F, 1.0F, 1.0F }),
               "the cube got a body");

    nya_render3d_begin(window, camera);
    nya_render3d_end(window);

    NYA_Render3DRay ray = nya_render3d_screen_ray(window, center);

    f32x3 point  = f32x3_zero;
    f32x3 normal = f32x3_zero;

    NYA_EntityHandle hit = nya_physics3d_raycast(ray.origin, ray.direction * 100.0F, &point, &normal);

    nya_assert(nya_entity_is_valid(hit), "the ray found something");
    nya_assert(hit.index == cube.index, "and it is the cube");

    // The near face of a unit cube at the origin, seen from +z, is at z = 0.5 — and its normal
    // points back at the camera.
    nya_assert(fabsf(point.z - 0.5F) < 0.01F, "it struck the near face, got %f", (f64)point.z);
    nya_assert(normal.z > 0.9F, "whose normal faces the camera, got %f", (f64)normal.z);

    // A corner of the screen aims well past a one metre cube.
    NYA_Render3DRay miss = nya_render3d_screen_ray(window, (f32x2){ 10.0F, 10.0F });
    nya_assert(!nya_entity_is_valid(nya_physics3d_raycast(miss.origin, miss.direction * 100.0F, nullptr, nullptr)), "a corner pixel misses");

    // And a ray that stops short of the cube misses it too, which is what the length of the
    // direction vector is for.
    nya_assert(!nya_entity_is_valid(nya_physics3d_raycast(ray.origin, ray.direction * 1.0F, nullptr, nullptr)), "a short ray falls short");

    nya_entity_despawn(cube);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an orthographic camera moves the ray's origin, not its direction
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_render3d_begin_orthographic(window, (NYA_Camera3DOrthographic){
      .position = { 0.0F, 0.0F, 4.0F },
      .target   = { 0.0F, 0.0F, 0.0F },
      .height   = 10.0F,
    });
    nya_render3d_end(window);

    NYA_Render3DRay middle = nya_render3d_screen_ray(window, center);
    NYA_Render3DRay offset = nya_render3d_screen_ray(window, (f32x2){ 700.0F, 300.0F });

    // Parallel rays: every pixel points the same way and the pixel chooses *where* the ray starts.
    // Getting this backwards gives a picker that works in the middle of the screen and nowhere else.
    nya_assert(fabsf(middle.direction.z - offset.direction.z) < 0.001F, "orthographic rays are parallel");
    nya_assert(fabsf(offset.origin.x - middle.origin.x) > 0.1F, "and the pixel moves the origin instead");

    printf("  PASSED\n");
  }

  printf("PASSED: test_render3d\n");
  return 0;
}
