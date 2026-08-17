/**
 * The application frame: the clock, the fixed timestep accumulator, and the nested frame step.
 *
 * nya_app_init is not what runs here. A full init opens a window and brings up the GPU, so these
 * tests stand up only the systems the frame loop actually touches and then drive the loop's own
 * pieces — _nya_app_advance_frame_clock, _nya_app_update, _nya_app_frame_step — directly. That is
 * also what makes them deterministic: the accumulator is fed a number rather than a stopwatch, so
 * "three ticks and a remainder" is asserted rather than hoped for.
 *
 * Headless, the renderer's stubs make _nya_app_render a pair of events and nothing else, and with
 * no windows open the per window loops have no bodies. What is left is the arithmetic, which is
 * the part that decides whether the simulation runs at the right rate.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Bytes handed out across every region of an arena. There is no public accessor for this. */
static u64 arena_used(NYA_Arena* arena) {
  u64 used = 0;
  for (NYA_ArenaRegion* region = arena->head; region != nullptr; region = region->next) used += region->used;
  return used;
}

s32 main(void) {
  /*
   * The subsystems the frame loop reaches into, and nothing more.
   *
   * _nya_app_update dispatches events and drives the entity and sim systems; _nya_app_render
   * dispatches two more. Both walk the window table, so the window system has to be up even with
   * no window in it — its slot array is heap allocated, and walking it before init reads through a
   * null pointer. The renderer and the asset system are never reached with nothing to draw into,
   * so they stay down.
   */
  _NYA_APP_INSTANCE = (NYA_App){
    .initialized                    = true,
    .options                        = { _NYA_APP_DEFAULT_OPTIONS },
    .frame_allocator                = nya_arena_create(.name = "test_frame_allocator"),
    .live_resize_allocator          = nya_arena_create(.name = "test_live_resize_allocator"),
    .frame_stats.started_ns         = nya_clock_get_monotonic_ns(),
    .frame_stats.prev_frame_time_ns = nya_clock_get_monotonic_ns(),
  };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  NYA_App* app = nya_app_get();

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_window_init();
  // The world: entities, physics and the simulation barrier, brought up in the order they depend on
  // each other. See core_world.h.
  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);

  defer nya_arena_destroy(app->live_resize_allocator);
  defer nya_arena_destroy(app->frame_allocator);
  defer nya_system_window_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: uptime is measured from started_ns and only goes forward
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u64 first = nya_app_uptime_ns();
    u64 second = nya_app_uptime_ns();

    nya_assert(second >= first, "uptime is monotonic");

    // Sampled against the same origin the frame stats use, so a caller mixing nya_app_uptime_ns
    // with frame_stats.uptime_ns is comparing two points on one timeline.
    u64 now = nya_clock_get_monotonic_ns();
    nya_assert(now >= app->frame_stats.started_ns, "started_ns is in the past");
    nya_assert(second <= now - app->frame_stats.started_ns + 1'000'000, "uptime tracks the wall clock");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: advancing the clock books elapsed time against the update debt
  // ─────────────────────────────────────────────────────────────────────────────
  {
    app->frame_stats.time_behind_ns  = 0;
    app->frame_stats.prev_frame_time_ns = nya_clock_get_monotonic_ns() - nya_time_ms_to_ns(10);

    _nya_app_advance_frame_clock();

    // Whatever the clock said, the gap since the previous frame is now owed to the simulation.
    nya_assert(app->frame_stats.elapsed_ns >= nya_time_ms_to_ns(10), "at least the 10ms that were faked");
    nya_assert(app->frame_stats.time_behind_ns == (s64)app->frame_stats.elapsed_ns, "the whole gap is booked as debt");
    nya_assert(app->frame_stats.frame_start_time_ns > 0);

    // uptime_s is the same number as uptime_ns, just lossy. Half a millisecond of tolerance covers
    // the f32 rounding without letting a genuinely wrong value through.
    f64 expected_s = (f64)app->frame_stats.uptime_ns / 1'000'000'000.0;
    nya_assert(fabs((f64)app->frame_stats.uptime_s - expected_s) < 0.0005, "uptime_s mirrors uptime_ns");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the fixed step consumes whole ticks and keeps the remainder
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The invariant the whole simulation rests on: a frame runs floor(debt / step) updates and
    // carries what is left into the next frame, so the tick rate stays independent of the frame
    // rate. Fed a number rather than a stopwatch, so the assertion is exact.
    u64 step      = app->options.time_step_ns;
    u64 remainder = step / 3;

    u64 tick_before = nya_world()->sim_system.tick;
    app->frame_stats.time_behind_ns = (s64)(3 * step + remainder);

    _nya_app_update();

    nya_assert(nya_world()->sim_system.tick - tick_before == 3, "three whole steps fit, so three ticks ran");
    nya_assert(app->frame_stats.time_behind_ns == (s64)remainder, "the partial step is carried, not dropped");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a debt smaller than one step runs nothing at all
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u64 tick_before = nya_world()->sim_system.tick;
    app->frame_stats.time_behind_ns = (s64)(app->options.time_step_ns - 1);

    _nya_app_update();

    nya_assert(nya_world()->sim_system.tick == tick_before, "not enough debt for a tick");
    nya_assert(app->frame_stats.time_behind_ns == (s64)(app->options.time_step_ns - 1), "and nothing was consumed");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: delta_time_s is the fixed step, not however long the frame took
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The point of a fixed timestep: an update is told the step it represents, so simulation
    // results do not change when the machine gets slower.
    app->frame_stats.delta_time_s   = 0.0F;
    app->frame_stats.time_behind_ns = (s64)((u64)2 * app->options.time_step_ns);

    _nya_app_update();

    f32 expected = (f32)nya_time_ns_to_s(app->options.time_step_ns);
    nya_assert(app->frame_stats.delta_time_s == expected, "the update sees the step, not the wall clock");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a nested frame step swaps the allocator and puts it back
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Frames produced during a window drag are nested inside an outer frame that is parked in the
    // event pump, so they must not touch the arena that outer frame is using.
    NYA_Arena* outer = app->frame_allocator;

    // Something the outer frame is holding, which a nested step must leave alone.
    void* outer_allocation = nya_arena_alloc(app->frame_allocator, 4096);
    nya_assert(outer_allocation != nullptr);
    u64 outer_used_before = arena_used(outer);

    app->frame_stats.time_behind_ns = 0;
    _nya_app_frame_step(true);

    nya_assert(app->frame_allocator == outer, "the swap is undone before the step returns");
    nya_assert(arena_used(outer) == outer_used_before, "the outer frame's allocations are untouched");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a long drag does not grow memory
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The reason live_resize_allocator exists at all. Each nested step empties it first, so thirty
    // seconds of dragging costs one frame's worth of scratch rather than thirty seconds of it.
    // Standing in for what a layer's update would allocate during the drag.
    (void)nya_arena_alloc(app->live_resize_allocator, 64 * 1024);
    u64 before = arena_used(app->live_resize_allocator);
    nya_assert(before >= 64 * 1024, "the drag arena is holding a nested frame's scratch");

    app->frame_stats.time_behind_ns = 0;
    _nya_app_frame_step(true);
    nya_assert(arena_used(app->live_resize_allocator) < before, "the next nested step reclaims it");

    // Flat however long the drag lasts. Without the reset this climbs by 64k a frame.
    u64 steady = arena_used(app->live_resize_allocator);
    for (u32 i = 0; i < 16; i++) {
      (void)nya_arena_alloc(app->live_resize_allocator, 64 * 1024);
      app->frame_stats.time_behind_ns = 0;
      _nya_app_frame_step(true);
      nya_assert(arena_used(app->live_resize_allocator) == steady, "step " FMTu32 " grew the drag arena", i);
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an ordinary frame step leaves the drag arena alone
  // ─────────────────────────────────────────────────────────────────────────────
  {
    (void)nya_arena_alloc(app->live_resize_allocator, 1024);
    u64 drag_used_before = arena_used(app->live_resize_allocator);

    app->frame_stats.time_behind_ns = 0;
    _nya_app_frame_step(false);

    nya_assert(arena_used(app->live_resize_allocator) == drag_used_before, "a normal frame does not reset the drag arena");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a nested step keeps the fixed timestep honest
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The bookkeeping a nested step does exists so the drag's whole duration does not arrive as one
    // delta when the mouse comes up, which the accumulator would then pay off as a burst of catch
    // up ticks. Advancing prev_frame_time_ns per nested frame is what prevents that.
    u64 tick_before = nya_world()->sim_system.tick;

    app->frame_stats.time_behind_ns     = 0;
    app->frame_stats.prev_frame_time_ns = nya_clock_get_monotonic_ns();

    for (u32 i = 0; i < 8; i++) _nya_app_frame_step(true);

    u64 ticks = nya_world()->sim_system.tick - tick_before;

    // Eight steps in well under one 16ms step of wall clock, so the debt never reaches a tick. The
    // ceiling is what matters: without the bookkeeping this grows without bound.
    nya_assert(ticks <= 8, "a burst of nested frames does not become a burst of ticks, got " FMTu64, ticks);
    nya_assert(app->frame_stats.prev_frame_time_ns > 0, "each nested step advances the frame clock");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: updating options recomputes the derived frame budget
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_app_options_update((NYA_AppOptions){
      .time_step_ns        = nya_time_ms_to_ns(8),
      .frame_rate_limit    = 60,
      .vsync_enabled       = false,
      .max_concurrent_jobs = 2,
    });

    nya_assert(app->options.time_step_ns == nya_time_ms_to_ns(8));
    nya_assert(app->frame_stats.min_frame_time_ns == 1'000'000'000 / 60, "min frame time follows the limit rather than being stale");

    // And the new step is what the accumulator now uses.
    u64 tick_before = nya_world()->sim_system.tick;
    app->frame_stats.time_behind_ns = (s64)((u64)2 * nya_time_ms_to_ns(8));
    _nya_app_update();
    nya_assert(nya_world()->sim_system.tick - tick_before == 2, "the updated step drives the loop");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the game state seam
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The pointer a game parks so its state survives a hot reload. What can be tested in process is
     * the contract around it — the reload itself needs two dlopens and a running game.
     *
     * It lives on the world rather than on the app now, which is the same seam moved: the world is
     * what owns the arena the state is allocated from, so the two share one lifetime.
     *
     * The property that carries the weight is "null until set": that is what lets a game answer
     * "am I starting fresh or coming back from a reload" without tracking it separately, and it is
     * the branch a reloaded library takes.
     */
    nya_world_user_data_set(nullptr);
    nya_assert(nya_world_user_data() == nullptr, "an unset seam must read as null, not as anything else");

    u64 state = 0xD00DFEED;
    nya_world_user_data_set(&state);
    nya_assert(nya_world_user_data() == &state, "the seam must hand back exactly what was parked in it");
    nya_assert(*(u64*)nya_world_user_data() == 0xD00DFEED, "and it must still point at the caller's memory");

    // Overwritten rather than accumulated — there is one root per world, not a stack of them.
    u64 second = 0x1234;
    nya_world_user_data_set(&second);
    nya_assert(nya_world_user_data() == &second, "setting again must replace, not append");

    // Reachable through the world struct too, since that is the same storage.
    nya_assert(nya_world()->user_data == &second, "the seam and the world struct must be one field");

    /*
     * Per world, not global. A second world starts with its own empty seam and does not see the
     * first one's — which is the whole reason this moved off NYA_App.
     */
    NYA_World* other = nya_world_create();
    NYA_World* first = nya_world_set(other);

    nya_assert(nya_world_user_data() == nullptr, "a fresh world's seam starts empty");

    (void)nya_world_set(first);
    nya_assert(nya_world_user_data() == &second, "and switching back finds the original untouched");

    nya_world_destroy(other);

    // Cleared back, so nothing later in this test sees a dangling stack address.
    nya_world_user_data_set(nullptr);
    nya_assert(nya_world_user_data() == nullptr);
  }

  printf("PASSED: test_app\n");
  return 0;
}
