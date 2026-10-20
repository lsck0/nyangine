/**
 * @file db_sql.h
 *
 * Example:
 * ```c
 * NYA_Arena* arena = nya_arena_create(.name = "db");
 * defer      nya_arena_destroy(arena);
 *
 * NYA_Database* db = nullptr;
 * NYA_EXPECT(nya_sql_open(arena, "./save.db", &db));
 * defer nya_sql_close(db);
 *
 * NYA_EXPECT(nya_sql_exec(db, "CREATE TABLE IF NOT EXISTS runs (id INTEGER PRIMARY KEY, score INTEGER)"));
 *
 * // Bound, never interpolated. See nya_sql_query.
 * NYA_SqlValue args[] = { nya_sql_s64(4200) };
 * NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO runs (score) VALUES (?)", args, 1));
 *
 * NYA_SqlResult result = { 0 };
 * NYA_EXPECT(nya_sql_query(db, arena, "SELECT id, score FROM runs WHERE score > ?", args, 1, &result));
 *
 * nya_array_foreach (result.rows, row) {
 *     NYA_Value* score = nya_object_get(*row, "score");
 * }
 * ```
 *
 * ```c
 * NYA_EXPECT(nya_sql_exec(db, "CREATE VIRTUAL TABLE IF NOT EXISTS lines USING vec0(embedding float[384])"));
 *
 * // A vector is bound as a blob of little endian f32, which is what vec0 stores.
 * f32          embedding[384] = { 0 };
 * NYA_SqlValue insert[]       = { nya_sql_s64(line_id), nya_sql_blob((const u8*)embedding, sizeof(embedding)) };
 * NYA_EXPECT(nya_sql_exec_bound(db, "INSERT INTO lines (rowid, embedding) VALUES (?, ?)", insert, 2));
 *
 * NYA_SqlValue  search[] = { nya_sql_blob((const u8*)query_embedding, sizeof(query_embedding)) };
 * NYA_SqlResult nearest  = { 0 };
 * NYA_EXPECT(nya_sql_query(db, arena, "SELECT rowid, distance FROM lines WHERE embedding MATCH ? AND k = 8", search, 1, &nearest));
 * ```
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_array.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_object.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/base/base_types.h"

// ───────────────────────────────────── TYPES ─────────────────────────────────────

typedef struct NYA_Database                NYA_Database;
typedef struct NYA_SqlOptions              NYA_SqlOptions;
typedef struct NYA_SqlResult               NYA_SqlResult;
typedef struct NYA_SqlValue                NYA_SqlValue;
typedef struct NYA_SqlStatementCacheStats  NYA_SqlStatementCacheStats;
typedef enum NYA_SqlValueKind              NYA_SqlValueKind;
typedef enum NYA_SqlJournal                NYA_SqlJournal;
typedef enum NYA_SqlSynchronous            NYA_SqlSynchronous;

/**
 * Bytes in the key a database is encrypted under: 32, which is crypto_secret.h's NYA_CryptoKey32 and
 * what SQLCipher takes as a raw key. Fixed rather than a range, so no caller has to be changed on the
 * day the cipher lands and no caller can pick a shorter one.
 * */
#define NYA_SQL_KEY_SIZE 32

/**
 * Prepared statements one connection keeps, keyed by their SQL text; past it the least recently used
 * is finalized. web_server's notes connection settles at 6, so 64 leaves room for a connection that
 * carries accounts, jobs and blobs too. Each entry costs SQLite one compiled program.
 * */
#ifndef NYA_SQL_STATEMENT_CACHE_MAX
#define NYA_SQL_STATEMENT_CACHE_MAX 64
#endif

/** How long a statement waits on another connection's lock before it fails as NYA_ERROR_TIMEOUT. db_jobs.c's default too. */
#define NYA_SQL_BUSY_TIMEOUT_MS_DEFAULT 5000

/**
 * SQLite's page cache per connection, in KiB, against SQLite's own 2 MiB. bench_db_file.c over a 3 MiB
 * table: a point read 1.14 us at 2 MiB and 1.05 us here, 8 readers beside a writer 554 ns a read
 * against 430-480. Filled only as pages are read, and per connection: a pool of 8 can reach 8 times it.
 * */
#define NYA_SQL_CACHE_SIZE_KIB_DEFAULT 16384

/** Pages the write-ahead log grows by before a commit checkpoints it into the file. SQLite's default. */
#define NYA_SQL_WAL_AUTOCHECKPOINT_PAGES_DEFAULT 1000

/**
 * What the write-ahead log or rollback journal is truncated to after a checkpoint or commit, so one
 * large transaction does not leave a file that size on disk forever. 64 MiB.
 * */
#define NYA_SQL_JOURNAL_SIZE_LIMIT_BYTES_DEFAULT (64ULL * 1024 * 1024)

/** How a connection journals its writes. */
enum NYA_SqlJournal {
    /** A write-ahead log: readers never block the writer or each other, and a commit is one append. */
    NYA_SQL_JOURNAL_WAL,

    /** The rollback journal, deleted at every commit: one file on disk, for a filesystem without shared memory. */
    NYA_SQL_JOURNAL_DELETE,

    NYA_SQL_JOURNAL_COUNT,
};

/** How hard a commit waits for the disk. */
enum NYA_SqlSynchronous {
    /** NORMAL under WAL and FULL under DELETE: the cheapest level that a power cut cannot corrupt the file at. */
    NYA_SQL_SYNCHRONOUS_DEFAULT,

    /**
     * Under WAL, a commit is not synced and a checkpoint is: a power cut can lose the last commits but
     * never corrupts the file. Under DELETE, a power cut at the wrong moment can corrupt it.
     * */
    NYA_SQL_SYNCHRONOUS_NORMAL,

    /**
     * Every commit is synced before it returns, so a committed write survives a power cut. For data that
     * cannot be redone; bench_db_file.c measured a commit at 4.4 ms under WAL against NORMAL's 4.3 us.
     * */
    NYA_SQL_SYNCHRONOUS_FULL,

    NYA_SQL_SYNCHRONOUS_COUNT,
};

/** What nya_sql_open takes besides the arena and where to put the connection. Zero is the default for every field. */
struct NYA_SqlOptions {
    /** The file, created if it is not there, or ":memory:" for one that never touches disk. */
    NYA_ConstCString path;

    /**
     * The key the whole file is encrypted under, or null for a database in the clear. Exactly
     * NYA_SQL_KEY_SIZE bytes, read during the open and never copied, logged or put in a crash report.
     *
     * A build that cannot encrypt refuses a key rather than quietly writing the database in the
     * clear; see the encryption note in db.h and nya_sql_encryption_available.
     * */
    const u8* key;
    u32       key_size;

    /** The SQLite VFS to open through, by name, or null for the default. A test names NYA_DB_FAULT_VFS. */
    NYA_ConstCString vfs;

    NYA_SqlJournal     journal;
    NYA_SqlSynchronous synchronous;

    /** Zero is NYA_SQL_BUSY_TIMEOUT_MS_DEFAULT; negative fails at once on a held lock. */
    s64 busy_timeout_ms;

    /** Zero is NYA_SQL_CACHE_SIZE_KIB_DEFAULT. */
    u32 cache_size_kib;

    /**
     * Bytes of the file SQLite reads through a memory map instead of read(). Off unless asked for: an
     * I/O error on a mapped page is a SIGBUS that ends the process rather than a NYA_ERROR_IO the
     * caller can answer with a 503, and at 256 MiB bench_db_file.c measured no gain (1.11 us a point read).
     * */
    u64 mmap_size_bytes;

    /** Zero is NYA_SQL_WAL_AUTOCHECKPOINT_PAGES_DEFAULT. */
    u32 wal_autocheckpoint_pages;

    /** Zero is NYA_SQL_JOURNAL_SIZE_LIMIT_BYTES_DEFAULT. */
    u64 journal_size_limit_bytes;
};

/** How a connection's statement cache has done since it opened. */
struct NYA_SqlStatementCacheStats {
    u64 hits;
    u64 misses;
    u64 evictions;

    /** Statements cached now, at most NYA_SQL_STATEMENT_CACHE_MAX. */
    u32 count;
};

/** One row. A named typedef because nya_derive_array needs a single token for its type. */
typedef NYA_Object* NYA_SqlRow;
nya_derive_array(NYA_SqlRow);

enum NYA_SqlValueKind {
    NYA_SQL_VALUE_NULL,
    NYA_SQL_VALUE_S64,
    NYA_SQL_VALUE_F64,
    NYA_SQL_VALUE_TEXT,
    NYA_SQL_VALUE_BLOB,
    NYA_SQL_VALUE_COUNT,
};

/**
 * One bound parameter.
 * */
struct NYA_SqlValue {
    NYA_SqlValueKind kind;

    union {
        s64              as_s64;
        f64              as_f64;
        NYA_ConstCString as_text;

        struct {
            const u8* data;
            u64       size;
        } as_blob;
    };
};

struct NYA_SqlResult {
    /** One NYA_Object per row, keyed by column name. Empty rather than null when nothing matched. */
    NYA_ArrayᐸNYA_SqlRowᐳ* rows;

    /** Rows changed by the statement, as sqlite3_changes reports it. Zero for a SELECT. */
    u64 rows_affected;

    /** Rowid of the last successful insert on this connection, or zero when there was none. */
    s64 last_insert_id;
};

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

// Constructors for bound parameters, so a call site reads as data rather than as struct assembly.
#define nya_sql_null()          ((NYA_SqlValue){ .kind = NYA_SQL_VALUE_NULL })
#define nya_sql_s64(value)      ((NYA_SqlValue){ .kind = NYA_SQL_VALUE_S64, .as_s64 = (value) })
#define nya_sql_f64(value)      ((NYA_SqlValue){ .kind = NYA_SQL_VALUE_F64, .as_f64 = (value) })
#define nya_sql_text(value)     ((NYA_SqlValue){ .kind = NYA_SQL_VALUE_TEXT, .as_text = (value) })
#define nya_sql_blob(ptr, len)  ((NYA_SqlValue){ .kind = NYA_SQL_VALUE_BLOB, .as_blob = { .data = (ptr), .size = (len) } })

/**
 * Opens `path`, creating it if it is not there. Use ":memory:" for a database that never touches disk.
 *
 * The connection comes up tuned for a server: a write-ahead log, synchronous NORMAL, a busy timeout,
 * a 16 MiB page cache, temporary tables in memory (so nothing of the data reaches an unencrypted temp
 * file), foreign keys enforced, and `PRAGMA optimize=0x10002` so the planner has statistics from the
 * first query. Each is an option; see NYA_SqlOptions.
 *
 * Anything beyond the path is an option, so a call that wants none reads as it always did:
 *
 * ```c
 * NYA_TRY(nya_sql_open(arena, "./server.db", &database));
 * NYA_TRY(nya_sql_open(arena, "./server.db", &database, .key = key.bytes, .key_size = sizeof(key.bytes)));
 * ```
 * */
// Not named `path`: a macro parameter is substituted after the dot too, so `.path` would collide with the caller's variable name.
#define nya_sql_open(arena, database_path, out_database, ...) \
    nya_sql_open_with_options((arena), (NYA_SqlOptions){ .path = (database_path), __VA_ARGS__ }, (out_database))

/**
 * What nya_sql_open expands to. A key that this build cannot honour is refused before the file is
 * touched, so a database asked for encrypted is never created in the clear instead.
 * */
NYA_API NYA_Error nya_sql_open_with_options(NYA_Arena* arena, NYA_SqlOptions options, OUT NYA_Database** out_database) __attr_no_discard;

/**
 * Closes the connection, after finalizing its cached statements and running `PRAGMA optimize`, which
 * SQLite asks of every connection on its way out. Safe on null, so an unwind path does not need to check.
 * */
NYA_API void nya_sql_close(NYA_Database* database);

/**
 * Runs `PRAGMA optimize`: analyzes the tables whose statistics have gone stale since the last run, and
 * nothing when none have. Open and close run it already; a connection that stays open for days calls
 * this every few hours, as SQLite's documentation asks.
 * */
NYA_API NYA_Error nya_sql_optimize(NYA_Database* database) __attr_no_discard;

/** The statement cache's counters, for a metrics row or a test that a statement was reused. */
NYA_API NYA_SqlStatementCacheStats nya_sql_statement_cache_get_stats(const NYA_Database* database) __attr_no_discard;

/**
 * Runs a statement that returns no rows. Several separated by `;` are allowed, so it never goes
 * through the statement cache; nya_sql_exec_bound and nya_sql_query do.
 * */
NYA_API NYA_Error nya_sql_exec(NYA_Database* database, NYA_ConstCString sql) __attr_no_discard;

/** One statement, with parameters bound to its `?` placeholders. Returns no rows. */
NYA_API NYA_Error nya_sql_exec_bound(NYA_Database* database, NYA_ConstCString sql, const NYA_SqlValue* values, u32 value_count) __attr_no_discard;

/**
 * Runs one statement and collects every row into `out_result`.
 * */
NYA_API NYA_Error nya_sql_query(
    NYA_Database* database, NYA_Arena* arena, NYA_ConstCString sql, const NYA_SqlValue* values, u32 value_count, OUT NYA_SqlResult* out_result
) __attr_no_discard;

/**
 * Whether a read reaches the file: `PRAGMA schema_version`, which takes the shared lock, and the
 * file's header read through the VFS, past the page cache. `SELECT 1` touches neither, so a readiness
 * check built on it stays green through a dead disk.
 * */
NYA_API NYA_Error nya_sql_ping(NYA_Database* database) __attr_no_discard;

/**
 * `BEGIN IMMEDIATE`: takes the write lock up front, waiting through the busy timeout. A deferred BEGIN
 * that reads and then writes under WAL fails with SQLITE_BUSY at the write, without waiting, whenever
 * another connection committed in between.
 * */
NYA_API NYA_Error nya_sql_transaction_begin(NYA_Database* database) __attr_no_discard;
NYA_API NYA_Error nya_sql_transaction_commit(NYA_Database* database) __attr_no_discard;
NYA_API NYA_Error nya_sql_transaction_rollback(NYA_Database* database) __attr_no_discard;

/**
 * Whether this build can honour NYA_SqlOptions.key, which today is false everywhere: the vendored
 * sqlite has no cipher in it. Asked rather than assumed, so a program that must not store anything
 * unencrypted can refuse to start instead of finding out by reading its own file off the disk.
 * */
NYA_API b8 nya_sql_encryption_available(void) __attr_no_discard;

/** The library version SQLite reports, for a log line or a bug report. */
NYA_API NYA_ConstCString nya_sql_version(void) __attr_no_discard;

/** The sqlite-vec version linked in, in upstream's `vX.Y.Z` form. Same purpose as nya_sql_version. */
NYA_API NYA_ConstCString nya_sql_vec_version(void) __attr_no_discard;
