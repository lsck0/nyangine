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
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  NYA_Error read_ok = nya_file_read("test_file_write.txt", &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
  nya_assert(nya_string_equals(&read_back, "Hello, World!"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_write / nya_file_read (NYA_String* path overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String path_str = *nya_string_from(arena, "test_file_write_path.txt");
  write_ok = nya_file_write(&path_str, &test_content);
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read(&path_str, &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
  nya_assert(nya_string_equals(&read_back, "Hello, World!"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_write (cstring content overload)
  // ─────────────────────────────────────────────────────────────────────────────
  write_ok = nya_file_write("test_file_write_cstr.txt", "Direct cstring content");
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_file_write_cstr.txt", &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
  nya_assert(nya_string_equals(&read_back, "Direct cstring content"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_write (NYA_String* path, cstring content overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String path2 = *nya_string_from(arena, "test_file_write_mixed.txt");
  write_ok = nya_file_write(&path2, "Mixed types");
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read(&path2, &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
  nya_assert(nya_string_equals(&read_back, "Mixed types"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_append (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  write_ok = nya_file_write("test_file_append.txt", "Initial");
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  write_ok = nya_file_append("test_file_append.txt", " Appended");
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_file_append.txt", &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
  nya_assert(nya_string_equals(&read_back, "Initial Appended"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_append (NYA_String* overload)
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String append_path    = *nya_string_from(arena, "test_file_append_str.txt");
  NYA_String append_content = *nya_string_from(arena, " Appended String");

  write_ok = nya_file_write(&append_path, "Start");
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  write_ok = nya_file_append(&append_path, &append_content);
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read(&append_path, &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
  nya_assert(nya_string_equals(&read_back, "Start Appended String"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_file_append (mixed overload)
  // ─────────────────────────────────────────────────────────────────────────────
  write_ok = nya_file_write("test_file_append_mixed.txt", "Base");
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  write_ok = nya_file_append("test_file_append_mixed.txt", " Extra");
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_file_append_mixed.txt", &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
  nya_assert(nya_string_equals(&read_back, "Base Extra"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: reading non-existent file returns NOT_FOUND
  // ─────────────────────────────────────────────────────────────────────────────
  read_ok = nya_file_read("nonexistent_file_12345.txt", &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NOT_FOUND);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: writing to invalid path returns error
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error r = nya_file_write("/nonexistent_dir_12345/file.txt", "data");
    nya_assert(r.error != NYA_ERROR_NONE);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_fd_write to invalid fd returns error
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error r = nya_fd_write(-1, "bad fd");
    nya_assert(r.error != NYA_ERROR_NONE);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: empty file read/write
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String empty = *nya_string_create(arena);
  write_ok = nya_file_write("test_empty_file.txt", &empty);
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_empty_file.txt", &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
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
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_large_file.txt", &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
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
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_binary_file.bin", &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
  nya_assert(read_back.length == 256);
  for (u32 i = 0; i < 256; ++i) { nya_assert(read_back.items[i] == (u8)i); }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: unicode content
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String unicode = *nya_string_from(arena, "Hello 世界 🌍 Привет");
  write_ok = nya_file_write("test_unicode.txt", &unicode);
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_unicode.txt", &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
  nya_assert(nya_string_equals(&read_back, "Hello 世界 🌍 Привет"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: multiple appends to same file
  // ─────────────────────────────────────────────────────────────────────────────
  write_ok = nya_file_write("test_multi_append.txt", "A");
  nya_assert(write_ok.error == NYA_ERROR_NONE);
  write_ok = nya_file_append("test_multi_append.txt", "B");
  nya_assert(write_ok.error == NYA_ERROR_NONE);
  write_ok = nya_file_append("test_multi_append.txt", "C");
  nya_assert(write_ok.error == NYA_ERROR_NONE);
  write_ok = nya_file_append("test_multi_append.txt", "D");
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_multi_append.txt", &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
  nya_assert(nya_string_equals(&read_back, "ABCD"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: special characters in content
  // ─────────────────────────────────────────────────────────────────────────────
  NYA_String special = *nya_string_from(arena, "Tab\tTab\nNewline\r\nCRLF");
  write_ok = nya_file_write("test_special_chars.txt", &special);
  nya_assert(write_ok.error == NYA_ERROR_NONE);

  nya_string_clear(&read_back);
  read_ok = nya_file_read("test_special_chars.txt", &read_back);
  nya_assert(read_ok.error == NYA_ERROR_NONE);
  nya_assert(nya_string_equals(&read_back, "Tab\tTab\nNewline\r\nCRLF"));

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_fd_write / nya_fd_read (NYA_String* content)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 fd = open("test_fd_write.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    nya_assert(fd >= 0);
    NYA_String content = *nya_string_from(arena, "fd write test");
    NYA_Error ok = nya_fd_write(fd, &content);
    nya_assert(ok.error == NYA_ERROR_NONE);
    close(fd);

    fd = open("test_fd_write.txt", O_RDONLY);
    nya_assert(fd >= 0);
    NYA_String fd_read_back = *nya_string_create(arena);
    ok = nya_fd_read(fd, &fd_read_back);
    nya_assert(ok.error == NYA_ERROR_NONE);
    nya_assert(nya_string_equals(&fd_read_back, "fd write test"));
    close(fd);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_fd_write (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 fd = open("test_fd_write_cstr.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    nya_assert(fd >= 0);
    NYA_Error ok = nya_fd_write(fd, "fd cstring write");
    nya_assert(ok.error == NYA_ERROR_NONE);
    close(fd);

    fd = open("test_fd_write_cstr.txt", O_RDONLY);
    nya_assert(fd >= 0);
    NYA_String fd_read_back = *nya_string_create(arena);
    ok = nya_fd_read(fd, &fd_read_back);
    nya_assert(ok.error == NYA_ERROR_NONE);
    nya_assert(nya_string_equals(&fd_read_back, "fd cstring write"));
    close(fd);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_fd_append (NYA_String* overload)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 fd = open("test_fd_append.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    nya_assert(fd >= 0);
    NYA_Error ok = nya_fd_write(fd, "Start");
    nya_assert(ok.error == NYA_ERROR_NONE);

    NYA_String append_str = *nya_string_from(arena, " Middle");
    ok = nya_fd_append(fd, &append_str);
    nya_assert(ok.error == NYA_ERROR_NONE);
    close(fd);

    fd = open("test_fd_append.txt", O_RDONLY);
    nya_assert(fd >= 0);
    NYA_String fd_read_back = *nya_string_create(arena);
    ok = nya_fd_read(fd, &fd_read_back);
    nya_assert(ok.error == NYA_ERROR_NONE);
    nya_assert(nya_string_equals(&fd_read_back, "Start Middle"));
    close(fd);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_fd_append (cstring overload)
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 fd = open("test_fd_append_cstr.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    nya_assert(fd >= 0);
    NYA_Error ok = nya_fd_write(fd, "Base");
    nya_assert(ok.error == NYA_ERROR_NONE);

    ok = nya_fd_append(fd, " Added");
    nya_assert(ok.error == NYA_ERROR_NONE);
    close(fd);

    fd = open("test_fd_append_cstr.txt", O_RDONLY);
    nya_assert(fd >= 0);
    NYA_String fd_read_back = *nya_string_create(arena);
    ok = nya_fd_read(fd, &fd_read_back);
    nya_assert(ok.error == NYA_ERROR_NONE);
    nya_assert(nya_string_equals(&fd_read_back, "Base Added"));
    close(fd);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: nya_fd_read on empty fd
  // ─────────────────────────────────────────────────────────────────────────────
  {
    s32 fd = open("test_fd_empty.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    nya_assert(fd >= 0);
    close(fd);

    fd = open("test_fd_empty.txt", O_RDONLY);
    nya_assert(fd >= 0);
    NYA_String fd_read_back = *nya_string_create(arena);
    NYA_Error ok = nya_fd_read(fd, &fd_read_back);
    nya_assert(ok.error == NYA_ERROR_NONE);
    nya_assert(nya_string_is_empty(&fd_read_back));
    close(fd);
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
