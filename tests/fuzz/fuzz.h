/**
 * @file fuzz.h
 *
 * What every fuzz target under tests/fuzz shares: one entry point that is an AFL++ persistent harness
 * when AFL built it, a replay of the committed corpus and every kept crash when it is run with no
 * arguments, and a single input when it is handed a file.
 *
 * A target is one file:
 *
 * ```c
 * #include "nyangine/nyangine.c"
 * #include "nyangine/nyangine.h"
 *
 * #define FUZZ_TARGET "serde_json"
 *
 * static void fuzz_once(const u8* data, u64 size) {
 *     NYA_Arena* arena = nya_arena_create(.name = "fuzz");
 *     defer      nya_arena_destroy(arena);
 *
 *     NYA_Object* object = nullptr;
 *     (void)nya_deserialize(arena, data, size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &object);
 * }
 *
 * #include "tests/fuzz/fuzz.h"
 * ```
 *
 * ## Why the same binary is a test
 *
 * "Every crash kept as a regression case" only means anything if something replays them. Run with no
 * arguments the target walks tests/fuzz/corpus/<target> and tests/fuzz/crashes/<target> and feeds
 * every file through fuzz_once, so `./build run test` fails the day a fixed crash comes back. That is
 * also why these live under tests/: the test runner finds them there and needs no list.
 *
 * ## The oracle
 *
 * There is none beyond the ones already in the process. The sanitizers catch the reads, the writes,
 * the overflows and the leaks, and the engine's own assertions catch a broken invariant. A parser
 * returning an error for hostile input is the expected answer and not a finding; a parser that
 * asserts on it is the finding, because an assert an attacker can reach is a denial of service.
 *
 * ## AFL is optional
 *
 * Nothing here needs afl-clang-fast to compile. Without it __AFL_FUZZ_TESTCASE_BUF is undefined and
 * the target is an ordinary replay binary, which is what CI runs when AFL++ is not installed. See
 * `./build run fuzz`, which probes for the toolchain and says what is missing rather than failing
 * three steps in.
 * */
#pragma once

#ifndef FUZZ_TARGET
#error "a fuzz target must define FUZZ_TARGET before including fuzz.h"
#endif

/** Where the committed inputs for a target live. */
#define FUZZ_CORPUS_DIRECTORY "./tests/fuzz/corpus/" FUZZ_TARGET

/**
 * Where an input that once crashed is kept forever. Replayed on every run, so a regression is a test
 * failure rather than something noticed the next time the fuzzer happens to rediscover it.
 * */
#define FUZZ_CRASHES_DIRECTORY "./tests/fuzz/crashes/" FUZZ_TARGET

/**
 * Iterations one persistent AFL process runs before it is restarted.
 *
 * Persistent mode is one to two orders of magnitude faster than forking per input, and the restart is
 * what stops a slow leak in a target from being reported as a leak in the code under test.
 * */
#ifndef FUZZ_PERSISTENT_ITERATIONS
#define FUZZ_PERSISTENT_ITERATIONS 10000
#endif

#ifdef __AFL_HAVE_MANUAL_CONTROL
__AFL_FUZZ_INIT();
#endif

/**
 * Feeds one file through the target. Missing or unreadable files are the walk's problem, not this one.
 *
 * Marked unused because under AFL it is: __AFL_INIT takes over main and the replay path is compiled
 * and never called, which -Werror,-Wunused-function otherwise makes a build failure of. That is the
 * whole reason `./build run fuzz` could not build a target.
 * */
__attr_allow_unused NYA_INTERNAL b8 _fuzz_replay_file(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    u32* count = (u32*)user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;

    NYA_Arena* arena = nya_arena_create(.name = "fuzz_replay");
    defer      nya_arena_destroy(arena);

    NYA_String* content = nya_string_create(arena);

    // an unreadable file is an operating error; the walk carries on and the count says how many ran.
    if (!nya_file_read(path, content).ok) {
        nya_log_warn("Could not read '%s'.", path);
        return true;
    }

    fuzz_once((const u8*)content->items, content->length);

    (*count)++;
    return true;
}

/** Replays every file under `directory`. A directory that does not exist yet is not a failure. Unused under AFL; see above. */
__attr_allow_unused NYA_INTERNAL u32 _fuzz_replay_directory(NYA_ConstCString directory) {
    if (!nya_filesystem_exists(directory)) return 0;

    u32 count = 0;

    NYA_Arena* arena = nya_arena_create(.name = "fuzz_walk");
    defer      nya_arena_destroy(arena);

    NYA_EXPECT(nya_filesystem_walk(arena, directory, _fuzz_replay_file, &count), "while walking '%s'", directory);

    return count;
}

s32 main(s32 argc, NYA_CString argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);

#ifdef FUZZ_SETUP
    FUZZ_SETUP();
#endif

#ifdef __AFL_HAVE_MANUAL_CONTROL
    // AFL feeds the input through its own buffer, so the command line is unused under it.
    nya_unused(argc, argv);

    /*
     * Persistent mode: AFL restarts this loop with a new input instead of forking a process per case.
     * __AFL_INIT has to come after everything one-time, since the fork server snapshots the process
     * here and every later iteration starts from this point.
     */
    __AFL_INIT();

    u8* input = __AFL_FUZZ_TESTCASE_BUF;

    while (__AFL_LOOP(FUZZ_PERSISTENT_ITERATIONS)) {
        u64 size = (u64)__AFL_FUZZ_TESTCASE_LEN;

        fuzz_once(input, size);
    }

    return EXIT_SUCCESS;
#else
    /* A file per argument: what a triage run does with a crash AFL found. */
    if (argc > 1) {
        for (s32 i = 1; i < argc; i++) {
            NYA_Arena* arena = nya_arena_create(.name = "fuzz_input");
            defer      nya_arena_destroy(arena);

            NYA_String* content = nya_string_create(arena);
            NYA_EXPECT(nya_file_read(argv[i], content), "while reading '%s'", argv[i]);

            fuzz_once((const u8*)content->items, content->length);
        }

        printf("PASSED: fuzz_" FUZZ_TARGET " (%d inputs, 0 failures)\n", argc - 1);
        return EXIT_SUCCESS;
    }

    /*
     * No arguments: the regression run. Every committed input and every kept crash, which is what
     * makes `./build run test` the thing that notices a fixed bug coming back.
     */
    u32 corpus  = _fuzz_replay_directory(FUZZ_CORPUS_DIRECTORY);
    u32 crashes = _fuzz_replay_directory(FUZZ_CRASHES_DIRECTORY);

    nya_assert(corpus > 0, "fuzz_" FUZZ_TARGET " has an empty corpus; " FUZZ_CORPUS_DIRECTORY " must hold at least one input");

    printf("TEST: %u corpus inputs and %u kept crashes replayed\n", corpus, crashes);
    printf("PASSED: fuzz_" FUZZ_TARGET " (0 failures)\n");

    return EXIT_SUCCESS;
#endif
}
