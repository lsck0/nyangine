#include "nyangine/base/base_basic.h"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    NYA_LogSink callback;
    void*       user_data;
} _NYA_LogSinkEntry;

typedef struct {
    NYA_CrashObserver callback;
    void*             user_data;
} _NYA_CrashObserverEntry;

#if NYA_DEBUG
NYA_INTERNAL NYA_LogLevel _nya_log_level_current = NYA_LOG_LEVEL_DEBUG;
#else
NYA_INTERNAL NYA_LogLevel _nya_log_level_current = NYA_LOG_LEVEL_INFO;
#endif

NYA_INTERNAL _NYA_LogSinkEntry       _nya_log_sinks[NYA_LOG_SINK_MAX]             = { 0 };
NYA_INTERNAL u32                     _nya_log_sink_count                          = 0;
NYA_INTERNAL _NYA_CrashObserverEntry _nya_crash_observers[NYA_CRASH_OBSERVER_MAX] = { 0 };
NYA_INTERNAL u32                     _nya_crash_observer_count                    = 0;

/** Guards against an observer, or the renderer it drives, crashing inside the crash handler. */
NYA_INTERNAL thread_local u32 _nya_crash_depth = 0;
/** Ensures that when several threads fault at once only the first one reports. */
NYA_INTERNAL atomic b8        _nya_crash_latched = false;

#ifdef NYA_TESTING
NYA_INTERNAL thread_local jmp_buf*      _nya_crash_prevent_jmp    = nullptr;
NYA_INTERNAL thread_local b8            _nya_crash_prevent_caught = false;
NYA_INTERNAL thread_local NYA_CrashInfo _nya_crash_prevent_info   = { 0 };
#endif // NYA_TESTING

/** Buffered log file. Raw descriptor, not stdio, so the crash path can flush it safely. */
#define _NYA_LOG_FILE_BUFFER_SIZE 8192

#if OS_WINDOWS
NYA_INTERNAL HANDLE _nya_log_file = nullptr;
#else
NYA_INTERNAL s32 _nya_log_file = -1;
#endif
NYA_INTERNAL u8  _nya_log_file_buffer[_NYA_LOG_FILE_BUFFER_SIZE];
NYA_INTERNAL u32 _nya_log_file_buffer_length = 0;

/**
 * The buffer is process wide, so concurrent logging would otherwise interleave memcpys and corrupt
 * the length. A spinlock rather than a platform mutex: held for a memcpy at a time, and usable
 * from the crash path where taking a real lock is not something to rely on.
 * */
NYA_INTERNAL atomic_flag _nya_log_file_lock = ATOMIC_FLAG_INIT;

/** Where the daily files go, and how many to keep. Empty directory means daily logging is off. */
#define _NYA_LOG_DIRECTORY_MAX 512
NYA_INTERNAL char _nya_log_directory[_NYA_LOG_DIRECTORY_MAX] = { 0 };
NYA_INTERNAL u32  _nya_log_retention_days                    = 0;

/** UTC days since the epoch of the file currently open, or -1 when none is. Drives the midnight roll. */
NYA_INTERNAL s64 _nya_log_open_day = -1;

NYA_INTERNAL void _nya_log_file_flush_locked(void);

NYA_INTERNAL NYA_ConstCString _NYA_LOG_LEVEL_NAME_MAP[NYA_LOG_LEVEL_COUNT] = {
    [NYA_LOG_LEVEL_TRACE] = "TRACE", [NYA_LOG_LEVEL_DEBUG] = "DEBUG", [NYA_LOG_LEVEL_INFO] = "INFO",
    [NYA_LOG_LEVEL_WARN] = "WARN",   [NYA_LOG_LEVEL_ERROR] = "ERROR", [NYA_LOG_LEVEL_PANIC] = "PANIC",
};

NYA_INTERNAL NYA_ConstCString _NYA_CRASH_SOURCE_NAME_MAP[NYA_CRASH_SOURCE_COUNT] = {
    [NYA_CRASH_SOURCE_ASSERT] = "ASSERTION FAILED",
    [NYA_CRASH_SOURCE_PANIC]  = "PANIC",
    [NYA_CRASH_SOURCE_ERROR]  = "ERROR THROWN",
    [NYA_CRASH_SOURCE_FAULT]  = "FAULT",
};

NYA_INTERNAL void _nya_log_emit(NYA_LogLevel level, NYA_ConstCString message, u32 length);
NYA_INTERNAL void _nya_crash_write_raw(NYA_ConstCString text, u32 length);
NYA_INTERNAL void _nya_crash_report(const NYA_CrashInfo* info);
NYA_INTERNAL void _nya_crash_terminate(const NYA_CrashInfo* info) __attr_noreturn;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_LogLevel nya_log_level_get(void) {
    return _nya_log_level_current;
}

void nya_log_level_set(NYA_LogLevel level) {
    _nya_log_level_current = level;
}

void nya_log_sink_add(NYA_LogSink sink, void* user_data) {
#ifndef NYA_NO_SDL
    // Registered here rather than at some dedicated init: logging has none, coming up before
    // anything else does, and this is the first point the count means anything. Guarded because a
    // test file adding and clearing sinks in a loop should not add a copy of itself each time. Base
    // has no ceiling registry of its own — see core_ceiling.h — so this is skipped under
    // -DNYA_NO_SDL, which excludes core entirely (the build tool itself is one such build).
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("log_sinks", NYA_LOG_SINK_MAX, &_nya_log_sink_count);
        ceiling_registered = true;
    }
#endif

    if (sink == nullptr) return;
    if (_nya_log_sink_count >= NYA_LOG_SINK_MAX) return;

    _nya_log_sinks[_nya_log_sink_count++] = (_NYA_LogSinkEntry){ .callback = sink, .user_data = user_data };
}

void nya_log_sink_clear(void) {
    _nya_log_sink_count = 0;
}

NYA_Error nya_log_file_open(NYA_ConstCString path) {
    nya_log_file_close();
    if (path == nullptr) return NYA_OK;

#if OS_WINDOWS
    _nya_log_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (_nya_log_file == INVALID_HANDLE_VALUE) return nya_error(NYA_ERROR_IO, "could not open the log file '%s'", path);
#else
    _nya_log_file = open(path, O_WRONLY | O_CREAT | O_APPEND, 0o644);
    if (_nya_log_file < 0) return nya_error(NYA_ERROR_IO, "could not open the log file '%s': %s", path, strerror(errno));
#endif

    return NYA_OK;
}

void nya_log_file_flush(void) {
    while (atomic_flag_test_and_set(&_nya_log_file_lock)) {}
    _nya_log_file_flush_locked();
    atomic_flag_clear(&_nya_log_file_lock);
}

NYA_INTERNAL void _nya_log_file_flush_locked(void) {
    if (_nya_log_file_buffer_length == 0) return;

#if OS_WINDOWS
    if (_nya_log_file == nullptr || _nya_log_file == INVALID_HANDLE_VALUE) return;
    DWORD ignored = 0;
    (void)WriteFile(_nya_log_file, _nya_log_file_buffer, (DWORD)_nya_log_file_buffer_length, &ignored, nullptr);
#else
    if (_nya_log_file < 0) return;
    ssize_t ignored = write(_nya_log_file, _nya_log_file_buffer, _nya_log_file_buffer_length);
    nya_unused(ignored);
#endif

    _nya_log_file_buffer_length = 0;
}

void nya_log_file_close(void) {
    nya_log_file_flush();

#if OS_WINDOWS
    if (_nya_log_file != nullptr && _nya_log_file != INVALID_HANDLE_VALUE) (void)CloseHandle(_nya_log_file);
    _nya_log_file = nullptr;
#else
    if (_nya_log_file >= 0) (void)close(_nya_log_file);
    _nya_log_file = -1;
#endif
}

/*
 * Civil date from a day count, after Howard Hinnant's chrono algorithms.
 *
 * Not localtime/gmtime: those read a global timezone, are not reentrant in the form that returns a
 * pointer, and this has to be usable from the crash path. This is a dozen integer operations with no
 * state behind them. Shifts the era so that the leap day lands at the end of a 400 year cycle, which
 * is what makes the month arithmetic below branchless.
 */
NYA_INTERNAL void _nya_log_civil_from_days(s64 days, OUT s32* out_year, OUT u32* out_month, OUT u32* out_day) {
    days += 719'468;

    const s64 era          = (days >= 0 ? days : days - 146'096) / 146'097;
    const u64 day_of_era   = (u64)(days - era * 146'097);
    const u64 year_of_era  = (day_of_era - day_of_era / 1'460 + day_of_era / 36'524 - day_of_era / 146'096) / 365;
    const s64 year         = (s64)year_of_era + era * 400;
    const u64 day_of_year  = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const u64 month_prime  = (5 * day_of_year + 2) / 153;
    const u64 day          = day_of_year - (153 * month_prime + 2) / 5 + 1;
    const u64 month        = month_prime < 10 ? month_prime + 3 : month_prime - 9;

    *out_year  = (s32)(year + (month <= 2 ? 1 : 0));
    *out_month = (u32)month;
    *out_day   = (u32)day;
}

/** UTC days since the epoch, right now. */
NYA_INTERNAL s64 _nya_log_day_now(void) {
    return (s64)(nya_clock_get_timestamp_s() / 86'400);
}

/** Writes `<directory>/YYYY-MM-DD.log` for a day count. */
NYA_INTERNAL void _nya_log_path_for_day(OUT char* buffer, u32 size, s64 day) {
    s32 year  = 0;
    u32 month = 0;
    u32 date  = 0;
    _nya_log_civil_from_days(day, &year, &month, &date);

    (void)snprintf(buffer, size, "%s/%04d-%02u-%02u.log", _nya_log_directory, year, month, date);
}

/**
 * Parses `YYYY-MM-DD.log` back to a day count, or returns false for anything else.
 *
 * Deliberately strict. This decides what the retention sweep is allowed to delete, so a name it does
 * not fully understand has to be a name it leaves alone.
 */
NYA_INTERNAL b8 _nya_log_day_from_name(NYA_ConstCString name, OUT s64* out_day) {
    s32 year  = 0;
    u32 month = 0;
    u32 date  = 0;
    char tail = 0;

    if (sscanf(name, "%4d-%2u-%2u.log%c", &year, &month, &date, &tail) != 3) return false;
    if (month < 1 || month > 12 || date < 1 || date > 31) return false;

    // The inverse of _nya_log_civil_from_days, same era shift.
    const s64 shifted     = year - (month <= 2 ? 1 : 0);
    const s64 era         = (shifted >= 0 ? shifted : shifted - 399) / 400;
    const u64 year_of_era = (u64)(shifted - era * 400);
    const u64 day_of_year = (u64)((153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + date - 1);
    const u64 day_of_era  = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

    *out_day = era * 146'097 + (s64)day_of_era - 719'468;
    return true;
}

/** Deletes every `YYYY-MM-DD.log` in the directory older than the retention window. */
NYA_INTERNAL void _nya_log_retention_sweep(void) {
    if (_nya_log_retention_days == 0) return;

    NYA_Arena arena = nya_arena_create_on_stack(.name = "log_retention");
    defer     nya_arena_destroy_on_stack(&arena);

    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
    NYA_Error                      listed  = nya_filesystem_list(&arena, _nya_log_directory, &entries);
    if (!listed.ok) return;

    // Today counts as one of the retained days, so a 14 day window keeps today and the 13 before it.
    const s64 oldest_kept = _nya_log_day_now() - (s64)_nya_log_retention_days + 1;

    nya_array_foreach (entries, entry) {
        if (entry->type != NYA_FILE_TYPE_FILE) continue;

        NYA_CString name = nya_string_to_cstring(&arena, entry->name);

        s64 day = 0;
        if (!_nya_log_day_from_name(name, &day)) continue;
        if (day >= oldest_kept) continue;

        char path[_NYA_LOG_DIRECTORY_MAX + 32];
        (void)snprintf(path, sizeof(path), "%s/%s", _nya_log_directory, name);

        NYA_Error deleted = nya_filesystem_delete(path);
        if (!deleted.ok) nya_log_warn("Could not delete the expired log file '%s'.", path);
    }
}

NYA_Error nya_log_directory_open(NYA_ConstCString directory, u32 retention_days) {
    if (directory == nullptr) {
        nya_log_file_close();
        _nya_log_directory[0] = 0;
        _nya_log_open_day     = -1;
        return NYA_OK;
    }

    if (!nya_filesystem_is_directory(directory)) NYA_TRY(nya_filesystem_create_directory(directory));

    (void)snprintf(_nya_log_directory, sizeof(_nya_log_directory), "%s", directory);
    _nya_log_retention_days = retention_days;

    const s64 today = _nya_log_day_now();

    char path[_NYA_LOG_DIRECTORY_MAX + 32];
    _nya_log_path_for_day(path, sizeof(path), today);

    NYA_TRY(nya_log_file_open(path));
    _nya_log_open_day = today;

    // After the file is open, so a failure in here is reported into the log it is sweeping around.
    _nya_log_retention_sweep();

    return NYA_OK;
}

void nya_log_directory_roll(void) {
    if (_nya_log_directory[0] == 0) return;

    const s64 today = _nya_log_day_now();
    if (today == _nya_log_open_day) return;

    char path[_NYA_LOG_DIRECTORY_MAX + 32];
    _nya_log_path_for_day(path, sizeof(path), today);

    // Flushes and closes the old file on the way, so nothing written yesterday is lost at the seam.
    NYA_Error opened = nya_log_file_open(path);
    if (!opened.ok) {
        nya_log_warn("Could not roll the log file over to '%s'; continuing in the previous one.", path);
        return;
    }

    _nya_log_open_day = today;

    // A process running across midnight is the one that most needs old files cleaned up.
    _nya_log_retention_sweep();
}

NYA_Error nya_crash_observer_add(NYA_CrashObserver observer, void* user_data) {
#ifndef NYA_NO_SDL
    // See nya_log_sink_add's identical comment, including why this is skipped under -DNYA_NO_SDL.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("crash_observers", NYA_CRASH_OBSERVER_MAX, &_nya_crash_observer_count);
        ceiling_registered = true;
    }
#endif

    if (observer == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "crash observer is null");
    if (_nya_crash_observer_count >= NYA_CRASH_OBSERVER_MAX) {
        return nya_error(NYA_ERROR_NOT_OK, "cannot register more than %d crash observers", NYA_CRASH_OBSERVER_MAX);
    }

    _nya_crash_observers[_nya_crash_observer_count++] = (_NYA_CrashObserverEntry){ .callback = observer, .user_data = user_data };
    return NYA_OK;
}

void nya_crash_observer_clear(void) {
    _nya_crash_observer_count = 0;
}

#ifdef NYA_TESTING
const NYA_CrashInfo* nya_crash_caught(void) {
    return _nya_crash_prevent_caught ? &_nya_crash_prevent_info : nullptr;
}

jmp_buf* _nya_crash_prevent_push(jmp_buf* jmp) {
    jmp_buf* previous = _nya_crash_prevent_jmp;

    _nya_crash_prevent_jmp    = jmp;
    _nya_crash_prevent_caught = false;

    return previous;
}

void _nya_crash_prevent_pop(jmp_buf* previous) {
    _nya_crash_prevent_jmp = previous;
}
#endif // NYA_TESTING

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_log_message(NYA_LogLevel level, NYA_ConstCString function, NYA_ConstCString file, u32 line, NYA_ConstCString format, ...) {
    if (level < _nya_log_level_current) return;

    char buffer[NYA_LOG_MESSAGE_MAX_LENGTH];
    s32  written = snprintf(buffer, sizeof(buffer), "[%s] %s (%s:%u): ", _NYA_LOG_LEVEL_NAME_MAP[level], function, file, line);
    if (written < 0) return;

    u32 length = (u32)written < sizeof(buffer) ? (u32)written : sizeof(buffer) - 1;

    va_list args;
    va_start(args, format);
    written = vsnprintf(&buffer[length], sizeof(buffer) - length, format, args);
    va_end(args);

    if (written > 0) {
        u32 remaining  = (u32)sizeof(buffer) - length;
        length        += ((u32)written < remaining) ? (u32)written : remaining - 1;
    }

    _nya_log_emit(level, buffer, length);
}

void _nya_crash_raise(
    NYA_CrashSource  source,
    NYA_ConstCString function,
    NYA_ConstCString file,
    u32              line,
    u32              error_kind,
    NYA_ConstCString format,
    ...
) {
    NYA_CrashInfo info = {
        .source     = source,
        .function   = function,
        .file       = file,
        .line       = line,
        .error_kind = error_kind,
        .fault_path = false,
    };

    va_list args;
    va_start(args, format);
    (void)vsnprintf((char*)info.message, sizeof(info.message), format, args);
    va_end(args);

    // skip 1 drops _nya_crash_raise itself so the trace starts at whatever panicked.
    nya_backtrace_capture(&info.backtrace, 1);

    _nya_crash_terminate(&info);
}

void _nya_crash_raise_with_backtrace(
    NYA_CrashSource      source,
    NYA_ConstCString     function,
    NYA_ConstCString     file,
    u32                  line,
    u32                  error_kind,
    const NYA_Backtrace* backtrace,
    NYA_ConstCString     format,
    ...
) {
    NYA_CrashInfo info = {
        .source     = source,
        .function   = function,
        .file       = file,
        .line       = line,
        .error_kind = error_kind,
        .fault_path = false,
    };

    va_list args;
    va_start(args, format);
    (void)vsnprintf((char*)info.message, sizeof(info.message), format, args);
    va_end(args);

    if (backtrace != nullptr && backtrace->count > 0) {
        info.backtrace = *backtrace;
    } else {
        nya_backtrace_capture(&info.backtrace, 1);
    }

    _nya_crash_terminate(&info);
}

void _nya_crash_raise_fault(s32 signal, u64 fault_address) {
    NYA_CrashInfo info = {
        .source        = NYA_CRASH_SOURCE_FAULT,
        .function      = "<fault>",
        .file          = "<unknown>",
        .line          = 0,
        .signal        = signal,
        .fault_address = fault_address,
        .fault_path    = true,
    };

    // Nothing here may allocate or touch stdio: we are in async signal context.
    (void)snprintf((char*)info.message, sizeof(info.message), "Fault, signal %d at address 0x%llx", signal, (unsigned long long)fault_address);

    // skip 2 drops _nya_crash_raise_fault and the platform handler that called it.
    nya_backtrace_capture(&info.backtrace, 2);

    _nya_crash_terminate(&info);
}

/** Appends one rendered line to the file buffer, flushing whenever it would not fit. */
NYA_INTERNAL void _nya_log_file_write(NYA_LogLevel level, NYA_ConstCString message, u32 length) {
#if OS_WINDOWS
    if (_nya_log_file == nullptr || _nya_log_file == INVALID_HANDLE_VALUE) return;
#else
    if (_nya_log_file < 0) return;
#endif

    while (atomic_flag_test_and_set(&_nya_log_file_lock)) {}

    // +1 for the newline. A line longer than the whole buffer is truncated rather than split.
    if (_nya_log_file_buffer_length + length + 1 > sizeof(_nya_log_file_buffer)) _nya_log_file_flush_locked();
    if (length + 1 > sizeof(_nya_log_file_buffer)) length = (u32)sizeof(_nya_log_file_buffer) - 1;

    nya_memcpy(&_nya_log_file_buffer[_nya_log_file_buffer_length], message, length);
    _nya_log_file_buffer_length                         += length;
    _nya_log_file_buffer[_nya_log_file_buffer_length++]  = '\n';

    // Anything at WARN or worse is what someone will be reading the file for, and the process may
    // not survive to fill the buffer, so do not let it sit there.
    if (level >= NYA_LOG_LEVEL_WARN) _nya_log_file_flush_locked();

    atomic_flag_clear(&_nya_log_file_lock);
}

NYA_INTERNAL void _nya_log_emit(NYA_LogLevel level, NYA_ConstCString message, u32 length) {
    // stderr rather than stdout: crash output must not sit in a pipe buffer when the process dies.
    (void)fprintf(stderr, "%s\n", message);

    _nya_log_file_write(level, message, length);

    for (u32 i = 0; i < _nya_log_sink_count; i++) { _nya_log_sinks[i].callback(level, message, length, _nya_log_sinks[i].user_data); }
}

NYA_INTERNAL void _nya_crash_write_raw(NYA_ConstCString text, u32 length) {
#if OS_WINDOWS
    DWORD ignored = 0;
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, (DWORD)length, &ignored, nullptr);
#else
    ssize_t ignored = write(STDERR_FILENO, text, length);
    nya_unused(ignored);
#endif
}

/**
 * Renders the crash and hands it to every observer. Split out from _nya_crash_terminate so the
 * ordering rules stay readable: prevention is checked before we ever get here.
 * */
NYA_INTERNAL void _nya_crash_report(const NYA_CrashInfo* info) {
    // Static rather than a local: this is around 11 KiB, and on the fault path we are running on
    // the alternate signal stack where that much is not free. Safe because the reentrancy guard
    // and the crash latch together mean only one thread ever reaches this, exactly once.
    static u8 buffer[NYA_CRASH_MESSAGE_MAX_LENGTH + (NYA_BACKTRACE_DEPTH_MAX * 160)];

    s32 written = snprintf(
        (char*)buffer,
        sizeof(buffer),
        "\n[%s] %s (%s:%u): %s\n\nStack Trace:\n",
        _NYA_CRASH_SOURCE_NAME_MAP[info->source],
        info->function,
        info->file,
        info->line,
        (const char*)info->message
    );
    if (written <= 0) return;

    u32 length = (u32)written < sizeof(buffer) ? (u32)written : (u32)sizeof(buffer) - 1;

    // Composed into one buffer and emitted once, so the report reaches every sink as a single
    // record rather than as a header and a trace that a log file could interleave.
    length += nya_backtrace_format(&info->backtrace, &buffer[length], (u32)sizeof(buffer) - length);

    // The sinks take a line, not a block: trim the trailing newline so nothing adds a second one.
    while (length > 0 && buffer[length - 1] == '\n') buffer[--length] = '\0';

    if (info->fault_path) {
        // Async signal context: bypass stdio and the sinks entirely.
        buffer[length++] = '\n';
        _nya_crash_write_raw((NYA_ConstCString)buffer, length);
    } else {
        _nya_log_emit(NYA_LOG_LEVEL_PANIC, (NYA_ConstCString)buffer, length);
    }

    for (u32 i = 0; i < _nya_crash_observer_count; i++) { _nya_crash_observers[i].callback(info, _nya_crash_observers[i].user_data); }
}

/**
 * The one place the engine dies. Prevention, reentrancy and concurrency are all resolved here so
 * that every crash source shares exactly the same policy.
 * */
NYA_INTERNAL void _nya_crash_terminate(const NYA_CrashInfo* info) {
    // An observer crashing must not loop forever. Second time through, say so and go straight out.
    if (_nya_crash_depth > 0) {
        NYA_ConstCString message = "\n[FATAL] Crashed while handling a crash. Terminating immediately.\n";
        _nya_crash_write_raw(message, (u32)strlen(message));
        // Not the locking flush: whoever held the lock may be the thread that just died.
        _nya_log_file_flush_locked();
        _exit(EXIT_FAILURE);
    }
    _nya_crash_depth++;

#ifdef NYA_TESTING
    // Checked before reporting on purpose: a test that provokes a panic must not fire telemetry or
    // pop a crash window. Faults are never preventable.
    if (_nya_crash_prevent_jmp != nullptr && info->source != NYA_CRASH_SOURCE_FAULT) {
        _nya_crash_prevent_info   = *info;
        _nya_crash_prevent_caught = true;

        jmp_buf* jmp = _nya_crash_prevent_jmp;

        _nya_crash_depth--;

        (void)fprintf(
            stderr,
            "[PREVENTED %s] %s (%s:%u): %s\n",
            _NYA_CRASH_SOURCE_NAME_MAP[info->source],
            info->function,
            info->file,
            info->line,
            (const char*)info->message
        );

        longjmp(*jmp, 1);
    }
#endif // NYA_TESTING

    // Only the first thread to crash gets to report. The rest would race the observers and, on the
    // fault path, race the report writer for the same file.
    b8 expected = false;
    if (!atomic_compare_exchange_strong(&_nya_crash_latched, &expected, true)) _exit(EXIT_FAILURE);

    _nya_crash_report(info);

    // The crash latch means this thread is the only one reporting, and a thread killed mid-append
    // could still be holding the lock, so flush without taking it.
    _nya_log_file_flush_locked();

    if (NYA_EXECUTION_MODE_CURRENT == NYA_EXECUTION_MODE_DEBUG) __builtin_debugtrap();

    // _exit on the fault path: atexit handlers and stdio flushing are not async signal safe, and
    // everything we had to say has already gone out through write(2).
    if (info->fault_path) _exit(EXIT_FAILURE);

    exit(EXIT_FAILURE);
}
