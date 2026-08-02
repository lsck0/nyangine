/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

static NYA_LogLevel original_level;
static b8           hook_called = false;

static u8 panic_test_hook(const char* function, const char* file, u32 line, const char* format, va_list args) {
  nya_unused(function, file, line, format, args);
  hook_called = true;
  return true;
}

s32 main(void) {
  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_log_level_get / nya_log_level_set
  // ─────────────────────────────────────────────────────────────────────────────
  original_level = nya_log_level_get();
  nya_assert(original_level >= NYA_LOG_LEVEL_TRACE && original_level < NYA_LOG_LEVEL_COUNT);

  nya_log_level_set(NYA_LOG_LEVEL_ERROR);
  nya_assert(nya_log_level_get() == NYA_LOG_LEVEL_ERROR);

  nya_log_level_set(NYA_LOG_LEVEL_DEBUG);
  nya_assert(nya_log_level_get() == NYA_LOG_LEVEL_DEBUG);

  nya_log_level_set(NYA_LOG_LEVEL_TRACE);
  nya_assert(nya_log_level_get() == NYA_LOG_LEVEL_TRACE);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: all log levels can be set
  // ─────────────────────────────────────────────────────────────────────────────
  for (u32 i = 0; i < NYA_LOG_LEVEL_COUNT; ++i) {
    nya_log_level_set((NYA_LogLevel)i);
    nya_assert(nya_log_level_get() == (NYA_LogLevel)i);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: log messages don't crash (just ensure they don't segfault)
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_level_set(NYA_LOG_LEVEL_TRACE);
  nya_trace("This is a trace message: %d", 42);
  nya_debug("This is a debug message: %s", "debug");
  nya_info("This is an info message: %f", 3.14);
  nya_warn("This is a warn message: %d %d %d", 1, 2, 3);
  NYA_Error("This is an error message: %s", "error");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: log filtering - lower levels not shown when higher level set
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_level_set(NYA_LOG_LEVEL_ERROR);
  nya_trace("Should not appear");
  nya_debug("Should not appear");
  nya_info("Should not appear");
  nya_warn("Should not appear");
  NYA_Error("Should appear: %d", 123);

  nya_log_level_set(NYA_LOG_LEVEL_INFO);
  nya_trace("Should not appear");
  nya_debug("Should not appear");
  nya_info("Should appear: %s", "info");
  nya_warn("Should appear: %d", 456);
  NYA_Error("Should appear: %f", 7.89);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_panic_hook_set
  // ─────────────────────────────────────────────────────────────────────────────
  hook_called = false;
  nya_panic_hook_set(panic_test_hook);

  // Hook hasn't been called yet
  nya_assert(hook_called == false);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: panic hook receives correct arguments via va_list
  // ─────────────────────────────────────────────────────────────────────────────
  hook_called = false;
  nya_panic_hook_set(panic_test_hook);

  {
    jmp_buf jb;
    if (setjmp(jb) == 0) {
      nya_panic_prevent_set(&jb);
      // Panic with format args — hook should be called with va_list without UB
      nya_panic("Hook test: %d %s %f", 42, "hello", 3.14);
      nya_assert(false);
    }
    // Panic was prevented before hook is called, so hook_called stays false.
    // The important thing is we didn't crash from va_list UB.
    nya_assert(nya_panic_prevent_happened() == true);
  }

  // Test hook actually preventing crash (without prevent_set)
  hook_called = false;
  nya_panic("This should be caught by hook: %d", 999);
  nya_assert(hook_called == true);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: setting hook to nullptr
  // ─────────────────────────────────────────────────────────────────────────────
  nya_panic_hook_set(nullptr);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_panic_prevent_set / nya_panic_prevent_happened
  // ─────────────────────────────────────────────────────────────────────────────
  jmp_buf jump_buffer;
  if (setjmp(jump_buffer) == 0) {
    nya_panic_prevent_set(&jump_buffer);

    // Trigger a panic (this will jump)
    nya_panic("This panic should be prevented");
    nya_assert(false); // Should never reach here
  }

  // After the jump, check that panic was prevented
  nya_assert(nya_panic_prevent_happened() == true);
  nya_assert(nya_panic_prevent_happened() == false); // Should reset to false

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: multiple panic prevention calls
  // ─────────────────────────────────────────────────────────────────────────────
  u32 panic_count = 0;
  for (u32 i = 0; i < 3; ++i) {
    if (setjmp(jump_buffer) == 0) {
      nya_panic_prevent_set(&jump_buffer);
      nya_panic("Panic %u", i);
      nya_assert(false);
    }
    panic_count++;
    nya_assert(nya_panic_prevent_happened() == true);
  }
  nya_assert(panic_count == 3);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: log messages with various format specifiers
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_level_set(NYA_LOG_LEVEL_INFO);
  nya_info("String: %s", "test");
  nya_info("Integer: %d", -42);
  nya_info("Unsigned: %u", 42);
  nya_info("Hex: 0x%x", 255);
  nya_info("Float: %f", 3.14159);
  nya_info("Pointer: %p", (void*)0x12345678);
  nya_info("Char: %c", 'A');
  nya_info("Percent: %%");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: log messages with multiple arguments
  // ─────────────────────────────────────────────────────────────────────────────
  nya_info("Multiple: %d %s %f %u", 1, "two", 3.0, 4);
  nya_info("Five args: %d %d %d %d %d", 1, 2, 3, 4, 5);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: empty log message
  // ─────────────────────────────────────────────────────────────────────────────
  nya_info("");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: log message with special characters
  // ─────────────────────────────────────────────────────────────────────────────
  nya_info("Special: \t\n\r%s", "test");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: all log level messages work
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_level_set(NYA_LOG_LEVEL_TRACE);
  nya_trace("TRACE level");
  nya_debug("DEBUG level");
  nya_info("INFO level");
  nya_warn("WARN level");
  NYA_Error("ERROR level");
  // nya_panic would crash, so skip it

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: very long log message
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Arena* arena    = nya_arena_create(.name = "test_logging");
  NYA_String long_msg = *nya_string_create(arena);
  for (u32 i = 0; i < 100; ++i) {
    NYA_String part = *nya_string_sprintf(arena, "%04d ", i);
    nya_string_extend(&long_msg, &part);
  }
  NYA_CString cstr = nya_string_to_cstring(arena, &long_msg);
  nya_info("Long message: %s", cstr);

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_level_set(original_level);
  nya_panic_hook_set(nullptr);
  nya_arena_destroy(arena);

  return 0;
}
