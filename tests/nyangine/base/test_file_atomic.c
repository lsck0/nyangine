/**
 * nya_file_write_atomic: the round trip, and the negative space. A write stopped before any of its
 * steps, by a failure or by a crash, leaves the target holding exactly its old bytes, and a failure
 * leaves no temp file behind. Then two writers racing on one target, and a simulated run that mixes
 * writes, failures and crashes and checks both after every step.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

#if OS_LINUX
#include <sys/stat.h>
#include <unistd.h>
#endif

#define DIRECTORY "test_file_atomic_scratch"
#define TARGET    DIRECTORY "/target.nya"

/** The racing writers, and how often each rewrites the target. Enough for the temp names to interleave. */
#define WRITER_COUNT      4
#define WRITES_PER_WRITER 32

/** Payload bytes in the simulated run's writes, zero included, since an empty file is a file too. */
#define SIMULATION_CONTENT_MAX 512
#define SIMULATION_STEPS       160
#define SIMULATION_SEED_COUNT  4

static NYA_ConstCString STEP_NAMES[NYA_FILE_ATOMIC_STEP_COUNT] = {
  [NYA_FILE_ATOMIC_STEP_OPEN]    = "open",
  [NYA_FILE_ATOMIC_STEP_WRITE]   = "write",
  [NYA_FILE_ATOMIC_STEP_SYNC]    = "sync",
  [NYA_FILE_ATOMIC_STEP_REPLACE] = "replace",
};

static void directory_reset(void) {
  if (nya_filesystem_exists(DIRECTORY)) NYA_EXPECT(nya_filesystem_delete_recursive(DIRECTORY));
  NYA_EXPECT(nya_filesystem_create_directory(DIRECTORY));
}

/** Entries in the directory, and how many of them are temp files. */
static void directory_count(OUT u32* out_entries, OUT u32* out_temporaries) {
  NYA_Arena scratch = nya_arena_create_on_stack(.name = "directory_count");
  defer     nya_arena_destroy_on_stack(&scratch);

  NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
  NYA_EXPECT(nya_filesystem_list(&scratch, DIRECTORY, &entries));

  *out_entries     = 0;
  *out_temporaries = 0;
  nya_array_foreach (entries, entry) {
    (*out_entries)++;
    if (nya_string_ends_with(entry->name, ".tmp")) (*out_temporaries)++;
  }
}

static void delete_temporaries(void) {
  NYA_Arena scratch = nya_arena_create_on_stack(.name = "delete_temporaries");
  defer     nya_arena_destroy_on_stack(&scratch);

  NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
  NYA_EXPECT(nya_filesystem_list(&scratch, DIRECTORY, &entries));

  nya_array_foreach (entries, entry) {
    if (!nya_string_ends_with(entry->name, ".tmp")) continue;
    NYA_CString name = nya_string_to_cstring(&scratch, entry->name);
    NYA_EXPECT(nya_filesystem_delete(nya_string_to_cstring(&scratch, nya_path_join(&scratch, DIRECTORY, name))));
  }
}

static b8 target_holds(const NYA_String* expected) {
  NYA_Arena scratch = nya_arena_create_on_stack(.name = "target_holds");
  defer     nya_arena_destroy_on_stack(&scratch);

  NYA_String* actual = nya_string_create(&scratch);
  if (!nya_file_read(TARGET, actual).ok) return false;

  return actual->length == expected->length && (expected->length == 0 || memcmp(actual->items, expected->items, expected->length) == 0);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * RACING WRITERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

static NYA_ConstCString WRITER_PAYLOADS[WRITER_COUNT] = {
  "first writer, and nothing else in this file",
  "second writer: a longer line so a torn mix of two would show up in the length",
  "third",
  "fourth writer",
};

static s32 SDLCALL writer(void* data) {
  NYA_ConstCString payload = data;

  for (u32 i = 0; i < WRITES_PER_WRITER; i++) NYA_EXPECT(nya_file_write_atomic(TARGET, payload));

  return 0;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * SIMULATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct AtomicScenario AtomicScenario;

struct AtomicScenario {
  NYA_Arena* arena;

  /** What the target must hold: the last write that returned OK. */
  NYA_String* committed;
  b8          exists;

  /** Temp files a crash is allowed to have left behind. A failure is allowed none. */
  u32 stale_temporaries;
};

/** Writes fresh content drawn from the run, with `fault` armed at a drawn step first, or not at all. */
static void write_with(NYA_SimulationRun* run, NYA_FileAtomicFault fault) {
  AtomicScenario* scenario = run->user_data;

  NYA_String* content = nya_string_create(scenario->arena);
  u64         length  = nya_simulation_below(run, SIMULATION_CONTENT_MAX + 1);
  for (u64 i = 0; i < length; i++) nya_string_push_back(content, (u8)nya_simulation_roll(run));

  NYA_FileAtomicStep step = (NYA_FileAtomicStep)nya_simulation_below(run, NYA_FILE_ATOMIC_STEP_COUNT);
  if (fault != NYA_FILE_ATOMIC_FAULT_NONE) nya_file_write_atomic_fault_set(step, fault);

  NYA_Error written = nya_file_write_atomic(TARGET, content);

  if (fault == NYA_FILE_ATOMIC_FAULT_NONE) {
    if (!written.ok) nya_simulation_fail(run, "a write with nothing armed failed: %s", (NYA_ConstCString)written.message);
    scenario->committed = content;
    scenario->exists    = true;
    return;
  }

  if (written.ok) nya_simulation_fail(run, "a write armed to stop before %s succeeded", STEP_NAMES[step]);

  // a crash before the temp file was opened has nothing to leave behind.
  if (fault == NYA_FILE_ATOMIC_FAULT_CRASH && step != NYA_FILE_ATOMIC_STEP_OPEN) scenario->stale_temporaries++;
}

static void action_write(NYA_SimulationRun* run) {
  write_with(run, NYA_FILE_ATOMIC_FAULT_NONE);
}

static void fault_fail(NYA_SimulationRun* run) {
  write_with(run, NYA_FILE_ATOMIC_FAULT_FAIL);
}

static void fault_crash(NYA_SimulationRun* run) {
  write_with(run, NYA_FILE_ATOMIC_FAULT_CRASH);
}

static void check_target(NYA_SimulationRun* run) {
  AtomicScenario* scenario = run->user_data;

  if (!scenario->exists) {
    if (nya_filesystem_exists(TARGET)) nya_simulation_fail(run, "a target nothing committed exists");
    return;
  }

  if (!target_holds(scenario->committed)) nya_simulation_fail(run, "the target is not the last committed write");
}

static void check_directory(NYA_SimulationRun* run) {
  AtomicScenario* scenario = run->user_data;

  u32 entries     = 0;
  u32 temporaries = 0;
  directory_count(&entries, &temporaries);

  if (temporaries != scenario->stale_temporaries) {
    nya_simulation_fail(run, "%u temp files where crashes left %u", temporaries, scenario->stale_temporaries);
  }
  if (entries != temporaries + (scenario->exists ? 1U : 0U)) nya_simulation_fail(run, "something besides the target and temps appeared");
}

static u32 simulate(u64 seed) {
  directory_reset();

  AtomicScenario scenario = { .arena = nya_arena_create(.name = "test_file_atomic_simulation") };

  NYA_SimulationRun* run = nya_simulation_create(.seed = seed, .step_count = SIMULATION_STEPS, .user_data = &scenario);

  nya_simulation_action_add(run, "write", 60, action_write);
  nya_simulation_fault_add(run, "write fails", 20, fault_fail);
  nya_simulation_fault_add(run, "write crashes", 20, fault_crash);

  nya_simulation_check_add(run, "the target is old or new, never a mix", check_target);
  nya_simulation_check_add(run, "only crashes leave temp files", check_directory);

  u32 failures = nya_simulation_run(run);

  nya_simulation_destroy(run);
  nya_arena_destroy(scenario.arena);

  return failures;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TESTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(void) {
  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  NYA_Arena* arena = nya_arena_create(.name = "test_file_atomic");

  NYA_String* old_bytes = nya_string_from(arena, "old bytes, which must survive");
  NYA_String* new_bytes = nya_string_from(arena, "new bytes");

  u32 entries     = 0;
  u32 temporaries = 0;

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a new file, a rewrite, and an empty one, each leaving only the target
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_file_write_atomic round trip\n");
  {
    directory_reset();

    NYA_EXPECT(nya_file_write_atomic(TARGET, old_bytes));
    nya_assert(target_holds(old_bytes));

    NYA_EXPECT(nya_file_write_atomic(TARGET, "new bytes"));
    nya_assert(target_holds(new_bytes));

    NYA_EXPECT(nya_file_write_atomic(TARGET, ""));
    nya_assert(target_holds(nya_string_create(arena)));

    directory_count(&entries, &temporaries);
    nya_assert(entries == 1 && temporaries == 0, "%u entries, %u temps", entries, temporaries);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a failure before any step keeps the old bytes and removes the temp
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_file_write_atomic failing at every step\n");
  for (NYA_FileAtomicStep step = 0; step < NYA_FILE_ATOMIC_STEP_COUNT; step++) {
    directory_reset();
    NYA_EXPECT(nya_file_write_atomic(TARGET, old_bytes));

    nya_file_write_atomic_fault_set(step, NYA_FILE_ATOMIC_FAULT_FAIL);
    NYA_Error written = nya_file_write_atomic(TARGET, new_bytes);

    nya_assert(!written.ok, "armed before %s and still succeeded", STEP_NAMES[step]);
    nya_assert(target_holds(old_bytes), "failing before %s touched the target", STEP_NAMES[step]);

    directory_count(&entries, &temporaries);
    nya_assert(entries == 1 && temporaries == 0, "failing before %s left %u temps", STEP_NAMES[step], temporaries);

    // disarmed as it fired, so the next write goes through.
    NYA_EXPECT(nya_file_write_atomic(TARGET, new_bytes));
    nya_assert(target_holds(new_bytes));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a failure on a first write leaves no target at all, not an empty one
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_file_write_atomic failing a first write\n");
  for (NYA_FileAtomicStep step = 0; step < NYA_FILE_ATOMIC_STEP_COUNT; step++) {
    directory_reset();

    nya_file_write_atomic_fault_set(step, NYA_FILE_ATOMIC_FAULT_FAIL);
    nya_assert(!nya_file_write_atomic(TARGET, new_bytes).ok);

    directory_count(&entries, &temporaries);
    nya_assert(entries == 0, "failing a first write before %s left %u entries", STEP_NAMES[step], entries);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a crash before any step keeps the old bytes, and the next write works
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_file_write_atomic crashing at every step\n");
  for (NYA_FileAtomicStep step = 0; step < NYA_FILE_ATOMIC_STEP_COUNT; step++) {
    directory_reset();
    NYA_EXPECT(nya_file_write_atomic(TARGET, old_bytes));

    nya_file_write_atomic_fault_set(step, NYA_FILE_ATOMIC_FAULT_CRASH);
    nya_assert(!nya_file_write_atomic(TARGET, new_bytes).ok);
    nya_assert(target_holds(old_bytes), "crashing before %s touched the target", STEP_NAMES[step]);

    // a crash cleans nothing up, so the temp it had open is still there.
    u32 expected = step == NYA_FILE_ATOMIC_STEP_OPEN ? 0 : 1;
    directory_count(&entries, &temporaries);
    nya_assert(temporaries == expected, "crashing before %s left %u temps, expected %u", STEP_NAMES[step], temporaries, expected);

    // the stale temp does not get in the way of the next writer.
    NYA_EXPECT(nya_file_write_atomic(TARGET, new_bytes));
    nya_assert(target_holds(new_bytes));

    delete_temporaries();
  }

#if OS_LINUX
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the target's permissions and a symlink to it both survive a write
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_file_write_atomic keeps permissions and symlinks\n");
  {
    directory_reset();
    NYA_EXPECT(nya_file_write_atomic(TARGET, old_bytes));
    nya_assert(chmod(TARGET, 0o600) == 0);

    NYA_EXPECT(nya_file_write_atomic(TARGET, new_bytes));

    struct stat target_stat;
    nya_assert(stat(TARGET, &target_stat) == 0);
    nya_assert((target_stat.st_mode & 0o7777) == 0o600, "mode became %o", target_stat.st_mode & 0o7777);

    nya_assert(symlink("target.nya", DIRECTORY "/link.nya") == 0);
    NYA_EXPECT(nya_file_write_atomic(DIRECTORY "/link.nya", old_bytes));

    NYA_FileInfo info = { 0 };
    NYA_EXPECT(nya_filesystem_info(DIRECTORY "/link.nya", &info));
    nya_assert(info.type == NYA_FILE_TYPE_SYMLINK, "the link was replaced by a file");
    nya_assert(target_holds(old_bytes));
  }
#endif

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: writers racing on one target never share a temp name or tear a file
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_file_write_atomic with racing writers\n");
  {
    directory_reset();

    SDL_Thread* threads[WRITER_COUNT];
    for (u32 i = 0; i < WRITER_COUNT; i++) {
      threads[i] = SDL_CreateThread(writer, "atomic_writer", (void*)WRITER_PAYLOADS[i]);
      nya_assert(threads[i] != nullptr, "SDL_CreateThread failed: %s", SDL_GetError());
    }
    for (u32 i = 0; i < WRITER_COUNT; i++) SDL_WaitThread(threads[i], nullptr);

    b8 whole = false;
    for (u32 i = 0; i < WRITER_COUNT; i++) whole = whole || target_holds(nya_string_from(arena, WRITER_PAYLOADS[i]));
    nya_assert(whole, "the target is none of the payloads whole");

    directory_count(&entries, &temporaries);
    nya_assert(entries == 1 && temporaries == 0, "%u entries, %u temps", entries, temporaries);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a simulated run of writes, failures and crashes in any order
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: nya_file_write_atomic under simulation\n");
  for (u64 seed = 1; seed <= SIMULATION_SEED_COUNT; seed++) {
    u32 failures = simulate(seed);
    nya_assert(failures == 0, "seed " FMTu64 " failed %u times", seed, failures);
  }

  NYA_EXPECT(nya_filesystem_delete_recursive(DIRECTORY));
  nya_arena_destroy(arena);
  SDL_Quit();

  printf("All nya_file_write_atomic tests passed.\n");
  return 0;
}
