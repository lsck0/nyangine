/**
 * The content-addressed blob store: an object is keyed by the SHA-256 of its bytes, so a put
 * deduplicates, a get verifies, and a corrupted row is refused.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Opens a fresh in-memory database. Each test gets its own, so none can pollute another. */
static NYA_Database* open_memory(NYA_Arena* arena) {
  NYA_Database* db = nullptr;
  NYA_EXPECT(nya_sql_open(arena, ":memory:", &db));
  return db;
}

/** Counts objects, for the list walk. */
static b8 count_visitor(NYA_BlobId id, u64 size, u64 created, void* user_data) {
  (void)id;
  (void)size;
  (void)created;
  *(u32*)user_data += 1;
  return true;
}

/** Stops after the first object, to prove a visitor can end the walk early. */
static b8 stop_visitor(NYA_BlobId id, u64 size, u64 created, void* user_data) {
  (void)id;
  (void)size;
  (void)created;
  *(u32*)user_data += 1;
  return false;
}

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_blob");
  defer      nya_arena_destroy(arena);

  // TEST: a put→get round trip returns identical bytes, and the id is the SHA-256
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_BlobStore* store = nullptr;
    NYA_EXPECT(nya_blob_store_open(arena, db, &store));
    defer nya_blob_store_close(store);

    const u8 bytes[] = { 0x00, 0x01, 0xFE, 0xFF, 0x42, 'n', 'y', 'a' };

    NYA_BlobId id = { 0 };
    NYA_EXPECT(nya_blob_put(store, bytes, sizeof(bytes), &id));

    // The id is the object's SHA-256 as 64 lower case hex digits, computed independently here.
    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256(bytes, sizeof(bytes), &digest);
    char expected[NYA_BLOB_ID_LENGTH + 1] = { 0 };
    NYA_ConstCString hex = "0123456789abcdef";
    for (u32 i = 0; i < NYA_CRYPTO_SHA256_BYTES; i++) {
      expected[i * 2]     = hex[digest.bytes[i] >> 4];
      expected[i * 2 + 1] = hex[digest.bytes[i] & 0x0F];
    }
    nya_assert(strcmp(id.hex, expected) == 0, "id %s is not the sha256 %s", id.hex, expected);

    u8* out      = nullptr;
    u64 out_size = 0;
    NYA_EXPECT(nya_blob_get(store, id, arena, &out, &out_size));
    nya_assert(out_size == sizeof(bytes), "got " FMTu64 " bytes", out_size);
    nya_assert(memcmp(out, bytes, sizeof(bytes)) == 0, "the bytes survived, zero byte included");
  }

  // TEST: putting the same bytes twice is one row and the same id (dedup)
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_BlobStore* store = nullptr;
    NYA_EXPECT(nya_blob_store_open(arena, db, &store));
    defer nya_blob_store_close(store);

    const u8 bytes[] = { 1, 2, 3, 4, 5 };

    NYA_BlobId first  = { 0 };
    NYA_BlobId second = { 0 };
    NYA_EXPECT(nya_blob_put(store, bytes, sizeof(bytes), &first));
    NYA_EXPECT(nya_blob_put(store, bytes, sizeof(bytes), &second));

    nya_assert(strcmp(first.hex, second.hex) == 0, "identical bytes hash to one id");

    u32 count = 0;
    NYA_EXPECT(nya_blob_list(store, count_visitor, &count));
    nya_assert(count == 1, "two puts of the same object left " FMTu32 " rows, not 1", count);
  }

  // TEST: distinct bytes are distinct objects, and list walks them all
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_BlobStore* store = nullptr;
    NYA_EXPECT(nya_blob_store_open(arena, db, &store));
    defer nya_blob_store_close(store);

    const u8 a[] = { 'a' };
    const u8 b[] = { 'b' };
    const u8 c[] = { 'c' };

    NYA_BlobId id_a = { 0 }, id_b = { 0 }, id_c = { 0 };
    NYA_EXPECT(nya_blob_put(store, a, sizeof(a), &id_a));
    NYA_EXPECT(nya_blob_put(store, b, sizeof(b), &id_b));
    NYA_EXPECT(nya_blob_put(store, c, sizeof(c), &id_c));

    u32 count = 0;
    NYA_EXPECT(nya_blob_list(store, count_visitor, &count));
    nya_assert(count == 3, "three distinct objects, got " FMTu32, count);

    // A visitor that returns false ends the walk after the first object.
    u32 stopped = 0;
    NYA_EXPECT(nya_blob_list(store, stop_visitor, &stopped));
    nya_assert(stopped == 1, "the walk did not stop early, saw " FMTu32, stopped);

    u64 size = 0;
    NYA_EXPECT(nya_blob_size(store, id_b, &size));
    nya_assert(size == sizeof(b), "size reported " FMTu64, size);
  }

  // TEST: has, and get of an unknown id fails cleanly
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_BlobStore* store = nullptr;
    NYA_EXPECT(nya_blob_store_open(arena, db, &store));
    defer nya_blob_store_close(store);

    const u8   bytes[] = { 9, 9, 9 };
    NYA_BlobId id      = { 0 };
    NYA_EXPECT(nya_blob_put(store, bytes, sizeof(bytes), &id));

    nya_assert(nya_blob_has(store, id), "the object just put is there");

    // An id of the right shape that names nothing.
    NYA_BlobId absent = { 0 };
    NYA_EXPECT(nya_blob_id_parse("0000000000000000000000000000000000000000000000000000000000000000", &absent));

    nya_assert(!nya_blob_has(store, absent), "an id that names nothing is not there");

    u8*       out      = (u8*)1;
    u64       out_size = 7;
    NYA_Error missing  = nya_blob_get(store, absent, arena, &out, &out_size);
    nya_assert(missing.kind == NYA_ERROR_NOT_FOUND, "get of an unknown id is NOT_FOUND");
    nya_assert(out == nullptr && out_size == 0, "and it clears the out parameters");

    u64       ignored = 0;
    NYA_Error no_size = nya_blob_size(store, absent, &ignored);
    nya_assert(no_size.kind == NYA_ERROR_NOT_FOUND, "size of an unknown id is NOT_FOUND");

    NYA_Error no_delete = nya_blob_delete(store, absent);
    nya_assert(no_delete.kind == NYA_ERROR_NOT_FOUND, "delete of an unknown id is NOT_FOUND");
  }

  // TEST: delete removes an object
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_BlobStore* store = nullptr;
    NYA_EXPECT(nya_blob_store_open(arena, db, &store));
    defer nya_blob_store_close(store);

    const u8   bytes[] = { 't', 'e', 'm', 'p' };
    NYA_BlobId id      = { 0 };
    NYA_EXPECT(nya_blob_put(store, bytes, sizeof(bytes), &id));
    nya_assert(nya_blob_has(store, id));

    NYA_EXPECT(nya_blob_delete(store, id));
    nya_assert(!nya_blob_has(store, id), "delete left the object behind");

    u8* out      = nullptr;
    u64 out_size = 0;
    NYA_Error gone = nya_blob_get(store, id, arena, &out, &out_size);
    nya_assert(gone.kind == NYA_ERROR_NOT_FOUND, "a deleted object is gone from get too");
  }

  // TEST: an empty object is valid and round-trips
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_BlobStore* store = nullptr;
    NYA_EXPECT(nya_blob_store_open(arena, db, &store));
    defer nya_blob_store_close(store);

    NYA_BlobId id = { 0 };
    NYA_EXPECT(nya_blob_put(store, nullptr, 0, &id));

    // The SHA-256 of the empty string, which is what an empty object's id has to be.
    nya_assert(strcmp(id.hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0, "empty object id is %s", id.hex);

    u8* out      = nullptr;
    u64 out_size = 7;
    NYA_EXPECT(nya_blob_get(store, id, arena, &out, &out_size));
    nya_assert(out_size == 0, "empty object has size 0, got " FMTu64, out_size);
    nya_assert(out != nullptr, "and still a non-null pointer, so it is told apart from missing");
  }

  // TEST: get refuses an object whose stored bytes were tampered with
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_BlobStore* store = nullptr;
    NYA_EXPECT(nya_blob_store_open(arena, db, &store));
    defer nya_blob_store_close(store);

    const u8   bytes[] = { 'h', 'o', 'n', 'e', 's', 't' };
    NYA_BlobId id      = { 0 };
    NYA_EXPECT(nya_blob_put(store, bytes, sizeof(bytes), &id));

    // Flip the stored bytes out from under the store with raw SQL, leaving the id — the row's key — exactly as it was. This is a bit rot or a row edited around the store.
    const u8     tampered[] = { 'f', 'o', 'r', 'g', 'e', 'd' };
    NYA_SqlValue update[]   = { nya_sql_blob(tampered, sizeof(tampered)), nya_sql_text(id.hex) };
    NYA_EXPECT(nya_sql_exec_bound(db, "UPDATE blobs SET data = ? WHERE id = ?", update, 2));

    u8*       out      = nullptr;
    u64       out_size = 0;
    NYA_Error refused  = nya_blob_get(store, id, arena, &out, &out_size);
    nya_assert(refused.kind == NYA_ERROR_CORRUPT, "get served bytes that no longer hash to their id");
    nya_assert(out == nullptr && out_size == 0, "and returned nothing");
  }

  // TEST: nya_blob_id_parse validates the shape, so no unchecked id reaches a statement
  {
    NYA_BlobId id = { 0 };

    NYA_EXPECT(nya_blob_id_parse("abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", &id));

    nya_assert(nya_blob_id_parse("tooshort", &id).kind == NYA_ERROR_INVALID_ARGUMENT, "a short id is refused");
    nya_assert(nya_blob_id_parse(nullptr, &id).kind == NYA_ERROR_INVALID_ARGUMENT, "a null id is refused");
    // Upper case is a second spelling, so it is refused: an id has exactly one.
    nya_assert(nya_blob_id_parse("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789", &id).kind == NYA_ERROR_INVALID_ARGUMENT, "upper case hex is refused");
    // A non-hex character.
    nya_assert(nya_blob_id_parse("zzzzef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", &id).kind == NYA_ERROR_INVALID_ARGUMENT, "a non-hex digit is refused");
  }

  // TEST: an oversized object and a bad table name are refused
  {
    NYA_Database* db = open_memory(arena);
    defer         nya_sql_close(db);

    NYA_BlobStore* store = nullptr;
    NYA_EXPECT(nya_blob_store_open(arena, db, &store));
    defer nya_blob_store_close(store);

    // A size over the bound is refused before anything is hashed or written; the pointer is never read.
    NYA_BlobId id      = { 0 };
    NYA_Error  too_big = nya_blob_put(store, (const u8*)"", NYA_BLOB_MAX_SIZE + 1, &id);
    nya_assert(too_big.kind == NYA_ERROR_INVALID_ARGUMENT, "an object over the limit is refused");

    // A table name that is not an identifier cannot be interpolated safely, so open refuses it.
    NYA_BlobStore* bad     = nullptr;
    NYA_Error      injection = nya_blob_store_open(arena, db, &bad, .table = "blobs; DROP TABLE blobs;--");
    nya_assert(injection.kind == NYA_ERROR_INVALID_ARGUMENT, "a non-identifier table name is refused");
  }

  // TEST: the store survives close and reopen on a file (persistence + custom table)
  {
    NYA_ConstCString path = "./_test_blob_persist.db";
    (void)remove(path);
    defer (void)remove(path);

    NYA_BlobId id = { 0 };

    {
      NYA_Database* db = nullptr;
      NYA_EXPECT(nya_sql_open(arena, path, &db));
      defer nya_sql_close(db);

      NYA_BlobStore* store = nullptr;
      NYA_EXPECT(nya_blob_store_open(arena, db, &store, .table = "attachments"));
      defer nya_blob_store_close(store);

      const u8 bytes[] = { 'p', 'e', 'r', 's', 'i', 's', 't' };
      NYA_EXPECT(nya_blob_put(store, bytes, sizeof(bytes), &id));
    }

    {
      NYA_Database* db = nullptr;
      NYA_EXPECT(nya_sql_open(arena, path, &db));
      defer nya_sql_close(db);

      // Reopening the same table finds the migration already applied and the object still there.
      NYA_BlobStore* store = nullptr;
      NYA_EXPECT(nya_blob_store_open(arena, db, &store, .table = "attachments"));
      defer nya_blob_store_close(store);

      nya_assert(nya_blob_has(store, id), "the object the previous connection stored is gone");

      u8* out      = nullptr;
      u64 out_size = 0;
      NYA_EXPECT(nya_blob_get(store, id, arena, &out, &out_size));
      nya_assert(out_size == 7 && memcmp(out, "persist", 7) == 0, "and its bytes survived the reopen");
    }
  }

  printf("PASSED: test_db_blob\n");
  return 0;
}
