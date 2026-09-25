#include "nyangine-core/nyangine.h"

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

struct NYA_BlobStore {
    NYA_Database* database;
    NYA_Arena*    arena;

    /** Validated at open, so it is interpolated into a statement's table name as a checked identifier. */
    char table[NYA_BLOB_TABLE_MAX];
};

/** The longest statement any operation builds: a fixed template plus one interpolated table name. */
#define NYA_BLOB_SQL_MAX 256

/** Whether `name` is an identifier SQLite can carry as a table name: [A-Za-z_][A-Za-z0-9_]*, bounded. */
NYA_INTERNAL b8 _nya_blob_table_valid(NYA_ConstCString name) __attr_no_discard;

/** Writes a digest as NYA_BLOB_ID_LENGTH lower case hex digits and a terminator into `out_id`. */
NYA_INTERNAL void _nya_blob_id_from_digest(const NYA_CryptoSha256Digest* digest, OUT NYA_BlobId* out_id);

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_Error nya_blob_store_open_with_options(NYA_Arena* arena, NYA_Database* database, NYA_BlobStoreOptions options, OUT NYA_BlobStore** out_store) {
    nya_assert(arena != nullptr);
    nya_assert(database != nullptr);
    nya_assert(out_store != nullptr);

    *out_store = nullptr;

    NYA_ConstCString table = options.table != nullptr ? options.table : NYA_BLOB_TABLE_DEFAULT;
    if (!_nya_blob_table_valid(table)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a table identifier", table);

    NYA_BlobStore* store = nya_arena_alloc(arena, sizeof(NYA_BlobStore));
    *store               = (NYA_BlobStore){ .database = database, .arena = arena };
    (void)snprintf(store->table, sizeof(store->table), "%s", table);

    // WITHOUT ROWID: the id is the key. The schema is fixed, so IF NOT EXISTS is the whole migration: no columns for a later build to grow.
    char sql[NYA_BLOB_SQL_MAX];
    (void)snprintf(
        sql,
        sizeof(sql),
        "CREATE TABLE IF NOT EXISTS %s ("
        "id TEXT PRIMARY KEY NOT NULL, "
        "size INTEGER NOT NULL, "
        "created INTEGER NOT NULL, "
        "data BLOB NOT NULL"
        ") WITHOUT ROWID",
        store->table
    );
    NYA_TRY(nya_sql_exec(database, sql));

    *out_store = store;
    return NYA_OK;
}

void nya_blob_store_close(NYA_BlobStore* store) {
    // Nothing to free (arena memory, caller's connection); the handle is cleared so a use after close trips rather than reads a live database.
    if (store == nullptr) return;
    *store = (NYA_BlobStore){ 0 };
}

NYA_Error nya_blob_put(NYA_BlobStore* store, const u8* bytes, u64 size, OUT NYA_BlobId* out_id) {
    nya_assert(store != nullptr);
    nya_assert(store->database != nullptr, "put on a closed store");
    nya_assert(out_id != nullptr);
    nya_assert(bytes != nullptr || size == 0, "null bytes with a non-zero size");

    // Checked before anything is hashed or written, so the bound is spent on nothing.
    if (size > NYA_BLOB_MAX_SIZE) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "object of " FMTu64 " bytes is over the " FMTu64 " byte limit", size, (u64)NYA_BLOB_MAX_SIZE);

    // The id is the content, computed here. A caller never says what an object's id is.
    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256(bytes, size, &digest);
    _nya_blob_id_from_digest(&digest, out_id);

    u64 created = nya_os_time_wall_ns() / 1000000000ULL;

    // INSERT OR IGNORE is the whole dedup: an id already present is a primary-key conflict, so a repeat writes no row and returns the same id.
    char sql[NYA_BLOB_SQL_MAX];
    (void)snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO %s (id, size, created, data) VALUES (?, ?, ?, ?)", store->table);

    // A zero-length object binds a non-null pointer so SQLite stores an empty blob rather than the forbidden NULL: an empty object still has an id.
    const u8*    data     = bytes != nullptr ? bytes : (const u8*)"";
    NYA_SqlValue values[] = { nya_sql_text(out_id->hex), nya_sql_s64((s64)size), nya_sql_s64((s64)created), nya_sql_blob(data, size) };
    NYA_TRY(nya_sql_exec_bound(store->database, sql, values, 4));

    return NYA_OK;
}

NYA_Error nya_blob_get(NYA_BlobStore* store, NYA_BlobId id, NYA_Arena* out_arena, OUT u8** out_bytes, OUT u64* out_size) {
    nya_assert(store != nullptr);
    nya_assert(store->database != nullptr, "get on a closed store");
    nya_assert(out_arena != nullptr);
    nya_assert(out_bytes != nullptr);
    nya_assert(out_size != nullptr);

    *out_bytes = nullptr;
    *out_size  = 0;

    // Row and base64 blob (see db_sql.c) live in a scratch arena that dies with the call, so the caller's arena holds only the object's bytes.
    NYA_Arena* scratch = nya_arena_create(.name = "blob_get");
    defer      nya_arena_destroy(scratch);

    char sql[NYA_BLOB_SQL_MAX];
    (void)snprintf(sql, sizeof(sql), "SELECT data FROM %s WHERE id = ?", store->table);

    NYA_SqlValue  key[]  = { nya_sql_text(id.hex) };
    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(store->database, scratch, sql, key, 1, &result));

    if (result.rows->length == 0) return nya_error(NYA_ERROR_NOT_FOUND, "no object %s", id.hex);

    NYA_Value* data = nya_object_get(result.rows->items[0], "data");
    nya_assert(data != nullptr && data->type == NYA_TYPE_STRING, "a blob column arrives as base64 text");

    NYA_String* decoded = nya_string_create(scratch);
    nya_base64_decode(decoded, (const u8*)data->as_string, strlen(data->as_string));

    // The integrity check: the bytes are rehashed and refused if they no longer hash to the id asked for, catching a flipped bit or a row edited around the store.
    NYA_CryptoSha256Digest digest = { 0 };
    nya_crypto_sha256(decoded->items, decoded->length, &digest);

    NYA_BlobId actual = { 0 };
    _nya_blob_id_from_digest(&digest, &actual);
    if (memcmp(actual.hex, id.hex, NYA_BLOB_ID_LENGTH) != 0) return nya_error(NYA_ERROR_CORRUPT, "object %s hashes to %s", id.hex, actual.hex);

    // Copied into the caller's arena before the scratch dies; a zero-length object still gets a non-null pointer, so empty is told from missing by the error.
    u8* bytes = nya_arena_alloc(out_arena, decoded->length > 0 ? decoded->length : 1);
    memcpy(bytes, decoded->items, decoded->length);

    *out_bytes = bytes;
    *out_size  = decoded->length;
    return NYA_OK;
}

b8 nya_blob_has(NYA_BlobStore* store, NYA_BlobId id) {
    nya_assert(store != nullptr);
    nya_assert(store->database != nullptr, "has on a closed store");

    NYA_Arena* scratch = nya_arena_create(.name = "blob_has");
    defer      nya_arena_destroy(scratch);

    char sql[NYA_BLOB_SQL_MAX];
    (void)snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE id = ? LIMIT 1", store->table);

    NYA_SqlValue  key[]  = { nya_sql_text(id.hex) };
    NYA_SqlResult result = { 0 };

    // A read error answers false rather than propagating: for a store that cannot be read, the only honest no is "not that I can see".
    NYA_TRY_OR(nya_sql_query(store->database, scratch, sql, key, 1, &result), false);

    return result.rows->length > 0;
}

NYA_Error nya_blob_size(NYA_BlobStore* store, NYA_BlobId id, OUT u64* out_size) {
    nya_assert(store != nullptr);
    nya_assert(store->database != nullptr, "size on a closed store");
    nya_assert(out_size != nullptr);

    *out_size = 0;

    NYA_Arena* scratch = nya_arena_create(.name = "blob_size");
    defer      nya_arena_destroy(scratch);

    // The size column is why this reads no blob: the answer is one integer beside the bytes.
    char sql[NYA_BLOB_SQL_MAX];
    (void)snprintf(sql, sizeof(sql), "SELECT size FROM %s WHERE id = ?", store->table);

    NYA_SqlValue  key[]  = { nya_sql_text(id.hex) };
    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(store->database, scratch, sql, key, 1, &result));

    if (result.rows->length == 0) return nya_error(NYA_ERROR_NOT_FOUND, "no object %s", id.hex);

    *out_size = (u64)nya_object_get(result.rows->items[0], "size")->as_s64;
    return NYA_OK;
}

NYA_Error nya_blob_delete(NYA_BlobStore* store, NYA_BlobId id) {
    nya_assert(store != nullptr);
    nya_assert(store->database != nullptr, "delete on a closed store");

    NYA_Arena* scratch = nya_arena_create(.name = "blob_delete");
    defer      nya_arena_destroy(scratch);

    char sql[NYA_BLOB_SQL_MAX];
    (void)snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE id = ?", store->table);

    // Run through query, not exec, because the row count is the whole answer: a delete that dropped an object versus one that found none.
    NYA_SqlValue  key[]  = { nya_sql_text(id.hex) };
    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(store->database, scratch, sql, key, 1, &result));

    if (result.rows_affected == 0) return nya_error(NYA_ERROR_NOT_FOUND, "no object %s", id.hex);
    return NYA_OK;
}

NYA_Error nya_blob_list(NYA_BlobStore* store, NYA_BlobVisitor visitor, void* user_data) {
    nya_assert(store != nullptr);
    nya_assert(store->database != nullptr, "list on a closed store");
    nya_assert(visitor != nullptr);

    NYA_Arena* scratch = nya_arena_create(.name = "blob_list");
    defer      nya_arena_destroy(scratch);

    // In id order, so a walk repeats and a caller can page by the last id it saw. No blob is read: a listing is identities and sizes, never bytes.
    char sql[NYA_BLOB_SQL_MAX];
    (void)snprintf(sql, sizeof(sql), "SELECT id, size, created FROM %s ORDER BY id", store->table);

    NYA_SqlResult result = { 0 };
    NYA_TRY(nya_sql_query(store->database, scratch, sql, nullptr, 0, &result));

    nya_array_foreach (result.rows, row) {
        NYA_ConstCString hex = nya_object_get(*row, "id")->as_string;

        NYA_BlobId id = { 0 };
        NYA_TRY(nya_blob_id_parse(hex, &id));

        u64 size    = (u64)nya_object_get(*row, "size")->as_s64;
        u64 created = (u64)nya_object_get(*row, "created")->as_s64;

        if (!visitor(id, size, created, user_data)) break;
    }

    return NYA_OK;
}

NYA_Error nya_blob_id_parse(NYA_ConstCString hex, OUT NYA_BlobId* out_id) {
    nya_assert(out_id != nullptr);

    *out_id = (NYA_BlobId){ 0 };

    if (hex == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a null id");
    if (strlen(hex) != NYA_BLOB_ID_LENGTH) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an id is %d hex digits", (s32)NYA_BLOB_ID_LENGTH);

    // Lower case hex only, so an id has one spelling and matches the one nya_blob_put writes.
    for (u32 i = 0; i < NYA_BLOB_ID_LENGTH; i++) {
        char c = hex[i];
        b8   ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%c' is not a lower case hex digit", c);
    }

    memcpy(out_id->hex, hex, NYA_BLOB_ID_LENGTH);
    out_id->hex[NYA_BLOB_ID_LENGTH] = '\0';
    return NYA_OK;
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

b8 _nya_blob_table_valid(NYA_ConstCString name) {
    if (name == nullptr) return false;

    u64 length = strlen(name);
    if (length == 0 || length >= NYA_BLOB_TABLE_MAX) return false;

    // [A-Za-z_] to start, [A-Za-z0-9_] after: the identifier grammar db_orm.h holds a table name to, since a table name is never a bound parameter.
    for (u64 i = 0; i < length; i++) {
        char c        = name[i];
        b8   letter   = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        b8   digit    = c >= '0' && c <= '9';
        if (i == 0 ? !letter : !(letter || digit)) return false;
    }

    return true;
}

void _nya_blob_id_from_digest(const NYA_CryptoSha256Digest* digest, OUT NYA_BlobId* out_id) {
    nya_assert(digest != nullptr);
    nya_assert(out_id != nullptr);

    NYA_ConstCString hex = "0123456789abcdef";
    for (u64 i = 0; i < NYA_CRYPTO_SHA256_BYTES; i++) {
        out_id->hex[i * 2]     = hex[digest->bytes[i] >> 4];
        out_id->hex[i * 2 + 1] = hex[digest->bytes[i] & 0x0F];
    }
    out_id->hex[NYA_BLOB_ID_LENGTH] = '\0';
}
