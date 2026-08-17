/**
 * Sprite animation: frames, loops, and the events that make a hit land on the right one.
 *
 * The frame walk is what most of this is about. Advancing by dividing the elapsed time is faster and
 * skips every frame in between — so a long tick swallows the marker an attack hangs off, and the
 * attack works at sixty frames a second and not at twenty. Several tests below are long ticks for
 * exactly that reason.
 *
 * Headless throughout: an animator is a frame index and a timer, and neither needs a GPU.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

enum {
  EVENT_WINDUP = 1,
  EVENT_CONNECTS,
  EVENT_ALSO_CONNECTS,
};

/** Six frames at ten a second, so one frame is exactly 0.1 s and the arithmetic is checkable. */
static const NYA_SpriteAnimationEvent attack_events[] = {
  { .frame = 0, .id = EVENT_WINDUP },
  { .frame = 3, .id = EVENT_CONNECTS },
  { .frame = 3, .id = EVENT_ALSO_CONNECTS },
};

static const NYA_SpriteAnimation attack = {
  .first_frame       = 10,
  .frame_count       = 6,
  .frames_per_second = 10.0F,
  .events            = attack_events,
  .event_count       = 3,
};

static const NYA_SpriteAnimation walk = {
  .first_frame       = 0,
  .frame_count       = 4,
  .frames_per_second = 10.0F,
  .looping           = true,
};

static const NYA_SpriteAnimation flicker = {
  .frame_count       = 3,
  .frames_per_second = 10.0F,
  .ping_pong         = true,
  .looping           = true,
};

/** Counts the signals of one kind in a batch, so a test can say what it means. */
static u32 count_of(const NYA_SpriteAnimationSignal* signals, u32 count, NYA_SpriteAnimationSignalKind kind) {
  u32 total = 0;
  for (u32 i = 0; i < count; i++) total += signals[i].kind == kind;
  return total;
}

/** Whether an EVENT with this id is in the batch. */
static b8 has_event(const NYA_SpriteAnimationSignal* signals, u32 count, u32 id) {
  for (u32 i = 0; i < count; i++) {
    if (signals[i].kind == NYA_SPRITE_ANIMATION_EVENT && signals[i].id == id) return true;
  }
  return false;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  NYA_SpriteAnimationSignal signals[NYA_SPRITE_ANIMATION_MAX_SIGNALS];

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: play emits STARTED once, before anything else
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_SpriteAnimator animator = { 0 };
    nya_sprite_animator_play(&animator, &attack);

    // Zero time, so nothing has advanced — but the animation has begun and frame zero is showing.
    u32 count = nya_sprite_animator_advance(&animator, 0.0F, signals, nya_carray_length(signals));

    nya_assert(count_of(signals, count, NYA_SPRITE_ANIMATION_STARTED) == 1, "play then advance emits exactly one STARTED");
    nya_assert(signals[0].kind == NYA_SPRITE_ANIMATION_STARTED, "STARTED comes before frame zero's own marker");

    // A marker on frame zero fires immediately, not when the animation leaves that frame. An attack
    // whose windup sound sits on frame zero would otherwise play a frame late, every single time.
    nya_assert(has_event(signals, count, EVENT_WINDUP), "a marker on frame zero fires on the first advance");

    nya_assert(animator.playing, "it is playing");
    nya_assert(!animator.finished, "and not finished");
    nya_assert(nya_sprite_animator_frame(&animator) == 10, "the atlas frame is first_frame plus zero");

    count = nya_sprite_animator_advance(&animator, 0.0F, signals, nya_carray_length(signals));
    nya_assert(count_of(signals, count, NYA_SPRITE_ANIMATION_STARTED) == 0, "STARTED does not repeat");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a long tick still visits every frame, so no marker is skipped
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_SpriteAnimator animator = { 0 };
    nya_sprite_animator_play(&animator, &attack);

    (void)nya_sprite_animator_advance(&animator, 0.0F, nullptr, 0);

    /*
     * Half a second at ten frames a second is five frames in one tick, straight past frame three.
     *
     * This is the case the frame-by-frame walk exists for: a divide would land on frame five and
     * report nothing in between, so the hit would silently not happen on a slow frame. Both markers
     * on frame three have to come out, because two things happening on one frame is ordinary.
     */
    u32 count = nya_sprite_animator_advance(&animator, 0.5F, signals, nya_carray_length(signals));

    nya_assert(has_event(signals, count, EVENT_CONNECTS), "a tick that skips past the frame still fires its marker");
    nya_assert(has_event(signals, count, EVENT_ALSO_CONNECTS), "and fires every marker on it, not just the first");
    nya_assert(animator.frame == 5, "it landed on the last frame, got " FMTu32, animator.frame);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a non-looping animation finishes exactly once and stops
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_SpriteAnimator animator = { 0 };
    nya_sprite_animator_play(&animator, &attack);

    (void)nya_sprite_animator_advance(&animator, 0.0F, nullptr, 0);

    // Well past the end. Six frames at 0.1 s is 0.6 s of animation.
    u32 count = nya_sprite_animator_advance(&animator, 0.6F, signals, nya_carray_length(signals));

    nya_assert(count_of(signals, count, NYA_SPRITE_ANIMATION_FINISHED) == 1, "it finishes once");
    nya_assert(!animator.playing, "and stops");
    nya_assert(animator.finished, "and says so");

    // Advancing a finished animator does nothing rather than emitting FINISHED again every tick.
    count = nya_sprite_animator_advance(&animator, 1.0F, signals, nya_carray_length(signals));
    nya_assert(count == 0, "a finished animator is silent, got " FMTu32 " signals", count);

    // Resume does not restart it either — replaying is what play is for.
    nya_sprite_animator_resume(&animator);
    nya_assert(!animator.playing, "resume does not revive a finished animation");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a looping animation wraps and counts its loops
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_SpriteAnimator animator = { 0 };
    nya_sprite_animator_play(&animator, &walk);

    (void)nya_sprite_animator_advance(&animator, 0.0F, nullptr, 0);

    // Four frames at 0.1 s is 0.4 s per loop; 0.8 s is two.
    u32 count = nya_sprite_animator_advance(&animator, 0.8F, signals, nya_carray_length(signals));

    nya_assert(count_of(signals, count, NYA_SPRITE_ANIMATION_LOOPED) == 2, "two loops in two loops' worth of time");
    nya_assert(count_of(signals, count, NYA_SPRITE_ANIMATION_FINISHED) == 0, "a looping animation never finishes");
    nya_assert(animator.loops == 2, "and the count agrees, got " FMTu32, animator.loops);
    nya_assert(animator.playing, "it keeps playing");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: ping-pong turns around instead of snapping back
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_SpriteAnimator animator = { 0 };
    nya_sprite_animator_play(&animator, &flicker);

    (void)nya_sprite_animator_advance(&animator, 0.0F, nullptr, 0);

    // Three frames: 0 → 1 → 2, then back 1 → 0. Two ticks puts it at the far end.
    (void)nya_sprite_animator_advance(&animator, 0.1F, nullptr, 0);
    nya_assert(animator.frame == 1, "one step forward, got " FMTu32, animator.frame);

    (void)nya_sprite_animator_advance(&animator, 0.1F, nullptr, 0);
    nya_assert(animator.frame == 2, "two steps forward, got " FMTu32, animator.frame);

    // The turn. The far frame is already on screen, so the next step goes back rather than showing
    // it a second time — repeating it reads as a stutter.
    (void)nya_sprite_animator_advance(&animator, 0.1F, nullptr, 0);
    nya_assert(animator.reversing, "it turned around at the end");
    nya_assert(animator.frame == 1, "and stepped back rather than repeating the last frame, got " FMTu32, animator.frame);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: pause, resume and speed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_SpriteAnimator animator = { 0 };
    nya_sprite_animator_play(&animator, &walk);

    (void)nya_sprite_animator_advance(&animator, 0.0F, nullptr, 0);

    nya_sprite_animator_pause(&animator);
    (void)nya_sprite_animator_advance(&animator, 1.0F, nullptr, 0);
    nya_assert(animator.frame == 0, "a paused animator does not advance, got " FMTu32, animator.frame);

    nya_sprite_animator_resume(&animator);
    (void)nya_sprite_animator_advance(&animator, 0.1F, nullptr, 0);
    nya_assert(animator.frame == 1, "and picks up where it left off, got " FMTu32, animator.frame);

    // Half speed: 0.1 s buys half a frame, so nothing moves until the second one.
    animator.speed = 0.5F;
    (void)nya_sprite_animator_advance(&animator, 0.1F, nullptr, 0);
    nya_assert(animator.frame == 1, "half speed needs twice the time, got " FMTu32, animator.frame);

    (void)nya_sprite_animator_advance(&animator, 0.1F, nullptr, 0);
    nya_assert(animator.frame == 2, "and then advances, got " FMTu32, animator.frame);

    nya_sprite_animator_stop(&animator);
    nya_assert(animator.animation == nullptr, "stop forgets the animation");
    nya_assert(nya_sprite_animator_frame(&animator) == 0, "and reports no frame");

    printf("  PASSED\n");
  }

  printf("PASSED: test_render2d_animation\n");
  return 0;
}
