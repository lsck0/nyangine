/**
 * Regression test for nya_backtrace_format overrunning its documented return contract.
 * */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Long enough that a small capacity is genuinely too small for it. */
#define NO_TRACE_TEXT "  <no stack trace available>\n"

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an empty backtrace into every capacity from 1 upward
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: empty backtrace into a short buffer\n");
  {
    NYA_Backtrace empty = { .count = 0 };

    for (u32 capacity = 1; capacity <= sizeof(NO_TRACE_TEXT) + 4; capacity++) {
      // A guard byte either side, so an off-by-one in the write is caught rather than inferred.
      u8 storage[128];
      nya_memset(storage, 0xAA, sizeof(storage));

      u8* buffer = &storage[8];
      u32 length = nya_backtrace_format(&empty, buffer, capacity);

      nya_assert(length < capacity, "capacity %u returned a length of %u, which is not inside it", capacity, length);

      // Always null terminated when capacity is non zero, per the header.
      nya_assert(buffer[length] == '\0', "capacity %u did not null terminate at the reported length", capacity);

      // Nothing outside [buffer, buffer + capacity) may have been touched.
      for (u32 i = 0; i < 8; i++) nya_assert(storage[i] == 0xAA, "capacity %u wrote %u bytes before the buffer", capacity, 8 - i);
      for (u64 i = 8 + capacity; i < sizeof(storage); i++) {
        nya_assert(storage[i] == 0xAA, "capacity %u wrote past the end of the buffer", capacity);
      }
    }
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a real captured backtrace into every capacity
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: a captured backtrace into a short buffer\n");
  {
    NYA_Backtrace trace = { 0 };
    nya_backtrace_capture(&trace, 0);

    for (u32 capacity = 1; capacity <= 256; capacity++) {
      u8 storage[512];
      nya_memset(storage, 0xAA, sizeof(storage));

      u8* buffer = &storage[8];
      u32 length = nya_backtrace_format(&trace, buffer, capacity);

      nya_assert(length < capacity, "capacity %u returned a length of %u, which is not inside it", capacity, length);
      nya_assert(buffer[length] == '\0', "capacity %u did not null terminate at the reported length", capacity);

      for (u32 i = 0; i < 8; i++) nya_assert(storage[i] == 0xAA, "capacity %u wrote before the buffer", capacity);
      for (u64 i = 8 + capacity; i < sizeof(storage); i++) {
        nya_assert(storage[i] == 0xAA, "capacity %u wrote past the end of the buffer", capacity);
      }
    }
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a capacity of zero writes nothing at all
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: zero capacity\n");
  {
    NYA_Backtrace empty = { .count = 0 };

    u8 storage[16];
    nya_memset(storage, 0xAA, sizeof(storage));

    nya_assert(nya_backtrace_format(&empty, storage, 0) == 0, "a zero capacity did not return zero");
    for (u64 i = 0; i < sizeof(storage); i++) nya_assert(storage[i] == 0xAA, "a zero capacity wrote to the buffer");
  }
  printf("  PASSED\n");

  printf("PASSED: test_bug_backtrace_format_capacity\n");
  return 0;
}
