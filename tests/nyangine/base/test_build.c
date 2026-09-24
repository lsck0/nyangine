/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

// Pre-build hook for testing
static b8   pre_build_called = false;
static void test_pre_build_hook(NYA_BuildRule* rule) {
  pre_build_called = true;
  nya_assert(rule != nullptr);
  nya_assert(nya_string_equals(rule->name, "Test Rule") == true);
}

// Post-build hook for testing
static b8   post_build_called = false;
static void test_post_build_hook(NYA_BuildRule* rule) {
  post_build_called = true;
  nya_assert(rule != nullptr);
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_build");

  // TEST: NYA_BuildRule struct initialization
  {
    NYA_BuildRule rule = {
      .name    = "Test Rule",
      .policy  = NYA_BUILD_ALWAYS,
      .command = {
        .program   = "echo",
        .arguments = { "hello", nullptr },
      },
    };

    nya_assert(nya_string_equals(rule.name, "Test Rule") == true);
    nya_assert(rule.policy == NYA_BUILD_ALWAYS);
    nya_assert(rule.is_metarule == false);
    nya_assert(nya_string_equals(rule.command.program, "echo") == true);
    nya_assert(nya_string_equals(rule.command.arguments[0], "hello") == true);
  }

  // TEST: NYA_BuildRule with hooks
  {
    pre_build_called  = false;
    post_build_called = false;

    NYA_BuildRule rule = {
      .name   = "Test Rule",
      .policy = NYA_BUILD_ALWAYS,
      .command = {
        .program   = "echo",
        .arguments = { "test", nullptr },
      },
    };

    // Set up hooks
    rule.pre_build_hooks[0]  = test_pre_build_hook;
    rule.post_build_hooks[0] = test_post_build_hook;

    nya_assert(rule.pre_build_hooks[0] != nullptr);
    nya_assert(rule.post_build_hooks[0] != nullptr);
  }

  // TEST: NYA_BuildRule with dependencies
  {
    NYA_BuildRule dep1 = {
      .name    = "Dependency 1",
      .policy  = NYA_BUILD_ALWAYS,
      .command = {
        .program   = "echo",
        .arguments = { "dep1", nullptr },
      },
    };

    NYA_BuildRule dep2 = {
      .name    = "Dependency 2",
      .policy  = NYA_BUILD_ALWAYS,
      .command = {
        .program   = "echo",
        .arguments = { "dep2", nullptr },
      },
    };

    NYA_BuildRule main_rule = {
      .name    = "Main Rule",
      .policy  = NYA_BUILD_ALWAYS,
      .command = {
        .program   = "echo",
        .arguments = { "main", nullptr },
      },
    };

    main_rule.dependencies[0] = &dep1;
    main_rule.dependencies[1] = &dep2;

    nya_assert(main_rule.dependencies[0] == &dep1);
    nya_assert(main_rule.dependencies[1] == &dep2);
    nya_assert(main_rule.dependencies[2] == nullptr);
  }

  // TEST: NYA_BuildRule as metarule
  {
    NYA_BuildRule metarule = {
      .name        = "Meta Rule",
      .policy      = NYA_BUILD_ALWAYS,
      .is_metarule = true,
    };

    nya_assert(metarule.is_metarule == true);
    nya_assert(metarule.command.program == nullptr);
  }

  // TEST: Different build policies
  {
    NYA_BuildRule rule_always = {
      .name   = "Always Build",
      .policy = NYA_BUILD_ALWAYS,
    };
    nya_assert(rule_always.policy == NYA_BUILD_ALWAYS);

    NYA_BuildRule rule_once = {
      .name        = "Build Once",
      .policy      = NYA_BUILD_ONCE,
      .output_file = "/tmp/test_output.txt",
    };
    nya_assert(rule_once.policy == NYA_BUILD_ONCE);
    nya_assert(nya_string_equals(rule_once.output_file, "/tmp/test_output.txt") == true);

    NYA_BuildRule rule_outdated = {
      .name        = "Build If Outdated",
      .policy      = NYA_BUILD_IF_OUTDATED,
      .input_file  = "/tmp/test_input.txt",
      .output_file = "/tmp/test_output.txt",
    };
    nya_assert(rule_outdated.policy == NYA_BUILD_IF_OUTDATED);
    nya_assert(nya_string_equals(rule_outdated.input_file, "/tmp/test_input.txt") == true);
    nya_assert(nya_string_equals(rule_outdated.output_file, "/tmp/test_output.txt") == true);
  }

  // TEST: NYA_BUILD_MAX_DEPENDENCIES constant
  {
    nya_assert(NYA_BUILD_MAX_DEPENDENCIES == 64);

    NYA_BuildRule rule = { 0 };
    // Verify we can access the full array
    for (u64 i = 0; i < NYA_BUILD_MAX_DEPENDENCIES; i++) {
      nya_assert(rule.dependencies[i] == nullptr);
      nya_assert(rule.pre_build_hooks[i] == nullptr);
      nya_assert(rule.post_build_hooks[i] == nullptr);
    }
  }

  // TEST: Complex command with multiple arguments
  {
    NYA_BuildRule rule = {
      .name   = "Complex Command",
      .policy = NYA_BUILD_ALWAYS,
      .command = {
        .program   = "clang",
        .arguments = {
          "-std=c23",
          "-Wall",
          "-Wextra",
          "-O2",
          "main.c",
          "-o",
          "main",
          nullptr,
        },
      },
    };

    nya_assert(nya_string_equals(rule.command.program, "clang") == true);
    nya_assert(nya_string_equals(rule.command.arguments[0], "-std=c23") == true);
    nya_assert(nya_string_equals(rule.command.arguments[1], "-Wall") == true);
    nya_assert(nya_string_equals(rule.command.arguments[2], "-Wextra") == true);
    nya_assert(nya_string_equals(rule.command.arguments[3], "-O2") == true);
    nya_assert(nya_string_equals(rule.command.arguments[4], "main.c") == true);
    nya_assert(nya_string_equals(rule.command.arguments[5], "-o") == true);
    nya_assert(nya_string_equals(rule.command.arguments[6], "main") == true);
    nya_assert(rule.command.arguments[7] == nullptr);
  }

  // CLEANUP
  nya_arena_destroy(arena);

  return 0;
}
