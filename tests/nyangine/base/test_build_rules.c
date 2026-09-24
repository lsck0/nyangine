/**
 * The build rule engine: policies, metarules, hooks, dependencies and failure.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <utime.h>

/* FIXTURES */

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

  // TEST: NYA_BUILD_ALWAYS runs every single time
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

  // TEST: NYA_BUILD_ONCE is keyed on the output existing
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

    // the command was `true`, so the output still does not exist and the rule must run again. Once means
    // once the artifact exists, not once per process.
    NYA_EXPECT(nya_build(&once));
    nya_assert(pre_hook_calls == 2, "an output that was never produced must build again");

    // Now it exists.
    write_file(OUTPUT_PATH, "built");
    NYA_EXPECT(nya_build(&once));
    NYA_EXPECT(nya_build(&once));
    nya_assert(pre_hook_calls == 2, "an existing output must skip, got " FMTu32, pre_hook_calls);

    /* A directory counts as an existing output. */
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

  // TEST: NYA_BUILD_IF_OUTDATED compares modification times
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

    /* Equal timestamps must count as up to date rather than as outdated. */
    stamp_file(INPUT_PATH, -50);
    stamp_file(OUTPUT_PATH, -50);

    NYA_EXPECT(nya_build(&outdated));
    nya_assert(pre_hook_calls == 2, "equal timestamps must count as up to date, got " FMTu32, pre_hook_calls);
  }

  // TEST: a metarule runs no command but still obeys its policy
  {
    /*
     * A metarule short circuits before spawning, so `command` may be empty. The policy is still decided
     * one level up, before the rule runs.
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

  // TEST: a failing command is reported rather than swallowed
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
    nya_assert(nya_build_last_failure() == &failing, "and the failure names the rule whose command failed");

    // the next build starts clean, so a success does not report the failure before it.
    NYA_BuildRule passing = { .name = "test_passing_after_failure", .policy = NYA_BUILD_ALWAYS, .command = { .program = "true" } };
    NYA_EXPECT(nya_build(&passing));
    nya_assert(nya_build_last_failure() == nullptr, "a build that succeeded reports no failure");

    // The rule was still entered, so the failure is the command's rather than the dispatch refusing
    // to run it.
    nya_assert(pre_hook_calls == 1, "the pre hook must have fired before the command");
  }

  // TEST: a shared dependency is built once per build, not once per path
  {
    /*
     * Two rules sharing a dependency must not build it twice in one nya_build; last_built_epoch
     * guarantees that.
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

  // TEST: the parallel pool refills a slot as soon as its rule finishes
  {
    reset_hooks();

    // Two slots. Batches would hold the fast rules behind the slow one; a pool runs them all beside it,
    // so the slow rule finishes last.
    NYA_BuildRule slow = {
      .name    = "test_parallel_slow",
      .policy  = NYA_BUILD_ALWAYS,
      .command = { .program = "sleep", .arguments = { "1" } },

      .pre_build_hooks  = { &record_pre },
      .post_build_hooks = { &record_post },
    };

    NYA_BuildRule fast[3];
    for (u32 i = 0; i < 3; i++) {
      fast[i] = (NYA_BuildRule){
        .name    = "test_parallel_fast",
        .policy  = NYA_BUILD_ALWAYS,
        .command = { .program = "true" },

        .post_build_hooks = { &record_pre },
      };
    }

    NYA_BuildRule* rules[] = { &slow, &fast[0], &fast[1], &fast[2] };
    NYA_EXPECT(nya_build_parallel(rules, 4, 2));

    // record_pre marks a fast rule finishing and record_post the slow one, after its own pre hook.
    nya_assert(hook_sequence_length == 5, "expected five hook calls, got " FMTu32, hook_sequence_length);
    nya_assert(hook_sequence[4] == 2, "the slow rule must finish after every fast one");
    for (u32 i = 0; i < 4; i++) nya_assert(!rules[i]->parallel_is_running);
  }

  // TEST: the parallel pool keeps reading output larger than a pipe buffer
  {
    // Blocked on a full pipe the child never exits, so this hangs unless the pool drains while it polls.
    NYA_BuildRule chatty = {
      .name    = "test_parallel_chatty",
      .policy  = NYA_BUILD_ALWAYS,
      .command = { .program = "head", .arguments = { "-c", "300000", "/dev/zero" } },
    };
    NYA_BuildRule slow = {
      .name    = "test_parallel_chatty_neighbour",
      .policy  = NYA_BUILD_ALWAYS,
      .command = { .program = "sleep", .arguments = { "0.2" } },
    };

    NYA_BuildRule* rules[] = { &slow, &chatty };
    NYA_EXPECT(nya_build_parallel(rules, 2, 2));

    nya_assert(chatty.command.stdout_content != nullptr);
    nya_assert(chatty.command.stdout_content->length == 300000, "captured " FMTu64 " bytes", chatty.command.stdout_content->length);
  }

  // TEST: a failing parallel rule fails the call and stops starting new rules
  {
    reset_hooks();

    NYA_BuildRule failing = {
      .name    = "test_parallel_failing",
      .policy  = NYA_BUILD_ALWAYS,
      .command = { .program = "false" },
    };
    NYA_BuildRule running = {
      .name    = "test_parallel_running",
      .policy  = NYA_BUILD_ALWAYS,
      .command = { .program = "sleep", .arguments = { "0.2" } },

      .post_build_hooks = { &record_post },
    };
    NYA_BuildRule never = {
      .name    = "test_parallel_never",
      .policy  = NYA_BUILD_ALWAYS,
      .command = { .program = "true" },

      .pre_build_hooks = { &record_pre },
    };

    NYA_BuildRule* rules[] = { &failing, &running, &never };
    NYA_Error      result  = nya_build_parallel(rules, 3, 2);

    nya_assert(!result.ok, "a failing rule must fail the parallel build");

    // prepared like every rule, but never started once the failure was seen, while the rule already
    // running was still reaped and finished.
    nya_assert(pre_hook_calls == 1, "every rule is prepared before any starts");
    nya_assert(post_hook_calls == 1, "a rule already running when another fails still finishes");
    nya_assert(never.last_built_epoch != running.last_built_epoch, "a rule after the failure must not have been built");
    for (u32 i = 0; i < 3; i++) nya_assert(!rules[i]->parallel_is_running);
  }

  printf("PASSED: test_build_rules\n");
  return 0;
}
