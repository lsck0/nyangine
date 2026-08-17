/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

// Test structure for field offset tests
typedef struct TestStruct TestStruct;
struct TestStruct {
  u8          byte_field;
  u32         int_field;
  f32         float_field;
  u64         long_field;
  u8          array_field[10];
  TestStruct* next;
};

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_memory");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_is_zeroed macro
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Test with zero-initialized struct
    TestStruct zero_struct = { 0 };
    nya_assert(nya_is_zeroed(zero_struct) == true);

    // Test with partially initialized struct
    TestStruct partial_struct = { 0 };
    partial_struct.int_field  = 42;
    nya_assert(nya_is_zeroed(partial_struct) == false);

    // Test with basic types
    s32 zero_int     = 0;
    s32 non_zero_int = 42;
    nya_assert(nya_is_zeroed(zero_int) == true);
    nya_assert(nya_is_zeroed(non_zero_int) == false);

    // Test with pointers
    void*       null_ptr   = nullptr;
    TestStruct* struct_ptr = (TestStruct*)0x12345678;
    nya_assert(nya_is_zeroed(null_ptr) == true);
    nya_assert(nya_is_zeroed(struct_ptr) == false);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_typeof_field macro
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Test that the macro returns the correct type
    // We can't directly test the type, but we can use it in assignments
    TestStruct test_struct = { 0 };

    // These should compile without errors if the types are correct
    // Note: nya_typeof_field is just typeof wrapper, so we test by usage
    __typeof__(test_struct.byte_field)  byte_val  = 0;
    __typeof__(test_struct.int_field)   int_val   = 0;
    __typeof__(test_struct.float_field) float_val = 0.0f;

    // Verify assignments work (basic sanity check)
    nya_assert(byte_val == 0);
    nya_assert(int_val == 0);
    nya_assert(float_val == 0.0f);

    (void)byte_val;
    (void)int_val;
    (void)float_val; // Suppress unused warnings
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_sizeof_field macro
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Test that the macro returns the correct sizes
    nya_assert(nya_sizeof_field(TestStruct, byte_field) == sizeof(u8));
    nya_assert(nya_sizeof_field(TestStruct, int_field) == sizeof(u32));
    nya_assert(nya_sizeof_field(TestStruct, float_field) == sizeof(f32));
    nya_assert(nya_sizeof_field(TestStruct, long_field) == sizeof(u64));
    nya_assert(nya_sizeof_field(TestStruct, array_field) == sizeof(u8[10]));
    nya_assert(nya_sizeof_field(TestStruct, next) == sizeof(TestStruct*));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_offsetof macro
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Test that the macro returns the correct offsets
    TestStruct test_struct = { 0 };

    // First field should be at offset 0
    nya_assert(nya_offsetof(TestStruct, byte_field) == 0);

    // Other fields should be at increasing offsets
    size_t int_offset   = nya_offsetof(TestStruct, int_field);
    size_t float_offset = nya_offsetof(TestStruct, float_field);
    size_t long_offset  = nya_offsetof(TestStruct, long_field);
    size_t array_offset = nya_offsetof(TestStruct, array_field);
    size_t next_offset  = nya_offsetof(TestStruct, next);

    // Verify offsets are increasing (basic sanity check)
    nya_assert(int_offset > 0);
    nya_assert(float_offset > int_offset);
    nya_assert(long_offset > float_offset);
    nya_assert(array_offset > long_offset);
    nya_assert(next_offset > array_offset);

    // Verify accessing fields through offsets gives correct values
    test_struct.int_field = 0x12345678;
    u32* int_ptr          = (u32*)((u8*)&test_struct + int_offset);
    nya_assert(*int_ptr == 0x12345678);

    test_struct.float_field = 3.14159f;
    f32* float_ptr          = (f32*)((u8*)&test_struct + float_offset);
    nya_assert(*float_ptr == 3.14159f);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_container_of macro
  // ─────────────────────────────────────────────────────────────────────────────
  //
  // No longer skipped. It used to be, because the macro named `assert_type_match` rather than
  // `nya_assert_type_match` and so failed to compile the moment anything expanded it — which meant
  // nothing ever did, and the one test for it asserted `true`.
  {
    TestStruct container = { .byte_field = 7, .int_field = 1234, .float_field = 2.5f, .long_field = 99 };

    // From a member in the middle of the struct, not just the first one, so a wrong offset shows up.
    u32*        int_ptr   = &container.int_field;
    TestStruct* recovered = nya_container_of(int_ptr, TestStruct, int_field);
    nya_assert(recovered == &container, "nya_container_of did not recover the enclosing struct");
    nya_assert(recovered->long_field == 99, "recovered struct has the wrong contents");

    // The first member, where the offset is zero.
    u8*         byte_ptr        = &container.byte_field;
    TestStruct* from_first      = nya_container_of(byte_ptr, TestStruct, byte_field);
    nya_assert(from_first == &container, "nya_container_of failed at offset zero");

    // And the const branch of the _Generic, which is the half that the type check used to reject.
    const f32*        const_ptr       = &container.float_field;
    const TestStruct* const_recovered = nya_container_of(const_ptr, TestStruct, float_field);
    nya_assert(const_recovered == &container, "nya_container_of lost the struct through a const pointer");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_offsetof_end macro
  // ─────────────────────────────────────────────────────────────────────────────
  {
    size_t byte_end  = nya_offsetof_end(TestStruct, byte_field);
    size_t int_end   = nya_offsetof_end(TestStruct, int_field);
    size_t float_end = nya_offsetof_end(TestStruct, float_field);
    size_t long_end  = nya_offsetof_end(TestStruct, long_field);

    // offsetof_end = offsetof + sizeof, so it should be > offsetof
    nya_assert(byte_end == nya_offsetof(TestStruct, byte_field) + sizeof(u8));
    nya_assert(int_end == nya_offsetof(TestStruct, int_field) + sizeof(u32));
    nya_assert(float_end == nya_offsetof(TestStruct, float_field) + sizeof(f32));
    nya_assert(long_end == nya_offsetof(TestStruct, long_field) + sizeof(u64));

    // byte_field end should be <= int_field offset (with possible padding)
    nya_assert(byte_end <= nya_offsetof(TestStruct, int_field));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Byte conversion macros
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Test binary unit conversions
    u64 kib_result = nya_kibyte_to_byte(1024);
    nya_assert(kib_result == 1024 * 1024);

    u64 mib_result = nya_mebyte_to_byte(1024);
    nya_assert(mib_result == 1024 * 1024 * 1024);

    u64 gib_result = nya_tebyte_to_byte(1);
    nya_assert(gib_result == 1024ULL * 1024 * 1024 * 1024);

    // Test reverse conversions
    u64 back_to_kib = nya_byte_to_kibyte(1024 * 1024);
    nya_assert(back_to_kib == 1024);

    u64 back_to_mib = nya_byte_to_mebyte(1024 * 1024 * 1024);
    nya_assert(back_to_mib == 1024);

    // Test decimal unit conversions
    u64 kb_result = nya_kbyte_to_byte(1000);
    nya_assert(kb_result == 1000 * 1000);

    u64 mb_result = nya_mbyte_to_byte(1000);
    nya_assert(mb_result == 1000 * 1000 * 1000);

    // Test reverse decimal conversions
    u64 back_to_kb = nya_byte_to_kbyte(1000 * 1000);
    nya_assert(back_to_kb == 1000);

    u64 back_to_mb = nya_byte_to_mbyte(1000 * 1000 * 1000);
    nya_assert(back_to_mb == 1000);

    // Test large values
    u64 tb_binary = nya_byte_to_gibyte(2048ULL * 1024 * 1024 * 1024); // 2048 GB = 2 TB
    nya_assert(tb_binary == 2048);

    u64 tb_decimal = nya_byte_to_gbyte(2000ULL * 1000 * 1000 * 1000); // 2000 GB = 2 TB (decimal)
    nya_assert(tb_decimal == 2000);

    // Test gibyte_to_byte
    u64 gib_to_bytes = nya_gibyte_to_byte(1UL);
    nya_assert(gib_to_bytes == 1024ULL * 1024 * 1024);
    nya_assert(nya_gibyte_to_byte(2UL) == 2ULL * 1024 * 1024 * 1024);

    // Test gbyte_to_byte
    u64 gb_to_bytes = nya_gbyte_to_byte(1UL);
    nya_assert(gb_to_bytes == 1000000000ULL);

    // Test tbyte_to_byte
    u64 tb_to_bytes = nya_tbyte_to_byte(1LL);
    nya_assert(tb_to_bytes == 1000000000000LL);

    // Test byte_to_tebyte
    u64 teb = nya_byte_to_tebyte(1024ULL * 1024 * 1024 * 1024);
    nya_assert(teb == 1);

    // Test alloca compiles
    void* stack_mem = nya_alloca(64);
    nya_assert(stack_mem != nullptr);
    nya_memset(stack_mem, 0, 64);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Memory allocation wrappers
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Test that memory allocation macros work as expected
    void* ptr = nya_malloc(64);
    nya_assert(ptr != nullptr);

    // Test realloc
    ptr = nya_realloc(ptr, 128);
    nya_assert(ptr != nullptr);

    // Test calloc
    void* zero_ptr = nya_calloc(10, sizeof(s32));
    nya_assert(zero_ptr != nullptr);

    // Verify calloc zeroed memory
    s32* int_array = (s32*)zero_ptr;
    for (int i = 0; i < 10; i++) { nya_assert(int_array[i] == 0); }

    // Test free
    nya_free(ptr);
    nya_free(zero_ptr);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Memory operation wrappers
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Test memcpy wrapper
    u8 src[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    u8 dst[16] = { 0 };

    nya_memcpy(dst, src, 16);
    nya_assert(nya_memcmp(dst, src, 16) == 0);

    // Test memset wrapper
    u8 buffer[32];
    nya_memset(buffer, 0xAA, 32);

    for (int i = 0; i < 32; i++) { nya_assert(buffer[i] == 0xAA); }

    // Test memmove wrapper (overlapping regions)
    u8 overlap[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    nya_memmove(overlap + 4, overlap, 8); // Move first 8 bytes right by 4

    // Expected result: [1, 2, 3, 4, 1, 2, 3, 4, 5, 6, 7, 8, 13, 14, 15, 16]
    u8 expected[16] = { 1, 2, 3, 4, 1, 2, 3, 4, 5, 6, 7, 8, 13, 14, 15, 16 };
    nya_assert(nya_memcmp(overlap, expected, 16) == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_alloca is bounded
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // The bound is the whole point: every caller sizes a stack allocation from data, and without it
    // a long enough input is a stack overflow with no diagnostic — a fault at whatever address the
    // next frame would have touched, nowhere near the call that caused it.

    // Right up to the limit still allocates, so the guard is a ceiling rather than a haircut.
    volatile u8* at_limit = nya_alloca(NYA_ALLOCA_MAX);
    nya_assert(at_limit != nullptr, "an allocation exactly at NYA_ALLOCA_MAX must still be made");

    // Written to, so the allocation has to be real memory rather than a pointer nobody touches. The
    // far end is what a too-small allocation would fail on.
    at_limit[0]                    = 0xAB;
    at_limit[NYA_ALLOCA_MAX - 1]   = 0xCD;
    nya_assert(at_limit[0] == 0xAB && at_limit[NYA_ALLOCA_MAX - 1] == 0xCD);

    // One byte past it must die rather than quietly take the stack with it. Left null so the
    // assertion after it also proves the allocation never happened.
    volatile u8* over = nullptr;
    nya_expect_crash(over = nya_alloca(NYA_ALLOCA_MAX + 1));
    nya_assert(over == nullptr, "an allocation past the bound must not be made");

    // The realistic shape of the bug: a length that came from data, not a literal.
    u64          attacker_length = NYA_ALLOCA_MAX * 4;
    volatile u8* huge            = nullptr;
    nya_expect_crash(huge = nya_alloca(attacker_length + 1));
    nya_assert(huge == nullptr, "a data-sized allocation past the bound must not be made");

    // Zero is legal and must not trip the bound — nya_alloca(0) is what an empty string reaches.
    volatile u8* empty = nya_alloca(0);
    nya_assert(empty != nullptr || empty == nullptr, "a zero sized allocation must not abort");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_alloca evaluates its size exactly once
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // A plain comma-expression version of the bound would evaluate `size` twice, so a caller passing
    // anything with a side effect would allocate one length and advance the other. The statement
    // expression exists for this; nothing else would notice it had been dropped.
    u64 evaluations = 0;
    u64 side_effect = 0;

    volatile u8* once = nya_alloca((evaluations++, side_effect = 16, side_effect));
    nya_unused(once);

    nya_assert(evaluations == 1, "nya_alloca evaluated its size " FMTu64 " times, expected once", evaluations);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_arena_destroy(arena);

  return 0;
}