/**
 * THIS FILE WAS CLANKER WANKED !!!
 *
 * Rewritten from NYA_CLEANUP_WITH / NYA_DEFINE_CLEANUP_FN, which base_clean.h no longer defines.
 * Scope exit cleanup is now C2y `defer` (via <stddefer.h>, enabled by -fdefer-ts), so the property
 * under test is unchanged and only the spelling moved: a statement attached to a scope runs when
 * control leaves it, once per scope, innermost first.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

// Test counter for cleanup function calls
static s32 g_test_cleanup_called = 0;

// Simple test structure for cleanup testing
typedef struct TestResource TestResource;
struct TestResource {
  s32  id;
  s32* cleanup_counter;
};

// Custom cleanup function that just increments a counter
void test_resource_cleanup(TestResource* resource) {
  if (resource && resource->cleanup_counter) { (*resource->cleanup_counter)++; }
  g_test_cleanup_called++;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_guard");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: defer with custom resource cleanup
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 cleanup_counter   = 0;
    g_test_cleanup_called = 0;

    {
      TestResource resource = { .id = 42, .cleanup_counter = &cleanup_counter };
      defer        test_resource_cleanup(&resource);

      // Verify initial state
      nya_assert(resource.id == 42);
      nya_assert(cleanup_counter == 0);
      nya_assert(g_test_cleanup_called == 0);
    } // resource should be automatically cleaned up here

    // Verify cleanup was called
    nya_assert(cleanup_counter == 1);
    nya_assert(g_test_cleanup_called == 1);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Multiple deferred statements in the same scope
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 cleanup_counter1  = 0;
    s32 cleanup_counter2  = 0;
    g_test_cleanup_called = 0;

    {
      TestResource resource1 = { .id = 1, .cleanup_counter = &cleanup_counter1 };
      defer        test_resource_cleanup(&resource1);

      TestResource resource2 = { .id = 2, .cleanup_counter = &cleanup_counter2 };
      defer        test_resource_cleanup(&resource2);

      // Verify initial state
      nya_assert(cleanup_counter1 == 0);
      nya_assert(cleanup_counter2 == 0);
      nya_assert(g_test_cleanup_called == 0);
    } // Both should be cleaned up here

    // Verify both were cleaned up
    nya_assert(cleanup_counter1 == 1);
    nya_assert(cleanup_counter2 == 1);
    nya_assert(g_test_cleanup_called == 2);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Nested scopes, inner cleanup runs before the outer scope ends
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 outer_counter     = 0;
    s32 inner_counter     = 0;
    g_test_cleanup_called = 0;

    {
      TestResource outer_resource = { .id = 10, .cleanup_counter = &outer_counter };
      defer        test_resource_cleanup(&outer_resource);

      {
        TestResource inner_resource = { .id = 20, .cleanup_counter = &inner_counter };
        defer        test_resource_cleanup(&inner_resource);

        // Only outer should exist so far
        nya_assert(outer_counter == 0);
        nya_assert(inner_counter == 0);
        nya_assert(g_test_cleanup_called == 0);
      } // inner_resource should be cleaned up here

      // inner should be cleaned up, outer should not
      nya_assert(outer_counter == 0);
      nya_assert(inner_counter == 1);
      nya_assert(g_test_cleanup_called == 1);
    } // outer_resource should be cleaned up here

    // Both should be cleaned up now
    nya_assert(outer_counter == 1);
    nya_assert(inner_counter == 1);
    nya_assert(g_test_cleanup_called == 2);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Deferred statements run in reverse order of declaration
  //
  // Worth pinning because it is the one thing NYA_CLEANUP_WITH could not promise: destructor order
  // there was the compiler's choice, whereas defer is specified last in, first out.
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 order[3] = { 0, 0, 0 };
    s32 next     = 0;

    {
      defer order[next++] = 1;
      defer order[next++] = 2;
      defer order[next++] = 3;
    }

    nya_assert(order[0] == 3);
    nya_assert(order[1] == 2);
    nya_assert(order[2] == 1);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: defer with nya_arena_destroy, which is how the engine itself uses it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Arena* temp_arena = nya_arena_create(.name = "temp");
    defer      nya_arena_destroy(temp_arena);

    nya_assert(temp_arena != nullptr);
    // Destroyed as this scope ends. That it happens exactly once is what LSan checks for us.
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  return 0;
}
