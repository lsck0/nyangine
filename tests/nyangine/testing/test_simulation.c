/**
 * Deterministic simulation over the engine: random sequences of atomic actions and injected faults,
 * with the engine's assertions and a handful of invariants as the oracle.
 *
 * Run with no arguments it replays the committed regression seeds, which is what `./build run test`
 * does. Run with `--seed` it takes that one seed, which is what `./build run simulation` does and what
 * a failure report tells you to paste.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "generated/reflection.h"

#include "SDL3/SDL_init.h"

/** Where the seeds that have ever failed live. Relative to the repository root, which tests run from. */
#define SEED_FILE "tests/nyangine/testing/simulation_seeds.txt"

/** Steps a regression seed is replayed for. Long enough to reach a deep world, short enough to run 187 tests. */
#define REGRESSION_STEPS 4000

/** Steps a seed given on the command line takes when none is asked for. */
#define DEFAULT_STEPS 20000

/** Regression seeds the file may hold. A line is eight bytes of seed; a thousand is more than anyone will write. */
#define SEED_MAX 1000

static u32 run_seed(u64 seed, u64 steps, b8 verbose) {
    NYA_SimulationRun* run = nya_simulation_create(.seed = seed, .step_count = steps, .verbose = verbose);
    defer              nya_simulation_destroy(run);

    nya_simulation_actions_add(run);
    defer nya_simulation_actions_remove();

    // the generated table, which only something above the engine can name; see testing_actions.h.
    nya_simulation_actions_reflect_add(run, NYA_REFLECT_TYPES, NYA_REFLECT_TYPE_COUNT);

    return nya_simulation_run(run);
}

/** Reads the committed seeds. Returns how many were read; a missing file is a failure, not an empty list. */
static u32 read_seeds(NYA_Arena* arena, OUT u64* out, u32 capacity) {
    NYA_String* text = nya_string_create(arena);
    NYA_EXPECT(nya_file_read(SEED_FILE, text), "while reading " SEED_FILE);

    NYA_ArrayᐸNYA_Stringᐳ* lines = nya_string_split_lines(arena, text);

    u32 count = 0;

    nya_array_foreach (lines, line) {
        nya_string_trim_whitespace(line);

        if (nya_string_is_empty(line)) continue;
        if (nya_string_starts_with(line, "#")) continue;

        nya_assert(count < capacity, "more than %u seeds in " SEED_FILE, capacity);

        // strtoull stops at the "#" a comment starts with, so the reason on the line costs no parsing.
        out[count++] = strtoull(nya_string_to_cstring(arena, line), nullptr, 0);
    }

    return count;
}

s32 main(s32 argc, NYA_CString argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    b8  have_seed = false;
    u64 seed      = 0;
    u64 steps     = DEFAULT_STEPS;
    b8  verbose   = false;

    for (s32 i = 1; i < argc; i++) {
        if (nya_string_equals(argv[i], "--verbose")) {
            verbose = true;
        } else if (nya_string_equals(argv[i], "--seed") && i + 1 < argc) {
            seed      = strtoull(argv[++i], nullptr, 0);
            have_seed = true;
        } else if (nya_string_equals(argv[i], "--steps") && i + 1 < argc) {
            steps = strtoull(argv[++i], nullptr, 0);
        } else {
            // a bad argument prints the usage and exits non-zero rather than running something the
            // caller did not ask for.
            (void)fprintf(stderr, "Error: unexpected argument '%s'\n\n", argv[i]);
            (void)fprintf(stderr, "Usage: test_simulation [--seed <n>] [--steps <n>] [--verbose]\n");
            return EXIT_FAILURE;
        }
    }

    nya_assert(steps > 0, "a run of zero steps proves nothing");

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    b8 sdl_ok = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    u32 failures = 0;

    if (have_seed) {
        failures += run_seed(seed, steps, verbose);
    } else {
        NYA_Arena* arena = nya_arena_create(.name = "test_simulation");
        defer      nya_arena_destroy(arena);

        u64 seeds[SEED_MAX];
        u32 count = read_seeds(arena, seeds, SEED_MAX);

        nya_assert(count > 0, SEED_FILE " holds no seeds; the regression list must never be empty");

        printf("TEST: %u committed regression seeds, %d steps each\n", count, REGRESSION_STEPS);

        for (u32 i = 0; i < count; i++) failures += run_seed(seeds[i], REGRESSION_STEPS, verbose);
    }

    if (failures > 0) {
        printf("FAILED: test_simulation (%u failures)\n", failures);
        return EXIT_FAILURE;
    }

    printf("PASSED: test_simulation (0 failures)\n");

    return EXIT_SUCCESS;
}
