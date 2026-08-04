/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

void dummy_fn_a(void) {}
void dummy_fn_b(void) {}
void dummy_fn_c(NYA_Event* e) { nya_unused(e); }

s32 main(void) {
#if NYA_CODE_HOT_RELOAD
  /*
   * The named callback registry is compiled out of a test build.
   *
   * _nya_callback exists only under NYA_CODE_HOT_RELOAD (debug and developer). Elsewhere
   * nya_callback carries the function pointer directly and the registry is not linked in,
   * so this file compiled but failed to link with 'undefined symbol: _nya_callback'.
   *
   * Guarded rather than deleted: the body is still correct against the current API and
   * compiles under a mode that has the subsystem. In NYA_EXECUTION_MODE=4 this test
   * therefore asserts nothing, which is a real coverage gap, not a passing test.
   */

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Zero handle returns nullptr
  // ─────────────────────────────────────────────────────────────────────────────
  {
    void* fn = nya_callback_get(0);
    nya_assert(fn == nullptr, "Handle 0 should return nullptr");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Register and retrieve a callback
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_CallbackHandle h = _nya_callback((NYA_Callback){ .name = "dummy_fn_a", .fn = (void*)dummy_fn_a });
    void* fn = nya_callback_get(h);
    nya_assert(fn == (void*)dummy_fn_a, "Should retrieve the registered function");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Multiple callbacks have unique handles
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_CallbackHandle h1 = _nya_callback((NYA_Callback){ .name = "fn1", .fn = (void*)dummy_fn_a });
    NYA_CallbackHandle h2 = _nya_callback((NYA_Callback){ .name = "fn2", .fn = (void*)dummy_fn_b });
    NYA_CallbackHandle h3 = _nya_callback((NYA_Callback){ .name = "fn3", .fn = (void*)dummy_fn_c });

    nya_assert(h1 != h2, "Handles should be unique");
    nya_assert(h2 != h3, "Handles should be unique");
    nya_assert(h1 != h3, "Handles should be unique");

    nya_assert(nya_callback_get(h1) == (void*)dummy_fn_a);
    nya_assert(nya_callback_get(h2) == (void*)dummy_fn_b);
    nya_assert(nya_callback_get(h3) == (void*)dummy_fn_c);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Handles are sequential
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_CallbackHandle h1 = _nya_callback((NYA_Callback){ .name = "seq1", .fn = (void*)dummy_fn_a });
    NYA_CallbackHandle h2 = _nya_callback((NYA_Callback){ .name = "seq2", .fn = (void*)dummy_fn_b });
    nya_assert(h2 == h1 + 1, "Handles should be sequential");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Callback with nya_callback macro
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_CallbackHandle h = nya_callback(dummy_fn_a);
    nya_assert(h != 0, "Macro should return a valid handle");
    nya_assert(nya_callback_get(h) == (void*)dummy_fn_a);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Many callbacks
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_CallbackHandle first = _nya_callback((NYA_Callback){ .name = "batch_0", .fn = (void*)dummy_fn_a });

    for (s32 i = 1; i < 100; i++) {
      _nya_callback((NYA_Callback){ .name = "batch_n", .fn = (void*)dummy_fn_b });
    }

    nya_assert(nya_callback_get(first) == (void*)dummy_fn_a, "First callback should still be valid after many inserts");
    nya_assert(nya_callback_get(first + 50) == (void*)dummy_fn_b, "Middle callback should be valid");
    nya_assert(nya_callback_get(first + 99) == (void*)dummy_fn_b, "Last callback should be valid");
  }

  nya_system_callback_deinit();
  SDL_Quit();
  _NYA_APP_INSTANCE = (NYA_App){ 0 };

  printf("PASSED: test_callback\n");
#endif

  return 0;
}
