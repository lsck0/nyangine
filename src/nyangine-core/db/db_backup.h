/**
 * @file db_backup.h
 *
 * A hot backup of a live database, a WAL checkpoint, and a defragmenting copy. Everything here
 * works while the connection is open and being written, which is the whole point: a server keeps
 * one database file next to its executable and has to be able to snapshot it without stopping.
 *
 * ```c
 * NYA_Database* db = nullptr;
 * NYA_TRY(nya_sql_open(arena, "./server.db", &db));
 * defer nya_sql_close(db);
 *
 * // ... the server runs, writing as it goes ...
 *
 * // A consistent point-in-time copy, taken without closing or locking out writers for long.
 * NYA_TRY(nya_sql_backup(db, "./server.backup.db"));
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * WHICH MECHANISM
 * ─────────────────────────────────────────────────────────
 *
 * nya_sql_backup is the online backup API (sqlite3_backup_init/step/finish). It copies the source
 * a page at a time between two connections, so writers are only locked out for the span of one
 * step rather than the whole copy, and a write that lands mid-copy is noticed and the affected
 * pages are recopied — the result is a snapshot consistent as of the moment the last step ran. It
 * reads through the write-ahead log, so a WAL-mode database is captured whole with no checkpoint
 * needed first.
 *
 * nya_sql_vacuum_into is `VACUUM INTO`: one statement, also hot and WAL-safe, and it defragments
 * as it writes so the copy is as small as a fresh load of the same rows. The tradeoff is that it
 * holds a read lock for the entire copy rather than yielding between steps, so it is the choice
 * when the copy is small or nothing else is writing, and nya_sql_backup the choice under a live
 * write load. Both refuse to overwrite an existing file, so a stale backup is never half-written
 * over.
 *
 * ─────────────────────────────────────────────────────────
 * ENCRYPTION
 * ─────────────────────────────────────────────────────────
 *
 * An encrypted database cannot be byte-copied and stay usable: the copy needs the key applied to
 * its own connection or it is written in the clear. So the backup takes the key the destination is
 * to be written under, exactly as nya_sql_open does, and — like open — refuses a key on a build
 * with no cipher rather than producing a file that silently cannot be opened. A source that was
 * itself opened under a key and handed no key here is refused too, since the alternative is a
 * plaintext copy of a database somebody asked to keep encrypted. Until SQLCipher is vendored
 * (see db.h) no source is ever encrypted, so in practice the key is always absent and the copy is
 * a copy in the clear of a database in the clear.
 * */
#pragma once

#include "nyangine-core/db/db_sql.h"

typedef struct NYA_SqlBackupOptions NYA_SqlBackupOptions;
typedef enum NYA_SqlCheckpoint      NYA_SqlCheckpoint;

/** What nya_sql_backup takes besides the source connection. */
struct NYA_SqlBackupOptions {
    /** Where to write the copy. Refused if a file is already there, so a backup never clobbers one. */
    NYA_ConstCString dest_path;

    /**
     * The key the copy is to be encrypted under, or null for a copy in the clear. Exactly
     * NYA_SQL_KEY_SIZE bytes, read during the backup and never copied, logged or put in a crash
     * report — the same contract as NYA_SqlOptions.key. A build with no cipher refuses it rather
     * than writing the copy unencrypted; see nya_sql_encryption_available.
     * */
    const u8* key;
    u32       key_size;

    /**
     * Pages copied per step of the online backup. Larger finishes sooner and yields to writers
     * less often; smaller yields more. Zero or negative picks a sensible default. Not the size of
     * the copy, which is the whole database either way — only how it is split.
     * */
    s32 pages_per_step;
};

/** Which WAL frames a checkpoint moves into the database file, from least to most aggressive. */
enum NYA_SqlCheckpoint {
    /** Checkpoint what it can without waiting on any reader. The gentlest, and what SQLite does on its own. */
    NYA_SQL_CHECKPOINT_PASSIVE,
    /** Wait for readers, then checkpoint every committed frame. */
    NYA_SQL_CHECKPOINT_FULL,
    /** FULL, and then wait for readers again so the next writer starts the WAL over from its front. */
    NYA_SQL_CHECKPOINT_RESTART,
    /** RESTART, and then truncate the WAL file to nothing. What keeps the WAL from growing without bound. */
    NYA_SQL_CHECKPOINT_TRUNCATE,
};

/**
 * Copies a live `source` to `options.dest_path` with the online backup API, a consistent snapshot
 * taken while the connection stays open and writable. The destination must not already exist.
 *
 * ```c
 * NYA_TRY(nya_sql_backup(db, "./server.backup.db"));
 * NYA_TRY(nya_sql_backup(db, "./server.backup.db", .pages_per_step = 64));
 * ```
 * */
// Not named `dest_path`, like nya_sql_open's `path`: a macro parameter is substituted after the dot too and would take the caller's name.
#define nya_sql_backup(source, destination_path, ...) \
    nya_sql_backup_with_options((source), (NYA_SqlBackupOptions){ .dest_path = (destination_path), __VA_ARGS__ })

/** What nya_sql_backup expands to. A key this build cannot honour is refused before any file is made. */
NYA_API NYA_Error nya_sql_backup_with_options(NYA_Database* source, NYA_SqlBackupOptions options) __attr_no_discard;

/**
 * `VACUUM INTO dest_path`: a single-statement hot copy that also defragments. Simpler than
 * nya_sql_backup and smaller in its output, but it holds a read lock for the whole copy rather
 * than yielding between steps. The destination must not already exist. An encrypted source is
 * copied under its own key by SQLite, so this takes none.
 * */
NYA_API NYA_Error nya_sql_vacuum_into(NYA_Database* database, NYA_ConstCString dest_path) __attr_no_discard;

/**
 * Runs `PRAGMA wal_checkpoint(<mode>)`, moving committed WAL frames into the database file. A
 * no-op on a database not in WAL mode. Relevant around a backup so committed frames are folded in
 * and the WAL does not grow without bound; the online backup in nya_sql_backup already reads
 * through the WAL, so this is control rather than a prerequisite. A checkpoint blocked by a live
 * reader does as much as it can and still returns NYA_OK, which is SQLite's own behaviour.
 * */
NYA_API NYA_Error nya_sql_checkpoint(NYA_Database* database, NYA_SqlCheckpoint mode) __attr_no_discard;
