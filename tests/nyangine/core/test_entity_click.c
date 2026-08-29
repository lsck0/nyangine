/**
 * Picking, in both dimensions.
 *
 * `on_click` is a field on every entity, and until recently only half of them could ever receive it:
 * nya_entity_click took an f32x2 and asked nya_physics2d_entity_at, so a 3D entity's callback was
 * unreachable — the 3D scene in gnyame did its own raycast and never ran it. Nothing failed loudly;
 * the callback simply never fired.
 *
 * So what is checked here is mostly that both paths reach the same place: the same callback type,
 * the same "no callback means not clickable" rule, and a world point that means something in each
 * case — the clicked position in 2D, the struck surface in 3D.
 *
 * Headless: both solvers need an arena and a clock and nothing else.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

static u32              clicks       = 0;
static u8               last_button  = 0;
static f32x3            last_point   = { 0 };
static NYA_EntityHandle last_clicked = NYA_ENTITY_HANDLE_NONE;

static void on_click(NYA_Entity* entity, f32x3 world_point, u8 button) {
  clicks++;
  last_button  = button;
  last_point   = world_point;
  last_clicked = entity->handle;
}

static void reset(void) {
  clicks       = 0;
  last_button  = 0;
  last_point   = (f32x3){ 0 };
  last_clicked = NYA_ENTITY_HANDLE_NONE;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();

  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);
  defer nya_system_callback_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: 2D — a click on a body runs its callback
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    NYA_EntityHandle crate = nya_entity_spawn(.name = "crate", .position = { 100.0F, 100.0F, 0.0F }, .on_click = nya_callback(on_click));

    // Static, so it does not fall out from under the point being clicked before it is clicked.
    b8 ok = nya_physics2d_body_attach(
      crate, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 40.0F, 40.0F }
    );
    nya_assert(ok);

    NYA_EntityHandle hit = nya_entity_click((f32x2){ 100.0F, 100.0F }, NYA_MOUSE_BUTTON_RIGHT);

    nya_assert(clicks == 1, "a click inside the body runs on_click exactly once");
    nya_assert(hit.index == crate.index && hit.generation == crate.generation, "and reports who it was");
    nya_assert(last_clicked.index == crate.index);
    nya_assert(last_button == NYA_MOUSE_BUTTON_RIGHT, "the button is passed through rather than assumed");

    // z is zero for a 2D click, and that is not a fiction: the 2D world is the z = 0 plane.
    nya_assert(last_point.x == 100.0F && last_point.y == 100.0F);
    nya_assert(last_point.z == 0.0F, "a 2D click reports z zero");

    reset();

    NYA_EntityHandle miss = nya_entity_click((f32x2){ 1000.0F, 1000.0F }, NYA_MOUSE_BUTTON_LEFT);
    nya_assert(clicks == 0, "a click on empty space runs nothing");
    nya_assert(!nya_entity_is_valid(miss));

    nya_entity_despawn(crate);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: 2D — an entity with no on_click is not clickable
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    // No callback at all. Not having one *is* declining to be clickable; there is deliberately no
    // separate flag, so there is nothing for the two to disagree about.
    NYA_EntityHandle terrain = nya_entity_spawn(.name = "terrain", .position = { 0.0F, 0.0F, 0.0F });

    b8 ok = nya_physics2d_body_attach(
      terrain, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 100.0F, 100.0F }
    );
    nya_assert(ok);

    NYA_EntityHandle hit = nya_entity_click((f32x2){ 0.0F, 0.0F }, NYA_MOUSE_BUTTON_LEFT);

    nya_assert(clicks == 0);
    nya_assert(!nya_entity_is_valid(hit), "hitting something that declines reads as hitting nothing");

    nya_entity_despawn(terrain);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: 3D — a ray runs the struck entity's callback
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    NYA_EntityHandle cube = nya_entity_spawn(.name = "cube", .position = { 0.0F, 0.0F, 0.0F }, .on_click = nya_callback(on_click));

    b8 ok = nya_physics3d_body_attach(
      cube, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 2.0F, 2.0F, 2.0F }
    );
    nya_assert(ok);

    // Ten units up the z axis, firing back at the origin. Length is reach, so this comfortably gets
    // there.
    NYA_EntityHandle hit = nya_entity_click((f32x3){ 0.0F, 0.0F, 10.0F }, (f32x3){ 0.0F, 0.0F, -20.0F }, NYA_MOUSE_BUTTON_LEFT);

    nya_assert(clicks == 1, "a ray through a body runs its on_click — the case that was unreachable before");
    nya_assert(hit.index == cube.index && hit.generation == cube.generation);
    nya_assert(last_button == NYA_MOUSE_BUTTON_LEFT);

    /*
     * The point is on the struck surface, not the ray's origin.
     *
     * Handing back the origin would make every click on an object report the camera's position,
     * which is the same value for every click and therefore useless for deciding which face was hit.
     * The cube is two units across at the origin, so its near face is at z = 1.
     */
    nya_assert(last_point.z > 0.5F && last_point.z < 1.5F, "the hit point is on the near_plane face, not at the ray origin");
    nya_assert(last_point.z < 10.0F, "and is certainly not where the ray started");

    reset();

    // Pointing away. The ray's length is its reach, so a ray that stops short is a miss too.
    NYA_EntityHandle behind = nya_entity_click((f32x3){ 0.0F, 0.0F, 10.0F }, (f32x3){ 0.0F, 0.0F, 20.0F }, NYA_MOUSE_BUTTON_LEFT);
    nya_assert(clicks == 0, "a ray pointing away hits nothing");
    nya_assert(!nya_entity_is_valid(behind));

    NYA_EntityHandle short_ray = nya_entity_click((f32x3){ 0.0F, 0.0F, 10.0F }, (f32x3){ 0.0F, 0.0F, -1.0F }, NYA_MOUSE_BUTTON_LEFT);
    nya_assert(clicks == 0, "a ray too short to reach hits nothing");
    nya_assert(!nya_entity_is_valid(short_ray));

    nya_entity_despawn(cube);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: 3D — the nearest body wins, and one behind it cannot take the click
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    NYA_EntityHandle near_plane = nya_entity_spawn(.name = "near", .position = { 0.0F, 0.0F, 0.0F }, .on_click = nya_callback(on_click));
    NYA_EntityHandle far_plane  = nya_entity_spawn(.name = "far", .position = { 0.0F, 0.0F, -10.0F }, .on_click = nya_callback(on_click));

    nya_assert(nya_physics3d_body_attach(near_plane, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 2.0F, 2.0F, 2.0F }));
    nya_assert(nya_physics3d_body_attach(far_plane, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 2.0F, 2.0F, 2.0F }));

    NYA_EntityHandle hit = nya_entity_click((f32x3){ 0.0F, 0.0F, 20.0F }, (f32x3){ 0.0F, 0.0F, -60.0F }, NYA_MOUSE_BUTTON_LEFT);

    nya_assert(clicks == 1, "only one entity is clicked; the ray does not fall through");
    nya_assert(hit.index == near_plane.index, "the nearest body takes the click, not the first one reported");

    nya_entity_despawn(near_plane);
    nya_entity_despawn(far_plane);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: 3D — an entity with no on_click declines, exactly as in 2D
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    // The ground in a 3D scene: hit constantly, and clickable by nobody.
    NYA_EntityHandle ground = nya_entity_spawn(.name = "ground", .position = { 0.0F, 0.0F, 0.0F });

    nya_assert(nya_physics3d_body_attach(ground, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = { 20.0F, 1.0F, 20.0F }));

    NYA_EntityHandle hit = nya_entity_click((f32x3){ 0.0F, 10.0F, 0.0F }, (f32x3){ 0.0F, -20.0F, 0.0F }, NYA_MOUSE_BUTTON_LEFT);

    nya_assert(clicks == 0);
    nya_assert(!nya_entity_is_valid(hit), "the same rule as 2D: no callback, no click");

    nya_entity_despawn(ground);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a despawned entity is not clickable, and the click does not fault
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset();

    NYA_EntityHandle crate = nya_entity_spawn(.name = "crate", .position = { 50.0F, 50.0F, 0.0F }, .on_click = nya_callback(on_click));
    nya_assert(nya_physics2d_body_attach(crate, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS2D_SHAPE_BOX, .size = { 20.0F, 20.0F }));

    nya_entity_despawn(crate);

    // The body went with the entity, so there is nothing at that point any more. Clicking where it
    // used to be must miss rather than resolve a stale handle.
    NYA_EntityHandle hit = nya_entity_click((f32x2){ 50.0F, 50.0F }, NYA_MOUSE_BUTTON_LEFT);

    nya_assert(clicks == 0, "a despawned entity is not clickable");
    nya_assert(!nya_entity_is_valid(hit));
  }

  nya_log_info("PASSED: test_entity_click (0 failures)");

  return EXIT_SUCCESS;
}
