/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_ConstCString test_file_path       = "test_file.txt";
  NYA_ConstCString test_file_copy_path  = "test_file_copy.txt";
  NYA_ConstCString test_file_moved_path = "test_file_moved.txt";
  NYA_String       file_content         = *nya_string_create(nya_arena_global);
  NYA_String       copied_file_content  = *nya_string_create(nya_arena_global);
  NYA_String       moved_file_content   = *nya_string_create(nya_arena_global);

  NYA_Error result;

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Files should not exist initially
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(!nya_filesystem_exists(test_file_path));
  nya_assert(!nya_filesystem_exists(test_file_copy_path));
  nya_assert(!nya_filesystem_exists(test_file_moved_path));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Write and read basic file
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_EXPECT(nya_file_write(test_file_path, "Hello, Nyangine!"));
  nya_assert(nya_filesystem_exists(test_file_path));
  NYA_EXPECT(nya_file_read(test_file_path, &file_content));
  nya_assert(nya_string_equals(&file_content, "Hello, Nyangine!"));
  nya_string_clear(&file_content);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Append to file
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_EXPECT(nya_file_append(test_file_path, " Appended text."));
  NYA_EXPECT(nya_file_read(test_file_path, &file_content));
  nya_assert(nya_string_equals(&file_content, "Hello, Nyangine! Appended text."));
  nya_string_clear(&file_content);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Copy file
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_EXPECT(nya_filesystem_copy(test_file_path, "test_file_copy.txt"));
  nya_assert(nya_filesystem_exists(test_file_copy_path));
  NYA_EXPECT(nya_file_read(test_file_copy_path, &copied_file_content));
  nya_assert(nya_string_equals(&copied_file_content, "Hello, Nyangine! Appended text."));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Move/rename file
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_EXPECT(nya_filesystem_move(test_file_copy_path, test_file_moved_path));
  nya_assert(!nya_filesystem_exists(test_file_copy_path));
  nya_assert(nya_filesystem_exists(test_file_moved_path));
  NYA_EXPECT(nya_file_read(test_file_moved_path, &moved_file_content));
  nya_assert(nya_string_equals(&moved_file_content, "Hello, Nyangine! Appended text."));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Delete files
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(!nya_filesystem_exists(test_file_copy_path));
  NYA_EXPECT(nya_filesystem_delete(test_file_path));
  nya_assert(!nya_filesystem_exists(test_file_path));
  NYA_EXPECT(nya_filesystem_delete(test_file_moved_path));
  nya_assert(!nya_filesystem_exists(test_file_moved_path));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Non-existent file operations
  // ─────────────────────────────────────────────────────────────────────────────
  nya_assert(!nya_filesystem_exists("nonexistent_file_12345.txt"));

  NYA_String   nonexistent_content = *nya_string_create(nya_arena_global);
  NYA_Error   read_result         = nya_file_read("nonexistent_file_12345.txt", &nonexistent_content);
  nya_assert(read_result.kind == NYA_ERROR_NOT_FOUND);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Copy non-existent source fails
  // ─────────────────────────────────────────────────────────────────────────────
  result = nya_filesystem_copy("nonexistent_src_12345.txt", "nonexistent_dst_12345.txt");
  nya_assert(result.kind != NYA_ERROR_NONE);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Move non-existent source fails
  // ─────────────────────────────────────────────────────────────────────────────
  result = nya_filesystem_move("nonexistent_src_12345.txt", "nonexistent_dst_12345.txt");
  nya_assert(result.kind != NYA_ERROR_NONE);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Last modified on non-existent file returns NOT_FOUND
  // ─────────────────────────────────────────────────────────────────────────────
  {
    u64 ts = 0;
    NYA_Error r = nya_filesystem_last_modified("nonexistent_file_12345.txt", &ts);
    nya_assert(r.kind == NYA_ERROR_NOT_FOUND);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Write empty content
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_ConstCString empty_file_path = "test_empty_file.txt";
  NYA_EXPECT(nya_file_write(empty_file_path, ""));
  nya_assert(nya_filesystem_exists(empty_file_path));
  NYA_String empty_content = *nya_string_create(nya_arena_global);
  NYA_EXPECT(nya_file_read(empty_file_path, &empty_content));
  nya_assert(nya_string_is_empty(&empty_content));
  NYA_EXPECT(nya_filesystem_delete(empty_file_path));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Write and read binary-like content
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_ConstCString binary_file_path = "test_binary_file.txt";
  NYA_EXPECT(nya_file_write(binary_file_path, "Line1\nLine2\nLine3\tTabbed"));
  NYA_String binary_content = *nya_string_create(nya_arena_global);
  NYA_EXPECT(nya_file_read(binary_file_path, &binary_content));
  nya_assert(nya_string_contains(&binary_content, "Line1"));
  nya_assert(nya_string_contains(&binary_content, "\n"));
  nya_assert(nya_string_contains(&binary_content, "\t"));
  NYA_EXPECT(nya_filesystem_delete(binary_file_path));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Overwrite existing file
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_ConstCString overwrite_path = "test_overwrite.txt";
  NYA_EXPECT(nya_file_write(overwrite_path, "Original content"));
  NYA_EXPECT(nya_file_write(overwrite_path, "New content"));
  NYA_String overwrite_content = *nya_string_create(nya_arena_global);
  NYA_EXPECT(nya_file_read(overwrite_path, &overwrite_content));
  nya_assert(nya_string_equals(&overwrite_content, "New content"));
  NYA_EXPECT(nya_filesystem_delete(overwrite_path));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Multiple appends
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_ConstCString append_path = "test_multi_append.txt";
  NYA_EXPECT(nya_file_write(append_path, "Start"));
  NYA_EXPECT(nya_file_append(append_path, " Middle"));
  NYA_EXPECT(nya_file_append(append_path, " End"));
  NYA_String append_content = *nya_string_create(nya_arena_global);
  NYA_EXPECT(nya_file_read(append_path, &append_content));
  nya_assert(nya_string_equals(&append_content, "Start Middle End"));
  NYA_EXPECT(nya_filesystem_delete(append_path));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Last modified timestamp
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_ConstCString timestamp_path = "test_timestamp.txt";
  NYA_EXPECT(nya_file_write(timestamp_path, "Testing timestamps"));

  u64 timestamp = 0;
  NYA_EXPECT(nya_filesystem_last_modified(timestamp_path, &timestamp));
  nya_assert(timestamp > 0);

  // Timestamp should be recent (within last hour for sanity check)
  u64 now = nya_clock_get_timestamp_ms();
  nya_assert(timestamp <= now);
  nya_assert((now - timestamp) < 3600000); // Less than 1 hour old

  NYA_EXPECT(nya_filesystem_delete(timestamp_path));

  // Non-existent file should fail
  u64 bad_timestamp = 0;
  result = nya_filesystem_last_modified("nonexistent_12345.txt", &bad_timestamp);
  nya_assert(result.kind != NYA_ERROR_NONE);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: Large file content
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_ConstCString large_file_path = "test_large_file.txt";
  NYA_String       large_content   = *nya_string_create_with_capacity(nya_arena_global, 40000);
  for (u32 i = 0; i < 1000; ++i) { nya_string_extend(&large_content, "Line of content with some data. "); }
  NYA_EXPECT(nya_file_write(large_file_path, nya_string_to_cstring(nya_arena_global, &large_content)));

  NYA_String read_large = *nya_string_create(nya_arena_global);
  NYA_EXPECT(nya_file_read(large_file_path, &read_large));
  nya_assert(read_large.length == large_content.length);
  NYA_EXPECT(nya_filesystem_delete(large_file_path));

  return 0;
}
