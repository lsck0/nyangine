/**
 * @file db_fault.h
 *
 * A SQLite VFS that lies on purpose, for tests: it wraps the default VFS and, while a fault is armed,
 * answers a lock with SQLITE_BUSY, a read, write or sync with an I/O error, a write with a full disk,
 * stalls a call, or drops every write from a chosen call on as a power cut would. Compiled only under
 * NYA_TESTING, and a connection only meets it when opened through it by name.
 *
 * Overview:
 *   nya_db_fault_register / _unregister   add the VFS under NYA_DB_FAULT_VFS, with the clock a stall waits on
 *   nya_db_fault_arm / _disarm            one fault per kind, answered for `count` calls after `after` calls
 *   nya_db_fault_disarm_all               the disk is healthy again
 *   nya_db_fault_armed                    whether a kind will answer its next call
 *   nya_db_fault_hits                     calls answered with a fault since register
 *
 * ```c
 * nya_db_fault_register(nullptr, nullptr);
 * defer nya_db_fault_unregister();
 *
 * NYA_TRY(nya_sql_open(arena, "./test.db", &database, .vfs = NYA_DB_FAULT_VFS));
 *
 * nya_db_fault_arm(.kind = NYA_DB_FAULT_IOERR_FSYNC, .count = 1);
 * NYA_Error failed = nya_sql_exec(database, "INSERT INTO t VALUES (1)");   // SQLITE_IOERR_FSYNC
 * nya_db_fault_disarm_all();
 * ```
 *
 * ## Why a VFS and not a mock of db_sql
 *
 * The VFS is the only seam below SQLite's pager, so a fault here meets the rollback, the busy handler
 * and the hot journal recovery a real disk failure meets. A mock above SQLite would test what its author
 * thought SQLite does.
 *
 * ## Time
 *
 * A stall and the busy handler's backoff both wait through the `sleep` given at register, so a
 * simulation advances its own clock instead of blocking. Null sleeps for real, for a test where the
 * wait itself is the point. One thread: the table is plain state, as the simulation that arms it is.
 * */
#pragma once

#ifdef NYA_TESTING

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_clock_instant.h"
#include "nyangine-std/base/base_types.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/** The name a connection opens through, as NYA_SqlOptions.vfs. */
#define NYA_DB_FAULT_VFS "nya_fault"

// ───────────────────────────────────── TYPES ─────────────────────────────────────

typedef struct NYA_DbFault  NYA_DbFault;
typedef enum NYA_DbFaultKind NYA_DbFaultKind;

/** Waits `duration`: a real sleep, or a simulated clock moved forward. */
typedef void (*NYA_DbFaultSleepFn)(void* context, NYA_Duration duration);

enum NYA_DbFaultKind {
    /** A lock answers SQLITE_BUSY, as when another connection holds the file. */
    NYA_DB_FAULT_BUSY,

    /** A read answers SQLITE_IOERR_READ. */
    NYA_DB_FAULT_IOERR_READ,

    /** A write answers SQLITE_IOERR_WRITE. */
    NYA_DB_FAULT_IOERR_WRITE,

    /** A sync answers SQLITE_IOERR_FSYNC. */
    NYA_DB_FAULT_IOERR_FSYNC,

    /** A write answers SQLITE_FULL. */
    NYA_DB_FAULT_FULL,

    /** A read, write or sync waits `stall` and then goes through. */
    NYA_DB_FAULT_STALL,

    /** Every write, sync, truncate and delete reports success and changes nothing: the power is gone. */
    NYA_DB_FAULT_POWER_LOSS,

    NYA_DB_FAULT_KIND_COUNT,
};

struct NYA_DbFault {
    NYA_DbFaultKind kind;

    /** Calls of its kind let through untouched before it starts answering, so a commit can die halfway. */
    u32 after;

    /** Calls it answers before it disarms itself. Zero answers every call until it is disarmed. */
    u32 count;

    /** How long a STALL waits. Ignored by every other kind. */
    NYA_Duration stall;
};

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

/** Registers the VFS over the default one, with nothing armed. `sleep` null waits for real. */
NYA_API void nya_db_fault_register(NYA_DbFaultSleepFn sleep, void* context);

/** Disarms everything and removes the VFS. A no-op when it is not registered. Close its connections first. */
NYA_API void nya_db_fault_unregister(void);

/** Arms `fault`, replacing whatever its kind had armed. */
#define nya_db_fault_arm(...) nya_db_fault_arm_with_options((NYA_DbFault){ __VA_ARGS__ })

/** What nya_db_fault_arm expands to. */
NYA_API void nya_db_fault_arm_with_options(NYA_DbFault fault);

/** Disarms one kind. */
NYA_API void nya_db_fault_disarm(NYA_DbFaultKind kind);

/** Disarms every kind. */
NYA_API void nya_db_fault_disarm_all(void);

/** Whether `kind` is armed and will answer its next call, once `after` has run out. */
NYA_API b8 nya_db_fault_armed(NYA_DbFaultKind kind) __attr_no_discard;

/** Calls answered with a fault since register, so a test can tell a fault that fired from one that never met a call. */
NYA_API u64 nya_db_fault_hits(void) __attr_no_discard;

#endif // NYA_TESTING
