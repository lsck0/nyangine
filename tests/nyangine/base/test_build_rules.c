/**
 * The build rule engine: policies, metarules, hooks, dependencies and failure.
 *
 * test_build.c covers the flag macros; nothing covered nya_build itself, which is why base_build.c
 * sat at zero branch coverage while being the thing every target in the tree goes through.
 *
 * Everything here runs `true` or `false` rather than a compiler. What is being tested is *whether a
 * rule runs and how often*, not what its command produces, so the cheapest possible command is the
 * honest one — and a hook counting invocations is a more direct answer than inspecting artifacts.
 *
 * Files are created and stamped by hand because the outdated policy is defined in terms of
 * modification times, and the only way to test "input newer than output" is to make it so.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <utime.h>

/*
 * ─────────────────────────────────────────────────────────
 * FIXTURES
 * ─────────────────────────────────────────────────────────
 */

#define WORK_DIRECTORY "./_test_build_rules"
#define INPUT_PATH     WORK_DIRECTORY "/input.txt"
#define OUTPUT_PATH    WORK_DIRECTORY "/output.txt"

/** How many times each hook kind has fired, so "did it run" is a number rather than a guess. */
static u32 pre_hook_calls  = 0;
static u32 post_hook_calls = 0;

/** The order hooks fired in, so pre-before-post is checked rather than assumed. */
static u32 hook_sequence[8] = { 0 };
static u32 hook_sequence_length = 0;

static void record_pre(NYA_BuildRule* rule) {
  nya_unused(rule);
  pre_hook_calls++;
  if (hook_sequence_length < 8) hook_sequence[hook_sequence_length++] = 1;
}

static void record_post(NYA_BuildRule* rule) {
  nya_unused(rule);
  post_hook_calls++;
  if (hook_sequence_length < 8) hook_sequence[hook_sequence_length++] = 2;
}

static void reset_hooks(void) {
  pre_hook_calls       = 0;
  post_hook_calls      = 0;
  hook_sequence_length = 0;
}

/** Writes `text` to `path`, creating it. */
static void write_file(NYA_ConstCString path, NYA_ConstCString text) {
  FILE* file = fopen(path, "wb");
  nya_assert(file != nullptr, "could not create the fixture at %s", path);
  (void)fputs(text, file);
  (void)fclose(file);
}

/** Forces a file's modification time, so "newer" and "older" are decided rather than raced. */
static void stamp_file(NYA_ConstCString path, s64 seconds_from_now) {
  struct utimbuf times = { 0 };
  times.actime         = (time_t)(time(nullptr) + seconds_from_now);
  times.modtime        = times.actime;

  nya_assert(utime(path, &times) == 0, "could not stamp %s", path);
}

s32 main(void) {
  // A real directory, because every policy below is defined against the filesystem.
  (void)nya_filesystem_delete_recursive(WORK_DIRECTORY);
  NYA_EXPECT(nya_filesystem_create_directory(WORK_DIRECTORY));
  defer (void)nya_filesystem_delete_recursive(WORK_DIRECTORY);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_BUILD_ALWAYS runs every single time
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset_hooks();

    NYA_BuildRule always = {
      .name    = "test_always",
      .policy  = NYA_BUILD_ALWAYS,
      .command = { .program = "true" },

      .pre_build_hooks  = { &record_pre },
      .post_build_hooks = { &record_post },
    };

    // Three separate builds. The epoch bookkeeping that stops a rule running twice within *one*
    // build must not turn into "runs once ever" across separate ones.
    for (u32 i = 0; i < 3; i++) NYA_EXPECT(nya_build(&always));

    nya_assert(pre_hook_calls == 3, "expected three runs, got " FMTu32, pre_hook_calls);
    nya_assert(post_hook_calls == 3, "expected three runs, got " FMTu32, post_hook_calls);

    // Pre before post, every time, rather than both merely having happened.
    nya_assert(hook_sequence_length == 6);
    for (u32 i = 0; i < 6; i += 2) {
      nya_assert(hook_sequence[i] == 1 && hook_sequence[i + 1] == 2, "hooks fired out of order at %u", i);
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_BUILD_ONCE is keyed on the output existing
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset_hooks();
    (void)nya_filesystem_delete(OUTPUT_PATH);

    NYA_BuildRule once = {
      .name        = "test_once",
      .policy      = NYA_BUILD_ONCE,
      .output_file = OUTPUT_PATH,
      .command     = { .program = "true" },

      .pre_build_hooks = { &record_pre },
    };

    // Missing output: runs.
    NYA_EXPECT(nya_build(&once));
    nya_assert(pre_hook_calls == 1, "a missing output must build, got " FMTu32, pre_hook_calls);

    // The command was `true`, so nothing created the output — it still does not exist, and the rule
    // must therefore run again. "Once" means once the artifact is there, not once per process.
    NYA_EXPECT(nya_build(&once));
    nya_assert(pre_hook_calls == 2, "an output that was never produced must build again");

    // Now it exists.
    write_file(OUTPUT_PATH, "built");
    NYA_EXPECT(nya_build(&once));
    NYA_EXPECT(nya_build(&once));
    nya_assert(pre_hook_calls == 2, "an existing output must skip, got " FMTu32, pre_hook_calls);

    /*
     * A directory counts as an existing output.
     *
     * Not incidental: every vendor's build-directory rule is exactly this shape, and the sqlean and
     * sqlvec ones were missing their policy entirely, so they re-ran their mkdir on every single
     * ./build. This is the behaviour that fix relies on.
     */
    reset_hooks();

    NYA_BuildRule directory = {
      .name        = "test_once_directory",
      .policy      = NYA_BUILD_ONCE,
      .output_file = WORK_DIRECTORY,
      .command     = { .program = "true" },

      .pre_build_hooks = { &record_pre },
    };

    NYA_EXPECT(nya_build(&directory));
    nya_assert(pre_hook_calls == 0, "an existing directory must satisfy NYA_BUILD_ONCE");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_BUILD_IF_OUTDATED compares modification times
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset_hooks();
    (void)nya_filesystem_delete(OUTPUT_PATH);

    write_file(INPUT_PATH, "source");

    NYA_BuildRule outdated = {
      .name        = "test_outdated",
      .policy      = NYA_BUILD_IF_OUTDATED,
      .input_file  = INPUT_PATH,
      .output_file = OUTPUT_PATH,
      .command     = { .program = "true" },

      .pre_build_hooks = { &record_pre },
    };

    // No output at all: build, without consulting any timestamp.
    NYA_EXPECT(nya_build(&outdated));
    nya_assert(pre_hook_calls == 1, "a missing output must build");

    // Output newer than input: skip.
    write_file(OUTPUT_PATH, "artifact");
    stamp_file(INPUT_PATH, -100);
    stamp_file(OUTPUT_PATH, -10);

    NYA_EXPECT(nya_build(&outdated));
    nya_assert(pre_hook_calls == 1, "a fresh output must skip, got " FMTu32, pre_hook_calls);

    // Input newer than output: build. The whole point of the policy.
    stamp_file(INPUT_PATH, -5);
    NYA_EXPECT(nya_build(&outdated));
    nya_assert(pre_hook_calls == 2, "a touched input must rebuild, got " FMTu32, pre_hook_calls);

    /*
     * Equal timestamps must count as up to date rather than as outdated.
     *
     * A filesystem with coarse timestamps gives an artifact the same second as its source all the
     * time, and treating that as stale means a build that never reaches a fixed point — every run
     * rebuilds everything, forever, on that machine only.
     */
    stamp_file(INPUT_PATH, -50);
    stamp_file(OUTPUT_PATH, -50);

    NYA_EXPECT(nya_build(&outdated));
    nya_assert(pre_hook_calls == 2, "equal timestamps must count as up to date, got " FMTu32, pre_hook_calls);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a metarule runs no command but still obeys its policy
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * A metarule short circuits before the command is ever spawned, which is what makes it safe to
     * leave `command` empty. What it does *not* skip is the policy — that is decided one level up,
     * before the rule is run at all.
     *
     * Worth pinning precisely, because leaving the policy off a metarule silently means
     * NYA_BUILD_ALWAYS: it is the enum's zero value, and the symptom is a rule quietly re-running
     * its hooks on every build rather than anything failing.
     */
    reset_hooks();

    NYA_BuildRule meta_always = {
      .name        = "test_meta_always",
      .is_metarule = true,
      // No policy, so NYA_BUILD_ALWAYS, and no output_file to key on either.
      .command     = { .program = "false" },

      .pre_build_hooks  = { &record_pre },
      .post_build_hooks = { &record_post },
    };

    // `false` would fail if it were spawned. That it succeeds is the proof no command ran.
    NYA_EXPECT(nya_build(&meta_always));
    NYA_EXPECT(nya_build(&meta_always));

    nya_assert(pre_hook_calls == 2, "a policy-less metarule runs every time, got " FMTu32, pre_hook_calls);
    nya_assert(post_hook_calls == 2, "and its post hooks too, got " FMTu32, post_hook_calls);

    // The same metarule with a policy it satisfies must not run at all.
    reset_hooks();

    NYA_BuildRule meta_once = {
      .name        = "test_meta_once",
      .policy      = NYA_BUILD_ONCE,
      .is_metarule = true,
      .output_file = WORK_DIRECTORY,
      .command     = { .program = "false" },

      .pre_build_hooks = { &record_pre },
    };

    NYA_EXPECT(nya_build(&meta_once));
    nya_assert(pre_hook_calls == 0, "a satisfied metarule must not run its hooks, got " FMTu32, pre_hook_calls);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a failing command is reported rather than swallowed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    reset_hooks();

    NYA_BuildRule failing = {
      .name    = "test_failing",
      .policy  = NYA_BUILD_ALWAYS,
      .command = { .program = "false" },

      .pre_build_hooks = { &record_pre },
    };

    NYA_Error result = nya_build(&failing);
    nya_assert(!result.ok, "a non-zero exit must surface as an error");

    // The rule was still entered, so the failure is the command's rather than the dispatch refusing
    // to run it.
    nya_assert(pre_hook_calls == 1, "the pre hook must have fired before the command");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a shared dependency is built once per build, not once per path
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * A dependency graph is a graph. Two rules that both need the same one must not build it twice
     * within a single nya_build, which is what last_built_epoch is for — and it is why
     * `./build build release` once compiled the shaders twice.
     */
    reset_hooks();

    NYA_BuildRule shared = {
      .name    = "test_shared",
      .policy  = NYA_BUILD_ALWAYS,
      .command = { .program = "true" },

      .pre_build_hooks = { &record_pre },
    };

    NYA_BuildRule left  = { .name = "test_left", .policy = NYA_BUILD_ALWAYS, .command = { .program = "true" }, .dependencies = { &shared } };
    NYA_BuildRule right = { .name = "test_right", .policy = NYA_BUILD_ALWAYS, .command = { .program = "true" }, .dependencies = { &shared } };

    NYA_BuildRule top = {
      .name         = "test_top",
      .policy       = NYA_BUILD_ALWAYS,
      .command      = { .program = "true" },
      .dependencies = { &left, &right },
    };

    NYA_EXPECT(nya_build(&top));
    nya_assert(pre_hook_calls == 1, "a diamond dependency must build once, got " FMTu32, pre_hook_calls);

    // A second, separate build is a new epoch, so it runs again.
    NYA_EXPECT(nya_build(&top));
    nya_assert(pre_hook_calls == 2, "a later build must run it again, got " FMTu32, pre_hook_calls);
  }

  printf("PASSED: test_build_rules\n");
  return 0;
}
