/**
 * @file db_blob.h
 *
 * A content-addressed blob store in a SQLite table: MinIO's idea — an object whose key is its own
 * content — with SQLite's shape, which is that the whole store is one file.
 *
 * The key of an object *is* the SHA-256 of its bytes, written as lower case hex. That one decision is
 * the whole design:
 *
 *   - **Dedup is free.** Identical bytes hash to the same id, so a second put of the same object is a
 *     no-op that returns the id already there. The store keeps one copy however many times it is put.
 *   - **Objects are immutable.** An id names exactly one sequence of bytes and never another, so a
 *     reference to an id can be cached forever and never goes stale — the same property the hashed
 *     names in http_static.h earn, and the reason this feels consistent with them.
 *   - **Integrity is checkable.** nya_blob_get rehashes the bytes it read and refuses to hand back an
 *     object whose stored bytes no longer hash to the id asked for, so a flipped bit on the disk, or a
 *     row edited under the store's feet, is caught rather than served.
 *
 * It lives in the db module, over a NYA_Database and nothing else, so any program that has a database
 * has a blob store — a game's save assets, a bot's attachments, a server's uploads — without dragging
 * in http or a filesystem. The store's table is created in whichever database the caller opens it on,
 * so it can share the app's file (and its encryption key and its transactions) or have its own.
 *
 * Overview:
 *   nya_blob_store_open / _close    create or migrate the table in a database, then let go of it
 *   nya_blob_put                    store bytes, get back their id; idempotent, so a repeat is one row
 *   nya_blob_get                    read an object back, verifying it still hashes to its id
 *   nya_blob_has                    whether an id is in the store
 *   nya_blob_size                   how many bytes an id names
 *   nya_blob_delete                 drop an object
 *   nya_blob_list                   walk every id with its size and store time
 *   nya_blob_id_parse               turn caller-supplied hex text into a validated id
 *
 * ```c
 * NYA_BlobStore* store = nullptr;
 * NYA_TRY(nya_blob_store_open(arena, database, &store));
 * defer nya_blob_store_close(store);
 *
 * NYA_BlobId id = { 0 };
 * NYA_TRY(nya_blob_put(store, bytes, size, &id));   // id.hex is the SHA-256, 64 hex digits
 *
 * u8* out      = nullptr;
 * u64 out_size = 0;
 * NYA_TRY(nya_blob_get(store, id, arena, &out, &out_size));  // rehashed and verified before it returns
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * WHAT THE STORE IS NOT
 * ─────────────────────────────────────────────────────────
 *
 * **An id is not a capability.** Knowing a hash is not permission to read the object: a content
 * address is guessable in principle (it is a hash of the content, not a random secret) and is meant to
 * be passed around, logged and cached. So the store itself is auth-agnostic — it answers any caller in
 * the same process — and access control is a layer above it, never "whoever quotes the hash gets the
 * bytes".
 *
 * **The next layer up is the HTTP facade, and it must sit behind accounts.** The intended follow-on
 * (rank 6, in the http module) is an upload/download route pair. It MUST guard every read and write
 * through the auth system: the `__Host-session` cookie resolved to a validated session and a user id,
 * and then a per-object ownership or permission check — never public-by-URL, never "has the hash". The
 * core store gives that layer what it needs to do the check: nya_blob_put records who and when only in
 * the sense of a created time; the *owner* of an object is the facade's to record, either by having the
 * store's caller keep a `blob id -> owner` row in the app's own table, or by the facade mapping the id
 * to an owner before it serves a byte. Whichever way, the facade refuses a get or a delete the
 * session's user is not allowed, and this file is the place that says so.
 *
 * ─────────────────────────────────────────────────────────
 * SIZE, AND WHY THE WHOLE OBJECT IS IN MEMORY
 * ─────────────────────────────────────────────────────────
 *
 * put takes an object that is already whole in memory, and get returns one whole into an arena, so an
 * object is bounded by what fits in memory with room to spare — the more so because the read path
 * moves the bytes through the db layer's base64 row encoding (see db_sql.c), which is transiently
 * larger than the object. NYA_BLOB_MAX_SIZE is the ceiling both ends enforce: a put over it is refused
 * before anything is hashed or written, so the bound is stated once and cannot be exceeded by a caller
 * that forgot it. A streaming, chunked API for objects larger than this is a possible later addition
 * (SQLite's incremental blob I/O), but the whole-object path is the one that has to be correct first,
 * and it is what this file is.
 *
 * ─────────────────────────────────────────────────────────
 * THE SCHEMA, AND WHY IT IS NOT SQLAR
 * ─────────────────────────────────────────────────────────
 *
 * One table, whose columns are the object's identity and its bytes:
 *
 * ```sql
 * CREATE TABLE <name> (
 *     id      TEXT    PRIMARY KEY NOT NULL,   -- lower case hex SHA-256 of `data`, 64 characters
 *     size    INTEGER NOT NULL,              -- bytes in `data`, so a size query reads no blob
 *     created INTEGER NOT NULL,              -- unix seconds when the object was first stored
 *     data    BLOB    NOT NULL               -- the object's bytes
 * ) WITHOUT ROWID;                            -- the id is the key; there is no second one to keep
 * ```
 *
 * The SQLite Archive format (`sqlite3 -A`) is deliberately not what this is. sqlar keys a row by
 * `name` — a file path — and stores `mode,mtime,sz,data`: it is a tar in a table, addressed by where a
 * file came from, and two identical files under two names are two rows. That is the opposite of what a
 * content-addressed store is for. Keying by the hash is what buys dedup and immutability, and a path is
 * exactly the thing this store does not have (and so cannot be made to traverse). The columns are named
 * for what they are rather than to be read by another tool.
 *
 * ─────────────────────────────────────────────────────────
 * WHAT ELSE TO KNOW
 * ─────────────────────────────────────────────────────────
 *
 * - **One thread per store, as one thread per connection.** The store is a thin thing over a
 *   NYA_Database, and inherits its rule from db.h: a store and the connection under it belong to one
 *   thread.
 * - **No id is ever trusted as bytes.** put computes the id from the object; it never takes one from a
 *   caller. nya_blob_id_parse is the only way caller text becomes an id, and it validates the shape
 *   (64 lower case hex digits) before the value exists, so a malformed id cannot reach a statement.
 * - **Identifiers, not data.** The table name is checked against NYA_BLOB_TABLE_MAX and an identifier
 *   grammar at open, because a table name cannot be a bound parameter; every object's bytes and id
 *   reach statements only as bound values.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/crypto/crypto_hash.h"
#include "nyangine-core/db/db_sql.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/**
 * Hex digits in an id: two per SHA-256 byte, so the digest has one spelling and it is this one. Kept a
 * u64 so the value is wide wherever it indexes or sizes, and never a narrow multiplication widened.
 * */
#define NYA_BLOB_ID_LENGTH ((u64)NYA_CRYPTO_SHA256_BYTES * 2)

/**
 * The largest object a put accepts and a get returns, 128 MiB.
 *
 * The whole object is in memory at both ends (see the size note in this file's block), and the read
 * path carries it through the db layer's base64 row encoding, which is about a third larger again for
 * the length of the read. 128 MiB leaves an object, its base64 form and its decoded copy all resident
 * without a store operation being the thing that exhausts an arena, and is far under SQLite's own
 * roughly-1 GB blob limit. A put over it is refused before the object is hashed or written.
 * */
#define NYA_BLOB_MAX_SIZE (128ULL * 1024 * 1024)

/** Longest store table name, terminator included; the same ceiling db_orm.h holds a table name to. */
#define NYA_BLOB_TABLE_MAX 64

/** The table a store uses when the caller names none. */
#define NYA_BLOB_TABLE_DEFAULT "blobs"

// ───────────────────────────────────── TYPES ─────────────────────────────────────

typedef struct NYA_BlobStore        NYA_BlobStore;
typedef struct NYA_BlobStoreOptions NYA_BlobStoreOptions;
typedef struct NYA_BlobId           NYA_BlobId;

/**
 * An object's id: its SHA-256 as NYA_BLOB_ID_LENGTH lower case hex digits, terminated. A value, so it
 * is passed and returned by copy and never allocates; `hex` is a C string a query can bind directly.
 * */
struct NYA_BlobId {
    char hex[NYA_BLOB_ID_LENGTH + 1];
};

/** What nya_blob_store_open takes besides the arena, the database and where to put the store. */
struct NYA_BlobStoreOptions {
    /**
     * The table the objects live in, created if it is not there. Null means NYA_BLOB_TABLE_DEFAULT.
     * An identifier — letters, digits and underscore, starting with a letter or underscore — because
     * it cannot be a bound parameter; anything else is refused at open.
     * */
    NYA_ConstCString table;
};

/**
 * Called once per object by nya_blob_list, with the id, its size and the unix second it was stored.
 * Returning false stops the walk, which is how a caller reads only as far as it needs to.
 * */
typedef b8 (*NYA_BlobVisitor)(NYA_BlobId id, u64 size, u64 created, void* user_data);

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

/**
 * Opens a store on `database`, creating or growing its table. The store is allocated in `arena` and is
 * valid until nya_blob_store_close or the arena dies, whichever comes first.
 *
 * ```c
 * NYA_TRY(nya_blob_store_open(arena, database, &store));
 * NYA_TRY(nya_blob_store_open(arena, database, &store, .table = "attachments"));
 * ```
 * */
// Not named `table`, like nya_sql_open: a macro parameter is substituted after the dot and would take the caller's variable name.
#define nya_blob_store_open(arena, database, out_store, ...) \
    nya_blob_store_open_with_options((arena), (database), (NYA_BlobStoreOptions){ __VA_ARGS__ }, (out_store))

/** What nya_blob_store_open expands to. Refuses a table name that is not an identifier. */
NYA_API NYA_Error nya_blob_store_open_with_options(
    NYA_Arena* arena, NYA_Database* database, NYA_BlobStoreOptions options, OUT NYA_BlobStore** out_store
) __attr_no_discard;

/** Lets go of the store. Safe on null, so an unwind path does not need to check. Leaves the table. */
NYA_API void nya_blob_store_close(NYA_BlobStore* store);

/**
 * Stores `size` bytes and writes their id to `out_id`. The id is the SHA-256 of the bytes, computed
 * here and never taken from the caller.
 *
 * Idempotent: putting bytes already in the store writes no second row and returns the same id, which is
 * what makes the store deduplicate. NYA_ERROR_INVALID_ARGUMENT for a `size` over NYA_BLOB_MAX_SIZE,
 * refused before anything is hashed or written. An empty object (size 0) is valid and has an id.
 * */
NYA_API NYA_Error nya_blob_put(NYA_BlobStore* store, const u8* bytes, u64 size, OUT NYA_BlobId* out_id) __attr_no_discard;

/**
 * Reads the object `id` names into `out_arena` and points `out_bytes`/`out_size` at it.
 *
 * The bytes read are rehashed and the digest compared to `id` before anything is returned:
 * NYA_ERROR_CORRUPT when they no longer agree, which is a flipped bit or a row changed outside the
 * store, and NYA_ERROR_NOT_FOUND when there is no such id. On either error `out_bytes` is null and
 * `out_size` is zero. A zero-length object returns size 0 and a non-null pointer.
 * */
NYA_API NYA_Error nya_blob_get(NYA_BlobStore* store, NYA_BlobId id, NYA_Arena* out_arena, OUT u8** out_bytes, OUT u64* out_size) __attr_no_discard;

/** Whether `id` is in the store. A read error or an absent id is false; only a present id is true. */
NYA_API b8 nya_blob_has(NYA_BlobStore* store, NYA_BlobId id) __attr_no_discard;

/**
 * Writes the size of the object `id` names to `out_size`, reading no blob to do it. NYA_ERROR_NOT_FOUND
 * when there is no such id, with `out_size` left zero.
 * */
NYA_API NYA_Error nya_blob_size(NYA_BlobStore* store, NYA_BlobId id, OUT u64* out_size) __attr_no_discard;

/**
 * Drops the object `id` names. NYA_ERROR_NOT_FOUND when it was not there, so a caller can tell a delete
 * that did something from one that found nothing; the store is in the same state after either.
 * */
NYA_API NYA_Error nya_blob_delete(NYA_BlobStore* store, NYA_BlobId id) __attr_no_discard;

/**
 * Calls `visitor` once per object, in id order, with its id, size and store time. Stops early when the
 * visitor returns false. `user_data` is passed through untouched.
 * */
NYA_API NYA_Error nya_blob_list(NYA_BlobStore* store, NYA_BlobVisitor visitor, void* user_data) __attr_no_discard;

/**
 * Turns `hex` into a validated id: exactly NYA_BLOB_ID_LENGTH lower case hex digits, nothing else.
 * NYA_ERROR_INVALID_ARGUMENT for any other length or character, with `out_id` left zeroed. The only
 * way caller-supplied text becomes an id, so a store operation never sees an unchecked one.
 * */
NYA_API NYA_Error nya_blob_id_parse(NYA_ConstCString hex, OUT NYA_BlobId* out_id) __attr_no_discard;
