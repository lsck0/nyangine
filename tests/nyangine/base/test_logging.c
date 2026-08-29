/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

static NYA_LogLevel original_level;

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
  nya_log_trace("This is a trace message: %d", 42);
  nya_log_debug("This is a debug message: %s", "debug");
  nya_log_info("This is an info message: %f", 3.14);
  nya_log_warn("This is a warn message: %d %d %d", 1, 2, 3);
  nya_log_error("This is an error message: %s", "error");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: log filtering - lower levels not shown when higher level set
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_level_set(NYA_LOG_LEVEL_ERROR);
  nya_log_trace("Should not appear");
  nya_log_debug("Should not appear");
  nya_log_info("Should not appear");
  nya_log_warn("Should not appear");
  nya_log_error("Should appear: %d", 123);

  nya_log_level_set(NYA_LOG_LEVEL_INFO);
  nya_log_trace("Should not appear");
  nya_log_debug("Should not appear");
  nya_log_info("Should appear: %s", "info");
  nya_log_warn("Should appear: %d", 456);
  nya_log_error("Should appear: %f", 7.89);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_expect_crash catches a panic, and reports where it came from
  //
  // This replaced a panic *hook* plus nya_panic_prevent_set/_happened. The hook let an observer
  // both see and swallow a panic; the crash API separates those, so a test arms a frame and then
  // inspects NYA_CrashInfo rather than setting a global flag from a callback.
  // ─────────────────────────────────────────────────────────────────────────────
  nya_expect_crash(nya_log_panic("This panic should be caught"));
  nya_assert(nya_crash_caught() != nullptr);
  nya_assert(nya_crash_caught()->source == NYA_CRASH_SOURCE_PANIC);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a panic carrying format arguments, which is where the old hook risked va_list UB
  // ─────────────────────────────────────────────────────────────────────────────
  nya_expect_crash(nya_log_panic("Panic with args: %d %s %f", 999, "text", 1.5));
  nya_assert(nya_crash_caught()->source == NYA_CRASH_SOURCE_PANIC);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a failed assertion reports as an assert, not a panic
  // ─────────────────────────────────────────────────────────────────────────────
  nya_expect_crash(nya_assert(false, "deliberate"));
  nya_assert(nya_crash_caught()->source == NYA_CRASH_SOURCE_ASSERT);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: repeated crashes, each caught independently
  // ─────────────────────────────────────────────────────────────────────────────
  for (u32 i = 0; i < 3; ++i) {
    nya_expect_crash(nya_log_panic("Panic %u", i));
    nya_assert(nya_crash_caught() != nullptr);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: log messages with various format specifiers
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_level_set(NYA_LOG_LEVEL_INFO);
  nya_log_info("String: %s", "test");
  nya_log_info("Integer: %d", -42);
  nya_log_info("Unsigned: %u", 42);
  nya_log_info("Hex: 0x%x", 255);
  nya_log_info("Float: %f", 3.14159);
  nya_log_info("Pointer: %p", (void*)0x12345678);
  nya_log_info("Char: %c", 'A');
  nya_log_info("Percent: %%");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: log messages with multiple arguments
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_info("Multiple: %d %s %f %u", 1, "two", 3.0, 4);
  nya_log_info("Five args: %d %d %d %d %d", 1, 2, 3, 4, 5);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: empty log message
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_info("");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: log message with special characters
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_info("Special: \t\n\r%s", "test");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: all log level messages work
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_level_set(NYA_LOG_LEVEL_TRACE);
  nya_log_trace("TRACE level");
  nya_log_debug("DEBUG level");
  nya_log_info("INFO level");
  nya_log_warn("WARN level");
  nya_log_error("ERROR level");
  // nya_log_panic would crash, so skip it

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
  nya_log_info("Long message: %s", cstr);

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_log_level_set(original_level);
  nya_arena_destroy(arena);

  return 0;
}
