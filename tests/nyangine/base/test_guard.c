/**
 * THIS FILE WAS CLANKER WANKED !!!
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

// Define cleanup function using the macro
NYA_DEFINE_CLEANUP_FN(test_resource_cleanup, TestResource, resource, test_resource_cleanup(&resource))

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_guard");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_CLEANUP_WITH with custom resource cleanup
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 cleanup_counter   = 0;
    g_test_cleanup_called = 0;

    {
      NYA_CLEANUP_WITH(test_resource_cleanup) TestResource resource = { .id = 42, .cleanup_counter = &cleanup_counter };

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
  // TEST: Multiple guarded variables in same scope
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 cleanup_counter1  = 0;
    s32 cleanup_counter2  = 0;
    g_test_cleanup_called = 0;

    {
      NYA_CLEANUP_WITH(test_resource_cleanup) TestResource resource1 = { .id = 1, .cleanup_counter = &cleanup_counter1 };

      NYA_CLEANUP_WITH(test_resource_cleanup) TestResource resource2 = { .id = 2, .cleanup_counter = &cleanup_counter2 };

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
  // TEST: Nested scopes with guarded variables
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 outer_counter     = 0;
    s32 inner_counter     = 0;
    g_test_cleanup_called = 0;

    {
      NYA_CLEANUP_WITH(test_resource_cleanup) TestResource outer_resource = { .id = 10, .cleanup_counter = &outer_counter };

      {
        NYA_CLEANUP_WITH(test_resource_cleanup) TestResource inner_resource = { .id = 20, .cleanup_counter = &inner_counter };

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
  // TEST: Built-in close cleanup with file descriptor
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Note: We can't easily test actual file operations without affecting the system,
    // but we can verify the macro compiles and doesn't crash with invalid values
    {
      NYA_CLEANUP_WITH(close) s32 fake_fd = -1; // Invalid file descriptor
      // When this goes out of scope, it should call close(-1) which should fail harmlessly
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Built-in fclose cleanup with FILE*
  // ─────────────────────────────────────────────────────────────────────────────
  {
    {
      NYA_CLEANUP_WITH(fclose) FILE* fake_file = nullptr;
      // When this goes out of scope, it should call fclose(nullptr) which should fail harmlessly
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: NYA_CLEANUP_WITH with nya_arena_destroy
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_CLEANUP_WITH(nya_arena_destroy) NYA_Arena* temp_arena = nya_arena_create(.name = "temp");
    // temp_arena should be automatically destroyed when going out of scope
    // No assertion needed - this is mainly to verify compilation and no crashes
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  return 0;
}