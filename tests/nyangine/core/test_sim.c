/**
 * The simulation: the command barrier, the record log, and observers.
 *
 * Both halves exist for the same reason — mutating the world while something is iterating it is how
 * a frame corrupts itself. A command is queued during the update and applied at the barrier, once
 * nothing is mid-iteration; a record is a copy of something that happened, handed to observers
 * after the fact. Neither is allowed to take effect the moment it is asked for, and that is what
 * this file checks.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** What a deferred command does when it finally runs. */
static u32 applied_count = 0;
static s32 applied_value = 0;

static void apply_command(void* data) {
  applied_count++;
  applied_value = *(s32*)data;
}

/** A second command, so ordering between two different functions is observable. */
static u32 second_count = 0;

static void apply_second(void* data) {
  nya_unused(data);
  second_count++;
}

/** Observer bookkeeping. */
static u32 observed_records = 0;
static u32 observer_b_calls = 0;

static void observer_a(const NYA_ArrayᐸNYA_SimRecordᐳ* records, void* user_data) {
  nya_unused(user_data);
  observed_records += (u32)records->length;
}

static void observer_b(const NYA_ArrayᐸNYA_SimRecordᐳ* records, void* user_data) {
  nya_unused(records);

  // Proves the user_data pointer arrives intact rather than being dropped or shuffled.
  if (user_data != nullptr) *(u32*)user_data += 1;
  observer_b_calls++;
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  // The world: entities, physics and the simulation barrier, brought up in the order they depend on
  // each other. See core_world.h.
  NYA_World* world = nya_world_create();
  (void)nya_world_set(world);

  defer nya_world_destroy(world);

  defer nya_system_callback_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a fresh simulation has run no ticks and holds nothing
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(nya_sim_tick() == 0, "no tick has happened yet");
    nya_assert(nya_sim_records()->length == 0, "and nothing has been recorded");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a deferred command waits for the barrier
  // ─────────────────────────────────────────────────────────────────────────────
  {
    applied_count = 0;
    applied_value = 0;

    s32 value = 42;
    nya_sim_defer(apply_command, &value, sizeof(value));

    // The whole point: queueing is not applying. A command queued from inside an update must not
    // run until every update for the tick has finished.
    nya_assert(applied_count == 0, "the command has not run yet");

    nya_system_sim_apply_commands();

    nya_assert(applied_count == 1, "the barrier is what runs it, got " FMTu32, applied_count);
    nya_assert(applied_value == 42, "and it received its data, got %d", applied_value);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the command's data is copied, not referenced
  // ─────────────────────────────────────────────────────────────────────────────
  {
    applied_count = 0;
    applied_value = 0;

    // A stack local that goes out of scope before the barrier. If the queue kept the pointer this
    // would read freed stack; because it copies, the value survives.
    {
      s32 scoped = 7;
      nya_sim_defer(apply_command, &scoped, sizeof(scoped));
      scoped = 999;  // overwritten after queueing, to prove the copy was taken at queue time
    }

    nya_system_sim_apply_commands();

    nya_assert(applied_count == 1);
    nya_assert(applied_value == 7, "the copy was taken when the command was queued, got %d", applied_value);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: commands run in the order they were queued
  // ─────────────────────────────────────────────────────────────────────────────
  {
    applied_count = second_count = 0;

    // Order matters for the obvious reason: "spawn the thing" then "point the camera at it" is not
    // the same as the reverse.
    s32 first = 1;
    nya_sim_defer(apply_command, &first, sizeof(first));
    nya_sim_defer(apply_second, nullptr, 0);
    s32 third = 3;
    nya_sim_defer(apply_command, &third, sizeof(third));

    nya_system_sim_apply_commands();

    nya_assert(applied_count == 2, "both apply_command entries ran, got " FMTu32, applied_count);
    nya_assert(second_count == 1);
    nya_assert(applied_value == 3, "the last apply_command to run was the one queued last, got %d", applied_value);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the queue is emptied by the barrier, so nothing runs twice
  // ─────────────────────────────────────────────────────────────────────────────
  {
    applied_count = 0;

    s32 value = 1;
    nya_sim_defer(apply_command, &value, sizeof(value));

    nya_system_sim_apply_commands();
    nya_assert(applied_count == 1);

    // A second barrier with nothing queued must do nothing at all. Re-running the previous tick's
    // commands would double every spawn in the game.
    nya_system_sim_apply_commands();
    nya_assert(applied_count == 1, "the queue was drained, got " FMTu32, applied_count);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: records accumulate and carry their tick
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_system_sim_end_frame();  // clear whatever earlier blocks left behind

    u32 payload = 0xABCD;
    nya_sim_record(1, &payload, sizeof(payload));
    nya_sim_record(2, &payload, sizeof(payload));

    const NYA_ArrayᐸNYA_SimRecordᐳ* records = nya_sim_records();
    nya_assert(records->length == 2, "two records, got " FMTu64, records->length);

    nya_assert(records->items[0].type == 1, "the type is passed through uninterpreted");
    nya_assert(records->items[1].type == 2);
    nya_assert(records->items[0].tick == nya_sim_tick(), "a record is stamped with the tick that produced it");

    // The payload is copied the same way a command's is.
    nya_assert(records->items[0].size == sizeof(payload));
    nya_assert(*(u32*)records->items[0].data == 0xABCD, "the recorded bytes survived");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: end of frame clears the records
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Records live in the frame arena and describe one frame. Carrying them over would mean an
    // observer sees the same event on every subsequent frame.
    nya_assert(nya_sim_records()->length > 0, "carried over from the block above");

    nya_system_sim_end_frame();

    nya_assert(nya_sim_records()->length == 0, "the frame ended, so its record log is gone");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: observers see the frame's records, with their user data
  // ─────────────────────────────────────────────────────────────────────────────
  {
    observed_records = observer_b_calls = 0;
    u32 counter      = 0;

    NYA_EXPECT(nya_sim_observer_add(nya_callback(observer_a), nullptr));
    NYA_EXPECT(nya_sim_observer_add(nya_callback(observer_b), &counter));

    u32 payload = 1;
    nya_sim_record(10, &payload, sizeof(payload));
    nya_sim_record(11, &payload, sizeof(payload));
    nya_sim_record(12, &payload, sizeof(payload));

    nya_system_sim_end_frame();

    nya_assert(observed_records == 3, "the observer saw every record from the frame, got " FMTu32, observed_records);
    nya_assert(observer_b_calls == 1, "each observer is called once per frame, not once per record");
    nya_assert(counter == 1, "user_data arrived intact");

    // A frame with nothing recorded still ends; whether observers run for it is the system's
    // business, but the counters must not go backwards and nothing may fault.
    u32 before = observed_records;
    nya_system_sim_end_frame();
    nya_assert(observed_records == before, "an empty frame contributes no records");

    nya_sim_observer_clear();

    nya_sim_record(13, &payload, sizeof(payload));
    nya_system_sim_end_frame();
    nya_assert(observed_records == before, "a cleared observer is not called again");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a record with no payload is allowed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_system_sim_end_frame();

    // "The player died" needs no data beyond its type, and forcing a dummy payload on every such
    // event would be noise at each call site.
    nya_sim_record(99, nullptr, 0);

    const NYA_ArrayᐸNYA_SimRecordᐳ* records = nya_sim_records();
    nya_assert(records->length == 1);
    nya_assert(records->items[0].type == 99);
    nya_assert(records->items[0].size == 0, "no payload means no bytes");

    nya_system_sim_end_frame();
  }

  printf("PASSED: test_sim\n");
  return 0;
}
