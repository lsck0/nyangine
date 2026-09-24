/**
 * THIS FILE WAS CLANKER WANKED !!!
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Counts what the walk visits, and notices a parent handed over before what is inside it. */
typedef struct {
    u32 visited;
    b8  saw_parent;
    b8  parent_came_first;
} WalkTally;

NYA_INTERNAL b8 _test_walk_tally(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    nya_unused(path);

    WalkTally* tally = user_data;
    tally->visited++;

    if (nya_string_equals(entry->name, "two")) tally->saw_parent = true;
    if (nya_string_equals(entry->name, "deep.txt") && tally->saw_parent) tally->parent_came_first = true;

    return true;
}

/** Stops on the first entry, which is what a callback returning false promises. */
NYA_INTERNAL b8 _test_walk_stop(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    nya_unused(path);
    nya_unused(entry);

    u32* visited  = user_data;
    *visited     += 1;

    return false;
}

s32 main(void) {
    NYA_ConstCString test_file_path       = "test_file.txt";
    NYA_ConstCString test_file_copy_path  = "test_file_copy.txt";
    NYA_ConstCString test_file_moved_path = "test_file_moved.txt";
    NYA_String       file_content         = *nya_string_create(nya_arena_global);
    NYA_String       copied_file_content  = *nya_string_create(nya_arena_global);
    NYA_String       moved_file_content   = *nya_string_create(nya_arena_global);

    NYA_Error result;

    // TEST: Files should not exist initially
    nya_assert(!nya_filesystem_exists(test_file_path));
    nya_assert(!nya_filesystem_exists(test_file_copy_path));
    nya_assert(!nya_filesystem_exists(test_file_moved_path));

    // TEST: Write and read basic file
    NYA_EXPECT(nya_file_write(test_file_path, "Hello, Nyangine!"));
    nya_assert(nya_filesystem_exists(test_file_path));
    NYA_EXPECT(nya_file_read(test_file_path, &file_content));
    nya_assert(nya_string_equals(&file_content, "Hello, Nyangine!"));
    nya_string_clear(&file_content);

    // TEST: Append to file
    NYA_EXPECT(nya_file_append(test_file_path, " Appended text."));
    NYA_EXPECT(nya_file_read(test_file_path, &file_content));
    nya_assert(nya_string_equals(&file_content, "Hello, Nyangine! Appended text."));
    nya_string_clear(&file_content);

    // TEST: Copy file
    NYA_EXPECT(nya_filesystem_copy(test_file_path, "test_file_copy.txt"));
    nya_assert(nya_filesystem_exists(test_file_copy_path));
    NYA_EXPECT(nya_file_read(test_file_copy_path, &copied_file_content));
    nya_assert(nya_string_equals(&copied_file_content, "Hello, Nyangine! Appended text."));

    // TEST: Move/rename file
    NYA_EXPECT(nya_filesystem_move(test_file_copy_path, test_file_moved_path));
    nya_assert(!nya_filesystem_exists(test_file_copy_path));
    nya_assert(nya_filesystem_exists(test_file_moved_path));
    NYA_EXPECT(nya_file_read(test_file_moved_path, &moved_file_content));
    nya_assert(nya_string_equals(&moved_file_content, "Hello, Nyangine! Appended text."));

    // TEST: Delete files
    nya_assert(!nya_filesystem_exists(test_file_copy_path));
    NYA_EXPECT(nya_filesystem_delete(test_file_path));
    nya_assert(!nya_filesystem_exists(test_file_path));
    NYA_EXPECT(nya_filesystem_delete(test_file_moved_path));
    nya_assert(!nya_filesystem_exists(test_file_moved_path));

    // TEST: Non-existent file operations
    nya_assert(!nya_filesystem_exists("nonexistent_file_12345.txt"));

    NYA_String nonexistent_content = *nya_string_create(nya_arena_global);
    NYA_Error  read_result         = nya_file_read("nonexistent_file_12345.txt", &nonexistent_content);
    nya_assert(read_result.kind == NYA_ERROR_NOT_FOUND);

    // TEST: Copy non-existent source fails
    result = nya_filesystem_copy("nonexistent_src_12345.txt", "nonexistent_dst_12345.txt");
    nya_assert(!result.ok);

    // TEST: Move non-existent source fails
    result = nya_filesystem_move("nonexistent_src_12345.txt", "nonexistent_dst_12345.txt");
    nya_assert(!result.ok);

    // TEST: Last modified on non-existent file returns NOT_FOUND
    {
        u64       ts = 0;
        NYA_Error r  = nya_filesystem_last_modified("nonexistent_file_12345.txt", &ts);
        nya_assert(r.kind == NYA_ERROR_NOT_FOUND);
    }

    // TEST: Write empty content
    NYA_ConstCString empty_file_path = "test_empty_file.txt";
    NYA_EXPECT(nya_file_write(empty_file_path, ""));
    nya_assert(nya_filesystem_exists(empty_file_path));
    NYA_String empty_content = *nya_string_create(nya_arena_global);
    NYA_EXPECT(nya_file_read(empty_file_path, &empty_content));
    nya_assert(nya_string_is_empty(&empty_content));
    NYA_EXPECT(nya_filesystem_delete(empty_file_path));

    // TEST: Write and read binary-like content
    NYA_ConstCString binary_file_path = "test_binary_file.txt";
    NYA_EXPECT(nya_file_write(binary_file_path, "Line1\nLine2\nLine3\tTabbed"));
    NYA_String binary_content = *nya_string_create(nya_arena_global);
    NYA_EXPECT(nya_file_read(binary_file_path, &binary_content));
    nya_assert(nya_string_contains(&binary_content, "Line1"));
    nya_assert(nya_string_contains(&binary_content, "\n"));
    nya_assert(nya_string_contains(&binary_content, "\t"));
    NYA_EXPECT(nya_filesystem_delete(binary_file_path));

    // TEST: Overwrite existing file
    NYA_ConstCString overwrite_path = "test_overwrite.txt";
    NYA_EXPECT(nya_file_write(overwrite_path, "Original content"));
    NYA_EXPECT(nya_file_write(overwrite_path, "New content"));
    NYA_String overwrite_content = *nya_string_create(nya_arena_global);
    NYA_EXPECT(nya_file_read(overwrite_path, &overwrite_content));
    nya_assert(nya_string_equals(&overwrite_content, "New content"));
    NYA_EXPECT(nya_filesystem_delete(overwrite_path));

    // TEST: Multiple appends
    NYA_ConstCString append_path = "test_multi_append.txt";
    NYA_EXPECT(nya_file_write(append_path, "Start"));
    NYA_EXPECT(nya_file_append(append_path, " Middle"));
    NYA_EXPECT(nya_file_append(append_path, " End"));
    NYA_String append_content = *nya_string_create(nya_arena_global);
    NYA_EXPECT(nya_file_read(append_path, &append_content));
    nya_assert(nya_string_equals(&append_content, "Start Middle End"));
    NYA_EXPECT(nya_filesystem_delete(append_path));

    // TEST: Last modified timestamp
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
    result            = nya_filesystem_last_modified("nonexistent_12345.txt", &bad_timestamp);
    nya_assert(!result.ok);

    // TEST: Large file content
    NYA_ConstCString large_file_path = "test_large_file.txt";
    NYA_String       large_content   = *nya_string_create_with_capacity(nya_arena_global, 40000);
    for (u32 i = 0; i < 1000; ++i) { nya_string_extend(&large_content, "Line of content with some data. "); }
    NYA_EXPECT(nya_file_write(large_file_path, nya_string_to_cstring(nya_arena_global, &large_content)));

    NYA_String read_large = *nya_string_create(nya_arena_global);
    NYA_EXPECT(nya_file_read(large_file_path, &read_large));
    nya_assert(read_large.length == large_content.length);
    NYA_EXPECT(nya_filesystem_delete(large_file_path));

    /* Everything the two per-target implementations used to be each other's only caller of. These ran on one host at a time before; now there is one implementation over os_file, so one test covers both. */

    NYA_Arena* arena = nya_arena_create(.name = "test_filesystem");
    defer      nya_arena_destroy(arena);

    // A handle knows where it is, and truncating moves the end without moving the handle.
    {
        NYA_ConstCString path = "test_handle.bin";
        NYA_EXPECT(nya_file_write(path, "0123456789"));

        u64 size = 0;
        NYA_EXPECT(nya_filesystem_size(path, &size));
        nya_check(size == 10, "the file should be 10 bytes, got " FMTu64, size);

        NYA_File file = { 0 };
        NYA_EXPECT(nya_file_open(path, NYA_FILE_MODE_READ | NYA_FILE_MODE_WRITE, &file));

        u64 offset = 1;
        NYA_EXPECT(nya_file_tell(&file, &offset));
        nya_check(offset == 0, "a fresh handle should sit at the start, got " FMTu64, offset);

        u8  buffer[4] = { 0 };
        u64 got       = 0;
        NYA_EXPECT(nya_file_read_bytes(&file, buffer, sizeof(buffer), &got));
        NYA_EXPECT(nya_file_tell(&file, &offset));
        nya_check(got == 4 && offset == 4, "reading 4 bytes should leave the handle at 4, got " FMTu64 " at " FMTu64, got, offset);

        NYA_EXPECT(nya_file_seek(&file, -1, NYA_FILE_SEEK_END));
        NYA_EXPECT(nya_file_tell(&file, &offset));
        nya_check(offset == 9, "seeking one back from the end should sit at 9, got " FMTu64, offset);

        NYA_EXPECT(nya_file_truncate(&file, 5));
        NYA_EXPECT(nya_file_flush(&file));
        nya_file_close(&file);
        nya_check(!nya_file_is_open(&file), "a closed file should not report as open");

        NYA_EXPECT(nya_filesystem_size(path, &size));
        nya_check(size == 5, "truncating to 5 should leave 5 bytes, got " FMTu64, size);

        // exclusive refuses what is already there, which is what the atomic write's temp name relies on.
        NYA_File  existing = { 0 };
        NYA_Error refused  = nya_file_open(path, NYA_FILE_MODE_WRITE | NYA_FILE_MODE_EXCLUSIVE, &existing);
        nya_check(
            refused.kind == NYA_ERROR_ALREADY_EXISTS,
            "an exclusive open of an existing file should report ALREADY_EXISTS, got %s",
            NYA_ERRORKIND_NAME_MAP[refused.kind]
        );

        NYA_EXPECT(nya_filesystem_delete(path));
    }

    // A tree is created, listed, walked, copied and deleted whole.
    {
        NYA_ConstCString root = "test_tree/one/two";
        NYA_EXPECT(nya_filesystem_create_directory(root));
        nya_check(nya_filesystem_is_directory(root), "creating a path should create its missing parents too");
        // already there is not a failure, which is what makes this usable as `mkdir -p`.
        NYA_EXPECT(nya_filesystem_create_directory(root));

        NYA_EXPECT(nya_file_write("test_tree/one/leaf.txt", "leaf"));
        NYA_EXPECT(nya_file_write("test_tree/one/two/deep.txt", "deep"));

        NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
        NYA_EXPECT(nya_filesystem_list(arena, "test_tree/one", &entries));
        nya_check(entries->length == 2, "the listing should hold the file and the directory, got " FMTu64, entries->length);

        // the metadata comes with the listing, whichever host filled it in.
        nya_array_foreach (entries, entry) {
            if (nya_string_equals(entry->name, "leaf.txt")) {
                nya_check(
                    entry->type == NYA_FILE_TYPE_FILE && entry->size == 4,
                    "leaf.txt should be listed as a 4 byte file, got %s of " FMTu64,
                    NYA_FILETYPE_NAME_MAP[entry->type],
                    entry->size
                );
                nya_check(entry->modified_at > 0, "a listed entry should carry when it was modified");
            } else {
                nya_check(entry->type == NYA_FILE_TYPE_DIRECTORY, "two should be listed as a directory, got %s", NYA_FILETYPE_NAME_MAP[entry->type]);
            }
        }

        WalkTally tally = { 0 };
        NYA_EXPECT(nya_filesystem_walk(arena, "test_tree", _test_walk_tally, &tally));
        nya_check(tally.visited == 4, "the walk should visit both directories and both files, got %u", tally.visited);
        // depth first: what is inside "two" is handed over before "two" itself, which is what lets a caller delete as it goes.
        nya_check(!tally.parent_came_first, "children should be visited before their parent");

        u32 stopped = 0;
        NYA_EXPECT(nya_filesystem_walk(arena, "test_tree", _test_walk_stop, &stopped));
        nya_check(stopped == 1, "a callback returning false should stop the walk at once, got %u visits", stopped);

        NYA_EXPECT(nya_filesystem_copy_recursive("test_tree", "test_tree_copy"));
        NYA_String* copied = nya_string_create(arena);
        NYA_EXPECT(nya_file_read("test_tree_copy/one/two/deep.txt", copied));
        nya_check(nya_string_equals(copied, "deep"), "a tree copy should carry the contents of the deepest file");

        NYA_EXPECT(nya_filesystem_delete_recursive("test_tree"));
        NYA_EXPECT(nya_filesystem_delete_recursive("test_tree_copy"));
        nya_check(!nya_filesystem_exists("test_tree") && !nya_filesystem_exists("test_tree_copy"), "a recursive delete should leave nothing behind");
    }

    // What one stat knows, and what a resolved path looks like.
    {
        NYA_ConstCString path = "test_info.txt";
        NYA_EXPECT(nya_file_write(path, "info"));

        NYA_FileInfo info = { 0 };
        NYA_EXPECT(nya_filesystem_info(path, &info));
        nya_check(info.type == NYA_FILE_TYPE_FILE, "a plain file should stat as a file, got %s", NYA_FILETYPE_NAME_MAP[info.type]);
        nya_check(info.size == 4, "the file should stat as 4 bytes, got " FMTu64, info.size);
        nya_check(!info.readonly, "a file this process just wrote should not stat as read only");
        nya_check(info.modified_at > 0 && info.modified_at <= nya_clock_get_timestamp_ms(), "the modification time should not be in the future");

        NYA_String* absolute = nullptr;
        NYA_EXPECT(nya_filesystem_absolute(arena, path, &absolute));
        nya_check(
            nya_path_is_absolute(nya_string_to_cstring(arena, absolute)),
            "a resolved path should be absolute, got %s",
            nya_string_to_cstring(arena, absolute)
        );
        nya_check(nya_string_ends_with(absolute, path), "a resolved path should still end in the name it was given");

        NYA_Error missing = nya_filesystem_info("test_info_missing.txt", &info);
        nya_check(
            missing.kind == NYA_ERROR_NOT_FOUND,
            "stat'ing what is not there should report NOT_FOUND, got %s",
            NYA_ERRORKIND_NAME_MAP[missing.kind]
        );

        NYA_EXPECT(nya_filesystem_delete(path));
    }

    // A replace lands in one step and keeps the permissions of what it replaced.
    {
        NYA_ConstCString target = "test_replace.txt";
        NYA_ConstCString staged = "test_replace.txt.new";
        NYA_EXPECT(nya_file_write(target, "old"));
        NYA_EXPECT(nya_file_write(staged, "new"));

        NYA_EXPECT(nya_filesystem_replace(staged, target));
        nya_check(!nya_filesystem_exists(staged), "the replaced file should be gone from under its staging name");

        NYA_String* content = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(target, content));
        nya_check(nya_string_equals(content, "new"), "the replace should have left the new contents");

        NYA_EXPECT(nya_filesystem_delete(target));
    }

    // The working directory moves and comes back, and the well known locations answer.
    {
        NYA_String* before = nullptr;
        NYA_EXPECT(nya_filesystem_working_directory(arena, &before));
        nya_check(nya_path_is_absolute(nya_string_to_cstring(arena, before)), "the working directory should be absolute");

        NYA_EXPECT(nya_filesystem_create_directory("test_cwd"));
        NYA_EXPECT(nya_filesystem_working_directory_set("test_cwd"));

        NYA_String* inside = nullptr;
        NYA_EXPECT(nya_filesystem_working_directory(arena, &inside));
        nya_check(
            nya_string_ends_with(inside, "test_cwd"),
            "the working directory should be the one just set, got %s",
            nya_string_to_cstring(arena, inside)
        );

        NYA_EXPECT(nya_filesystem_working_directory_set(nya_string_to_cstring(arena, before)));
        NYA_EXPECT(nya_filesystem_delete_recursive("test_cwd"));

        NYA_Error refused = nya_filesystem_working_directory_set("test_cwd_missing");
        nya_check(!refused.ok, "moving into a directory that is not there should fail");

        NYA_String* temp = nullptr;
        NYA_EXPECT(nya_filesystem_temp_directory(arena, &temp));
        nya_check(nya_filesystem_is_directory(nya_string_to_cstring(arena, temp)), "the temp directory should exist");

        NYA_String* executable = nullptr;
        NYA_EXPECT(nya_filesystem_executable_path(arena, &executable));
        nya_check(
            nya_filesystem_is_file(nya_string_to_cstring(arena, executable)),
            "the executable path should name a file, got %s",
            nya_string_to_cstring(arena, executable)
        );

        NYA_String* user_data = nullptr;
        NYA_EXPECT(nya_filesystem_user_data_directory(arena, "nyangine_test", &user_data));
        nya_check(
            nya_string_ends_with(user_data, "nyangine_test"),
            "the user data directory should end in the application name, got %s",
            nya_string_to_cstring(arena, user_data)
        );
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
