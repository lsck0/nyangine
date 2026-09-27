#include "nyangine-core/nyangine.h"

#ifdef NYA_TESTING

#include <sqlite3.h>

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/** Our file, with the real VFS's file right behind it in the block SQLite allocates (szOsFile covers both). */
typedef struct {
    sqlite3_file  base;
    sqlite3_file* real;
} _NYA_DbFaultFile;

/** One kind's arming: calls still to let through, then calls still to answer (zero with `active` is forever). */
typedef struct {
    b8           active;
    u32          after;
    u32          count;
    NYA_Duration stall;
} _NYA_DbFaultSlot;

typedef struct {
    b8 registered;

    sqlite3_vfs  vfs;
    sqlite3_vfs* real;

    NYA_DbFaultSleepFn sleep;
    void*              sleep_context;

    _NYA_DbFaultSlot slots[NYA_DB_FAULT_KIND_COUNT];
    u64              hits;
} _NYA_DbFaultState;

NYA_INTERNAL _NYA_DbFaultState _NYA_DB_FAULT = { 0 };

/** Whether `kind` answers this call, spending one of its `after` or `count` calls. */
NYA_INTERNAL b8 _nya_db_fault_take(NYA_DbFaultKind kind);

/** Waits through the registered sleep, or for real without one. */
NYA_INTERNAL void _nya_db_fault_wait(NYA_Duration duration);

/** Takes a STALL, if one is armed, before a read, write or sync goes through. */
NYA_INTERNAL void _nya_db_fault_stall(void);

NYA_INTERNAL sqlite3_file* _nya_db_fault_real(sqlite3_file* file);

/* The VFS calls it intercepts; every other one is the real VFS's own function. */
NYA_INTERNAL int _nya_db_fault_open(sqlite3_vfs* vfs, const char* name, sqlite3_file* file, int flags, int* out_flags);
NYA_INTERNAL int _nya_db_fault_delete(sqlite3_vfs* vfs, const char* name, int sync_directory);
NYA_INTERNAL int _nya_db_fault_sleep(sqlite3_vfs* vfs, int microseconds);

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

void nya_db_fault_register(NYA_DbFaultSleepFn sleep, void* context) {
    nya_assert(!_NYA_DB_FAULT.registered, "the fault VFS is already registered");

    sqlite3_vfs* real = sqlite3_vfs_find(nullptr);
    nya_assert(real != nullptr, "SQLite has no default VFS to wrap");

    _NYA_DB_FAULT = (_NYA_DbFaultState){ .registered = true, .real = real, .sleep = sleep, .sleep_context = context };

    // a copy, so every call this does not intercept goes straight to the real VFS's own function.
    _NYA_DB_FAULT.vfs          = *real;
    _NYA_DB_FAULT.vfs.pNext    = nullptr;
    _NYA_DB_FAULT.vfs.zName    = NYA_DB_FAULT_VFS;
    _NYA_DB_FAULT.vfs.szOsFile = (int)sizeof(_NYA_DbFaultFile) + real->szOsFile;
    _NYA_DB_FAULT.vfs.xOpen    = _nya_db_fault_open;
    _NYA_DB_FAULT.vfs.xDelete  = _nya_db_fault_delete;
    _NYA_DB_FAULT.vfs.xSleep   = _nya_db_fault_sleep;

    int code = sqlite3_vfs_register(&_NYA_DB_FAULT.vfs, 0);
    nya_assert(code == SQLITE_OK, "could not register the fault VFS: %d", code);
}

void nya_db_fault_unregister(void) {
    if (!_NYA_DB_FAULT.registered) return;

    (void)sqlite3_vfs_unregister(&_NYA_DB_FAULT.vfs);
    _NYA_DB_FAULT = (_NYA_DbFaultState){ 0 };
}

void nya_db_fault_arm_with_options(NYA_DbFault fault) {
    nya_assert(_NYA_DB_FAULT.registered, "nya_db_fault_register comes first");
    nya_assert(fault.kind < NYA_DB_FAULT_KIND_COUNT, "unknown fault kind %d", (s32)fault.kind);
    nya_assert(fault.stall.ns >= 0, "a stall cannot wait a negative time");

    _NYA_DB_FAULT.slots[fault.kind] = (_NYA_DbFaultSlot){ .active = true, .after = fault.after, .count = fault.count, .stall = fault.stall };
}

void nya_db_fault_disarm(NYA_DbFaultKind kind) {
    nya_assert(kind < NYA_DB_FAULT_KIND_COUNT, "unknown fault kind %d", (s32)kind);

    _NYA_DB_FAULT.slots[kind] = (_NYA_DbFaultSlot){ 0 };
}

void nya_db_fault_disarm_all(void) {
    nya_memset(_NYA_DB_FAULT.slots, 0, sizeof(_NYA_DB_FAULT.slots));
}

b8 nya_db_fault_armed(NYA_DbFaultKind kind) {
    nya_assert(kind < NYA_DB_FAULT_KIND_COUNT, "unknown fault kind %d", (s32)kind);

    return _NYA_DB_FAULT.slots[kind].active;
}

u64 nya_db_fault_hits(void) {
    return _NYA_DB_FAULT.hits;
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

b8 _nya_db_fault_take(NYA_DbFaultKind kind) {
    _NYA_DbFaultSlot* slot = &_NYA_DB_FAULT.slots[kind];
    if (!slot->active) return false;

    if (slot->after > 0) {
        slot->after--;
        return false;
    }

    _NYA_DB_FAULT.hits++;

    // counted: this call is one of them, and the last one disarms the kind; zero stays armed until told.
    if (slot->count > 0 && --slot->count == 0) slot->active = false;

    return true;
}

void _nya_db_fault_wait(NYA_Duration duration) {
    nya_assert(duration.ns >= 0);

    if (_NYA_DB_FAULT.sleep != nullptr) {
        _NYA_DB_FAULT.sleep(_NYA_DB_FAULT.sleep_context, duration);
        return;
    }

    s64 milliseconds = duration.ns / (NYA_NS_PER_SECOND / 1000);
    nya_os_time_sleep_ms(milliseconds > (s64)U32_MAX ? U32_MAX : (u32)milliseconds);
}

void _nya_db_fault_stall(void) {
    // a take that disarms the slot only clears `active`, so the duration is still there to wait.
    if (_nya_db_fault_take(NYA_DB_FAULT_STALL)) _nya_db_fault_wait(_NYA_DB_FAULT.slots[NYA_DB_FAULT_STALL].stall);
}

sqlite3_file* _nya_db_fault_real(sqlite3_file* file) {
    return ((_NYA_DbFaultFile*)file)->real;
}

// ── io methods: each one decides whether to lie, then forwards to the real file ──

NYA_INTERNAL int _nya_db_fault_close(sqlite3_file* file) {
    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xClose(real);
}

NYA_INTERNAL int _nya_db_fault_read(sqlite3_file* file, void* buffer, int amount, sqlite3_int64 offset) {
    if (_nya_db_fault_take(NYA_DB_FAULT_IOERR_READ)) return SQLITE_IOERR_READ;
    _nya_db_fault_stall();

    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xRead(real, buffer, amount, offset);
}

NYA_INTERNAL int _nya_db_fault_write(sqlite3_file* file, const void* buffer, int amount, sqlite3_int64 offset) {
    if (_nya_db_fault_take(NYA_DB_FAULT_POWER_LOSS)) return SQLITE_OK;
    if (_nya_db_fault_take(NYA_DB_FAULT_FULL)) return SQLITE_FULL;
    if (_nya_db_fault_take(NYA_DB_FAULT_IOERR_WRITE)) return SQLITE_IOERR_WRITE;
    _nya_db_fault_stall();

    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xWrite(real, buffer, amount, offset);
}

NYA_INTERNAL int _nya_db_fault_truncate(sqlite3_file* file, sqlite3_int64 size) {
    if (_nya_db_fault_take(NYA_DB_FAULT_POWER_LOSS)) return SQLITE_OK;

    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xTruncate(real, size);
}

NYA_INTERNAL int _nya_db_fault_sync(sqlite3_file* file, int flags) {
    if (_nya_db_fault_take(NYA_DB_FAULT_POWER_LOSS)) return SQLITE_OK;
    if (_nya_db_fault_take(NYA_DB_FAULT_IOERR_FSYNC)) return SQLITE_IOERR_FSYNC;
    _nya_db_fault_stall();

    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xSync(real, flags);
}

NYA_INTERNAL int _nya_db_fault_file_size(sqlite3_file* file, sqlite3_int64* out_size) {
    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xFileSize(real, out_size);
}

NYA_INTERNAL int _nya_db_fault_lock(sqlite3_file* file, int level) {
    if (_nya_db_fault_take(NYA_DB_FAULT_BUSY)) return SQLITE_BUSY;

    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xLock(real, level);
}

NYA_INTERNAL int _nya_db_fault_unlock(sqlite3_file* file, int level) {
    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xUnlock(real, level);
}

NYA_INTERNAL int _nya_db_fault_reserved(sqlite3_file* file, int* out_reserved) {
    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xCheckReservedLock(real, out_reserved);
}

NYA_INTERNAL int _nya_db_fault_control(sqlite3_file* file, int operation, void* argument) {
    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xFileControl(real, operation, argument);
}

NYA_INTERNAL int _nya_db_fault_sector_size(sqlite3_file* file) {
    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xSectorSize(real);
}

NYA_INTERNAL int _nya_db_fault_characteristics(sqlite3_file* file) {
    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xDeviceCharacteristics(real);
}

NYA_INTERNAL int _nya_db_fault_shm_map(sqlite3_file* file, int region, int size, int extend, void volatile** out_memory) {
    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xShmMap(real, region, size, extend, out_memory);
}

NYA_INTERNAL int _nya_db_fault_shm_lock(sqlite3_file* file, int offset, int count, int flags) {
    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xShmLock(real, offset, count, flags);
}

NYA_INTERNAL void _nya_db_fault_shm_barrier(sqlite3_file* file) {
    sqlite3_file* real = _nya_db_fault_real(file);
    real->pMethods->xShmBarrier(real);
}

NYA_INTERNAL int _nya_db_fault_shm_unmap(sqlite3_file* file, int delete_flag) {
    sqlite3_file* real = _nya_db_fault_real(file);
    return real->pMethods->xShmUnmap(real, delete_flag);
}

// version 2, not 3: without xFetch SQLite never memory-maps the file, so no read can go around xRead.
NYA_INTERNAL const sqlite3_io_methods _NYA_DB_FAULT_METHODS = {
    .iVersion               = 2,
    .xClose                 = _nya_db_fault_close,
    .xRead                  = _nya_db_fault_read,
    .xWrite                 = _nya_db_fault_write,
    .xTruncate              = _nya_db_fault_truncate,
    .xSync                  = _nya_db_fault_sync,
    .xFileSize              = _nya_db_fault_file_size,
    .xLock                  = _nya_db_fault_lock,
    .xUnlock                = _nya_db_fault_unlock,
    .xCheckReservedLock     = _nya_db_fault_reserved,
    .xFileControl           = _nya_db_fault_control,
    .xSectorSize            = _nya_db_fault_sector_size,
    .xDeviceCharacteristics = _nya_db_fault_characteristics,
    .xShmMap                = _nya_db_fault_shm_map,
    .xShmLock               = _nya_db_fault_shm_lock,
    .xShmBarrier            = _nya_db_fault_shm_barrier,
    .xShmUnmap              = _nya_db_fault_shm_unmap,
};

// ── vfs methods: open wraps the file, delete can be lost, sleep goes through the registered clock ──

int _nya_db_fault_open(sqlite3_vfs* vfs, const char* name, sqlite3_file* file, int flags, int* out_flags) {
    nya_unused(vfs);

    _NYA_DbFaultFile* ours = (_NYA_DbFaultFile*)file;
    ours->real             = (sqlite3_file*)(ours + 1);

    int code = _NYA_DB_FAULT.real->xOpen(_NYA_DB_FAULT.real, name, ours->real, flags, out_flags);

    // SQLite closes a file whose pMethods is set even when the open failed, so ours is set exactly when the real one's is.
    ours->base.pMethods = ours->real->pMethods != nullptr ? &_NYA_DB_FAULT_METHODS : nullptr;

    return code;
}

int _nya_db_fault_delete(sqlite3_vfs* vfs, const char* name, int sync_directory) {
    nya_unused(vfs);

    // the journal's delete is a rollback-mode commit, so losing it is losing the commit.
    if (_nya_db_fault_take(NYA_DB_FAULT_POWER_LOSS)) return SQLITE_OK;

    return _NYA_DB_FAULT.real->xDelete(_NYA_DB_FAULT.real, name, sync_directory);
}

int _nya_db_fault_sleep(sqlite3_vfs* vfs, int microseconds) {
    nya_unused(vfs);

    // the busy handler's backoff, which under a simulation must move its clock rather than block.
    _nya_db_fault_wait((NYA_Duration){ .ns = (s64)microseconds * (NYA_NS_PER_SECOND / 1'000'000) });

    return microseconds;
}

#endif // NYA_TESTING
