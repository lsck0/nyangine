/**
 * The baked asset index, as a shipping build sees it.
 *
 * NYA_ASSET_PREFER_BLOB is only set for release builds, so every other test compiles the branch that
 * answers "no blob". Defining it here, before the engine, is what gets the other branch compiled and
 * run at all: the index, the lookup by path and the enumeration all read the generated table.
 **/

#define NYA_ASSET_PREFER_BLOB

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_asset_blob");
  defer nya_arena_destroy(arena);

  const u64 count = nya_asset_blob_count();

  // TEST: the index is there, and every entry names itself the way a lookup spells it
  {
    nya_check(count > 0, "a build with the blob has entries in it");

    for (u64 i = 0; i < count; i++) {
      const NYA_AssetBlobHeader* entry = nya_asset_blob_at(i);

      nya_check(entry != nullptr, "entry " FMTu64 " of " FMTu64 " is there", i, count);
      nya_check(nya_string_starts_with(entry->path, "./assets/"), "and is spelled from the root, got '%s'", entry->path);
      nya_check(entry->data != nullptr || entry->size == 0, "and has its bytes, '%s'", entry->path);
    }

    nya_check(nya_asset_blob_at(count) == nullptr, "past the end is nothing rather than the next thing in memory");

    printf("  PASSED\n");
  }

  // TEST: a lookup by path finds the entry the index holds, and only a real path
  {
    for (u64 i = 0; i < count; i += nya_max(count / 16, (u64)1)) {
      const NYA_AssetBlobHeader* entry = nya_asset_blob_at(i);
      nya_check(nya_asset_blob_find(entry->path) == entry, "'%s' finds itself", entry->path);
    }

    nya_check(nya_asset_blob_find("./assets/there_is_no_such_file.png") == nullptr, "a path nothing baked finds nothing");
    nya_check(nya_asset_blob_find(nullptr) == nullptr, "and so does no path");

    printf("  PASSED\n");
  }

  // TEST: enumeration lists the baked paths, sorted, filtered by suffix
  {
    NYA_ArrayᐸNYA_Stringᐳ* all = nya_asset_enumerate(arena, nullptr);
    nya_check(all->length == count, "unfiltered, it lists every entry, " FMTu64 " against " FMTu64, (u64)all->length, count);

    NYA_ArrayᐸNYA_Stringᐳ* images = nya_asset_enumerate(arena, ".png");
    nya_check(images->length > 0 && images->length < count, "a suffix narrows it, got " FMTu64, (u64)images->length);

    for (u64 i = 0; i < images->length; i++) {
      NYA_String* path = &images->items[i];

      nya_check(nya_string_ends_with(path, ".png"), "and only to what it names");
      nya_check(nya_asset_blob_find(nya_string_to_cstring(arena, path)) != nullptr, "each of which is in the index");
      if (i > 0) nya_check(strcmp((const char*)images->items[i - 1].items, (const char*)path->items) < 0, "in order, with no repeats");
    }

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_asset_blob");

  return nya_check_failures() == 0 ? 0 : 1;
}
