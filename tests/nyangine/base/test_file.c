/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_file");

  NYA_String test_content = *nya_string_from(arena, "Hello, World!");
  NYA_String read_back    = *nya_string_create(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_write / nya_file_read (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_Error write_ok = nya_file_write("test_file_write.txt", &test_content);
  nya_assert(write_ok.ok);

  NYA_Error read_ok = nya_file_read("test_file_write.txt", &read_back);
  nya_assert(read_ok.ok);
  nya_assert(nya_string_equals(&read_back, "Hello, World!"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_write / nya_file_read (NYA_String* path overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String path_str = *nya_string_from(arena, "test_file_write_path.txt");
  write_ok = nya_file_write(&path_str, &test_content);
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read(&path_str, &read_back);
  nya_assert(read_ok.ok);
  nya_assert(nya_string_equals(&read_back, "Hello, World!"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_write (cstring content overload)
  // ─────────────────────────────────────────────────────────────────────────────
  write_ok = nya_file_write("test_file_write_cstr.txt", "Direct cstring content");
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_file_write_cstr.txt", &read_back);
  nya_assert(read_ok.ok);
  nya_assert(nya_string_equals(&read_back, "Direct cstring content"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_write (NYA_String* path, cstring content overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String path2 = *nya_string_from(arena, "test_file_write_mixed.txt");
  write_ok = nya_file_write(&path2, "Mixed types");
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read(&path2, &read_back);
  nya_assert(read_ok.ok);
  nya_assert(nya_string_equals(&read_back, "Mixed types"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_append (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  write_ok = nya_file_write("test_file_append.txt", "Initial");
  nya_assert(write_ok.ok);

  write_ok = nya_file_append("test_file_append.txt", " Appended");
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_file_append.txt", &read_back);
  nya_assert(read_ok.ok);
  nya_assert(nya_string_equals(&read_back, "Initial Appended"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_append (NYA_String* overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String append_path    = *nya_string_from(arena, "test_file_append_str.txt");
  NYA_String append_content = *nya_string_from(arena, " Appended String");

  write_ok = nya_file_write(&append_path, "Start");
  nya_assert(write_ok.ok);

  write_ok = nya_file_append(&append_path, &append_content);
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read(&append_path, &read_back);
  nya_assert(read_ok.ok);
  nya_assert(nya_string_equals(&read_back, "Start Appended String"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_append (mixed overload)
  // ─────────────────────────────────────────────────────────────────────────────
  write_ok = nya_file_write("test_file_append_mixed.txt", "Base");
  nya_assert(write_ok.ok);

  write_ok = nya_file_append("test_file_append_mixed.txt", " Extra");
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_file_append_mixed.txt", &read_back);
  nya_assert(read_ok.ok);
  nya_assert(nya_string_equals(&read_back, "Base Extra"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: reading non-existent file returns NOT_FOUND
  // ─────────────────────────────────────────────────────────────────────────────
  read_ok = nya_file_read("nonexistent_file_12345.txt", &read_back);
  nya_assert(read_ok.kind == NYA_ERROR_NOT_FOUND);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: writing to invalid path returns error
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error r = nya_file_write("/nonexistent_dir_12345/file.txt", "data");
    nya_assert(!r.ok);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: writing to a file that was never opened is an assertion failure
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_File closed = { 0 };
    nya_assert(nya_file_is_open(&closed) == false);

    // An assertion, not a returned error. The fd API this replaced took a raw int and could only
    // report a bad handle at runtime; NYA_File knows whether it was opened, so using an unopened
    // one is a programming mistake and is treated as one.
    nya_expect_crash((void)nya_file_write_string(&closed, "bad handle"));
    nya_assert(nya_crash_caught()->source == NYA_CRASH_SOURCE_ASSERT);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: empty file read/write
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String empty = *nya_string_create(arena);
  write_ok = nya_file_write("test_empty_file.txt", &empty);
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_empty_file.txt", &read_back);
  nya_assert(read_ok.ok);
  nya_assert(nya_string_is_empty(&read_back));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: large file write/read
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String large = *nya_string_create(arena);
  for (u32 i = 0; i < 1000; ++i) {
    NYA_String num = *nya_string_sprintf(arena, "Line %u\n", i);
    nya_string_extend(&large, &num);
  }

  write_ok = nya_file_write("test_large_file.txt", &large);
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_large_file.txt", &read_back);
  nya_assert(read_ok.ok);
  nya_assert(read_back.length == large.length);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: binary data (with null bytes)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String binary = *nya_string_create(arena);
  for (u32 i = 0; i < 256; ++i) {
    u8 byte = (u8)i;
    nya_string_extend(&binary, &(NYA_String){ .items = &byte, .length = 1 });
  }

  write_ok = nya_file_write("test_binary_file.bin", &binary);
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_binary_file.bin", &read_back);
  nya_assert(read_ok.ok);
  nya_assert(read_back.length == 256);
  for (u32 i = 0; i < 256; ++i) { nya_assert(read_back.items[i] == (u8)i); }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: unicode content
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String unicode = *nya_string_from(arena, "Hello 世界 🌍 Привет");
  write_ok = nya_file_write("test_unicode.txt", &unicode);
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_unicode.txt", &read_back);
  nya_assert(read_ok.ok);
  nya_assert(nya_string_equals(&read_back, "Hello 世界 🌍 Привет"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: multiple appends to same file
  // ─────────────────────────────────────────────────────────────────────────────
  write_ok = nya_file_write("test_multi_append.txt", "A");
  nya_assert(write_ok.ok);
  write_ok = nya_file_append("test_multi_append.txt", "B");
  nya_assert(write_ok.ok);
  write_ok = nya_file_append("test_multi_append.txt", "C");
  nya_assert(write_ok.ok);
  write_ok = nya_file_append("test_multi_append.txt", "D");
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_multi_append.txt", &read_back);
  nya_assert(read_ok.ok);
  nya_assert(nya_string_equals(&read_back, "ABCD"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: special characters in content
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String special = *nya_string_from(arena, "Tab\tTab\nNewline\r\nCRLF");
  write_ok = nya_file_write("test_special_chars.txt", &special);
  nya_assert(write_ok.ok);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_special_chars.txt", &read_back);
  nya_assert(read_ok.ok);
  nya_assert(nya_string_equals(&read_back, "Tab\tTab\nNewline\r\nCRLF"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_write_string / nya_file_read_string (NYA_String* content)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_File file = { 0 };
    NYA_EXPECT(nya_file_open("test_fd_write.txt", NYA_FILE_MODE_WRITE | NYA_FILE_MODE_TRUNCATE, &file));
    NYA_String content = *nya_string_from(arena, "fd write test");
    NYA_Error ok = nya_file_write_string(&file, &content);
    nya_assert(ok.ok);
    nya_file_close(&file);

    NYA_EXPECT(nya_file_open("test_fd_write.txt", NYA_FILE_MODE_READ, &file));
    NYA_String fd_read_back = *nya_string_create(arena);
    ok = nya_file_read_string(&file, &fd_read_back);
    nya_assert(ok.ok);
    nya_assert(nya_string_equals(&fd_read_back, "fd write test"));
    nya_file_close(&file);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_write_string (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_File file = { 0 };
    NYA_EXPECT(nya_file_open("test_fd_write_cstr.txt", NYA_FILE_MODE_WRITE | NYA_FILE_MODE_TRUNCATE, &file));
    NYA_Error ok = nya_file_write_string(&file, "fd cstring write");
    nya_assert(ok.ok);
    nya_file_close(&file);

    NYA_EXPECT(nya_file_open("test_fd_write_cstr.txt", NYA_FILE_MODE_READ, &file));
    NYA_String fd_read_back = *nya_string_create(arena);
    ok = nya_file_read_string(&file, &fd_read_back);
    nya_assert(ok.ok);
    nya_assert(nya_string_equals(&fd_read_back, "fd cstring write"));
    nya_file_close(&file);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_append_string (NYA_String* overload)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_File file = { 0 };
    NYA_EXPECT(nya_file_open("test_fd_append.txt", NYA_FILE_MODE_WRITE | NYA_FILE_MODE_TRUNCATE, &file));
    NYA_Error ok = nya_file_write_string(&file, "Start");
    nya_assert(ok.ok);

    NYA_String append_str = *nya_string_from(arena, " Middle");
    ok = nya_file_append_string(&file, &append_str);
    nya_assert(ok.ok);
    nya_file_close(&file);

    NYA_EXPECT(nya_file_open("test_fd_append.txt", NYA_FILE_MODE_READ, &file));
    NYA_String fd_read_back = *nya_string_create(arena);
    ok = nya_file_read_string(&file, &fd_read_back);
    nya_assert(ok.ok);
    nya_assert(nya_string_equals(&fd_read_back, "Start Middle"));
    nya_file_close(&file);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_append_string (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_File file = { 0 };
    NYA_EXPECT(nya_file_open("test_fd_append_cstr.txt", NYA_FILE_MODE_WRITE | NYA_FILE_MODE_TRUNCATE, &file));
    NYA_Error ok = nya_file_write_string(&file, "Base");
    nya_assert(ok.ok);

    ok = nya_file_append_string(&file, " Added");
    nya_assert(ok.ok);
    nya_file_close(&file);

    NYA_EXPECT(nya_file_open("test_fd_append_cstr.txt", NYA_FILE_MODE_READ, &file));
    NYA_String fd_read_back = *nya_string_create(arena);
    ok = nya_file_read_string(&file, &fd_read_back);
    nya_assert(ok.ok);
    nya_assert(nya_string_equals(&fd_read_back, "Base Added"));
    nya_file_close(&file);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_read_string on an empty file
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_File file = { 0 };
    NYA_EXPECT(nya_file_open("test_fd_empty.txt", NYA_FILE_MODE_WRITE | NYA_FILE_MODE_TRUNCATE, &file));
    nya_file_close(&file);

    NYA_EXPECT(nya_file_open("test_fd_empty.txt", NYA_FILE_MODE_READ, &file));
    NYA_String fd_read_back = *nya_string_create(arena);
    NYA_Error ok = nya_file_read_string(&file, &fd_read_back);
    nya_assert(ok.ok);
    nya_assert(nya_string_is_empty(&fd_read_back));
    nya_file_close(&file);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  remove("test_file_write.txt");
  remove("test_file_write_path.txt");
  remove("test_file_write_cstr.txt");
  remove("test_file_write_mixed.txt");
  remove("test_file_append.txt");
  remove("test_file_append_str.txt");
  remove("test_file_append_mixed.txt");
  remove("test_empty_file.txt");
  remove("test_large_file.txt");
  remove("test_binary_file.bin");
  remove("test_unicode.txt");
  remove("test_multi_append.txt");
  remove("test_special_chars.txt");
  remove("test_fd_write.txt");
  remove("test_fd_write_cstr.txt");
  remove("test_fd_append.txt");
  remove("test_fd_append_cstr.txt");
  remove("test_fd_empty.txt");

  nya_arena_destroy(arena);

  return 0;
}
