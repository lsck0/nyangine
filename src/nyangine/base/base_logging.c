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
NYA_INTERNAL char _nya_log_directory_path[NYA_LOG_DIRECTORY_MAX] = { 0 };
NYA_INTERNAL u32  _nya_log_retention_days                        = 0;

/**
 * The ring of recent lines. Written by _nya_log_emit, read by a crash report.
 *
 * A flat fixed array rather than base_ring.h's template: the ring has to be usable after an allocator has
 * been corrupted, so it owns its storage outright, and the reader wants lines oldest first rather than a
 * pop that consumes them.
 * */
typedef struct {
    NYA_LogLevel level;
    u8           text[NYA_LOG_RING_LINE_MAX];
} _NYA_LogRingLine;

NYA_INTERNAL _NYA_LogRingLine _nya_log_ring[NYA_LOG_RING_MAX] = { 0 };

/** Where the next line goes, modulo the capacity, and how many slots hold a line. */
NYA_INTERNAL u32 _nya_log_ring_next  = 0;
NYA_INTERNAL u32 _nya_log_ring_count = 0;

/** Its own lock, not the file's: a sink taking a while must not hold the ring against the crash path. */
NYA_INTERNAL atomic_flag _nya_log_ring_lock = ATOMIC_FLAG_INIT;

/** UTC days since the epoch of the file currently open, or -1 when none is. Drives the midnight roll. */
NYA_INTERNAL s64 _nya_log_open_day = -1;

NYA_INTERNAL void _nya_log_file_flush_locked(void);

NYA_INTERNAL NYA_ConstCString _NYA_LOG_LEVEL_NAME_MAP[NYA_LOG_LEVEL_COUNT] = {
    [NYA_LOG_LEVEL_TRACE] = "TRACE", [NYA_LOG_LEVEL_DEBUG] = "DEBUG", [NYA_LOG_LEVEL_INFO] = "INFO",
    [NYA_LOG_LEVEL_WARN] = "WARN",   [NYA_LOG_LEVEL_ERROR] = "ERROR", [NYA_LOG_LEVEL_PANIC] = "PANIC",
};

NYA_INTERNAL void _nya_log_emit(NYA_LogLevel level, NYA_ConstCString message, u32 length);
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
    // registered on first use, since logging has no init and comes up before everything. Guarded so
    // tests adding and clearing sinks in a loop register once. Skipped under -DNYA_NO_SDL, which excludes
    // core and its ceiling registry (the build tool is such a build).
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

b8 nya_log_sink_remove(NYA_LogSink sink, void* user_data) {
    if (sink == nullptr) return false;

    for (u32 i = 0; i < _nya_log_sink_count; i++) {
        if (_nya_log_sinks[i].callback != sink || _nya_log_sinks[i].user_data != user_data) continue;

        // Shifted down rather than swapped with the last, because sinks are notified in registration
        // order and a swap would silently reorder the ones that stay. The list is NYA_LOG_SINK_MAX long,
        // so the move is a handful of entries.
        for (u32 j = i + 1; j < _nya_log_sink_count; j++) _nya_log_sinks[j - 1] = _nya_log_sinks[j];

        _nya_log_sink_count--;
        _nya_log_sinks[_nya_log_sink_count] = (_NYA_LogSinkEntry){ 0 };

        return true;
    }

    return false;
}

void nya_log_sink_clear(void) {
    _nya_log_sink_count = 0;
}

/*
 * ─────────────────────────────────────────────────────────
 * RING
 * ─────────────────────────────────────────────────────────
 */

/** Copies one rendered line into the ring, evicting the oldest once it is full. */
NYA_INTERNAL void _nya_log_ring_push(NYA_LogLevel level, NYA_ConstCString message, u32 length) {
#ifndef NYA_NO_SDL
    /*
     * See nya_log_sink_add's identical comment, including why this is skipped under -DNYA_NO_SDL.
     *
     * Two differences from every other ceiling, both because this is the only one registered from the
     * log path itself. The flag is set before the call, not after, or a registration that logs arrives
     * back here with the flag still false and recurses until the stack runs out. And room is asked for
     * first, because a refusal warns, and a warning is a log line: best effort rather than a warning
     * that would report itself.
     */
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        ceiling_registered = true;
        if (nya_ceiling_count() < NYA_CEILING_REGISTRY_MAX) nya_ceiling_register("log_ring", NYA_LOG_RING_MAX, &_nya_log_ring_count);
    }
#endif

    if (length > NYA_LOG_RING_LINE_MAX - 1) length = NYA_LOG_RING_LINE_MAX - 1;

    while (atomic_flag_test_and_set(&_nya_log_ring_lock)) {}

    _NYA_LogRingLine* line = &_nya_log_ring[_nya_log_ring_next];

    line->level = level;
    nya_memcpy(line->text, message, length);
    line->text[length] = '\0';

    _nya_log_ring_next = (_nya_log_ring_next + 1) % NYA_LOG_RING_MAX;
    if (_nya_log_ring_count < NYA_LOG_RING_MAX) _nya_log_ring_count++;

    atomic_flag_clear(&_nya_log_ring_lock);
}

/** Which slot holds the `index`th oldest line. Only meaningful while `index < _nya_log_ring_count`. */
NYA_INTERNAL u32 _nya_log_ring_slot(u32 index) {
    // Before it wraps the oldest line is slot 0; after, it is wherever the next write is about to land.
    const u32 oldest = _nya_log_ring_count < NYA_LOG_RING_MAX ? 0 : _nya_log_ring_next;
    return (oldest + index) % NYA_LOG_RING_MAX;
}

u32 nya_log_ring_count(void) {
    return _nya_log_ring_count;
}

NYA_ConstCString nya_log_ring_at(u32 index) {
    if (index >= _nya_log_ring_count) return nullptr;

    return (NYA_ConstCString)_nya_log_ring[_nya_log_ring_slot(index)].text;
}

NYA_LogLevel nya_log_ring_level_at(u32 index) {
    if (index >= _nya_log_ring_count) return NYA_LOG_LEVEL_COUNT;

    return _nya_log_ring[_nya_log_ring_slot(index)].level;
}

void nya_log_ring_clear(void) {
    while (atomic_flag_test_and_set(&_nya_log_ring_lock)) {}

    _nya_log_ring_next  = 0;
    _nya_log_ring_count = 0;

    atomic_flag_clear(&_nya_log_ring_lock);
}

NYA_Error nya_log_file_open(NYA_ConstCString path) {
    nya_log_file_close();
    if (path == nullptr) return NYA_OK;

#if OS_WINDOWS
    // shared for writing as well, as O_APPEND is on Linux: a second process logging to the same day's file,
    // a second copy of the game or a crashing child, was refused its log. FILE_APPEND_DATA keeps each write whole.
    _nya_log_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

/** UTC days since the epoch, right now. */
NYA_INTERNAL s64 _nya_log_day_now(void) {
    return (s64)(nya_clock_get_timestamp_s() / NYA_CLOCK_SECONDS_PER_DAY);
}

/** Writes `<directory>/YYYY-MM-DD.log` for a day count. */
NYA_INTERNAL void _nya_log_path_for_day(OUT char* buffer, u32 size, s64 day) {
    s32 year  = 0;
    u32 month = 0;
    u32 date  = 0;
    nya_clock_civil_from_days(day, &year, &month, &date);

    (void)snprintf(buffer, size, "%s/%04d-%02u-%02u.log", _nya_log_directory_path, year, month, date);
}

/**
 * Parses `YYYY-MM-DD.log` back to a day count, or returns false for anything else.
 */
NYA_INTERNAL b8 _nya_log_day_from_name(NYA_ConstCString name, OUT s64* out_day) {
    // exactly "DDDD-DD-DD.log", digits checked before they are converted.
    const char PATTERN[] = "0000-00-00.log";
    for (u32 i = 0; i < sizeof(PATTERN); i++) {
        b8 matches = PATTERN[i] == '0' ? (name[i] >= '0' && name[i] <= '9') : name[i] == PATTERN[i];
        if (!matches) return false;
    }

    s32 year  = ((name[0] - '0') * 1000) + ((name[1] - '0') * 100) + ((name[2] - '0') * 10) + (name[3] - '0');
    u32 month = (u32)(((name[5] - '0') * 10) + (name[6] - '0'));
    u32 date  = (u32)(((name[8] - '0') * 10) + (name[9] - '0'));
    if (month < 1 || month > 12 || date < 1 || date > 31) return false;

    *out_day = nya_clock_days_from_civil(year, month, date);
    return true;
}

/** Deletes every `YYYY-MM-DD.log` in the directory older than the retention window. */
NYA_INTERNAL void _nya_log_retention_sweep(void) {
    if (_nya_log_retention_days == 0) return;

    NYA_Arena arena = nya_arena_create_on_stack(.name = "log_retention");
    defer     nya_arena_destroy_on_stack(&arena);

    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
    NYA_Error                      listed  = nya_filesystem_list(&arena, _nya_log_directory_path, &entries);
    if (!listed.ok) return;

    // Today counts as one of the retained days, so a 14 day window keeps today and the 13 before it.
    const s64 oldest_kept = _nya_log_day_now() - (s64)_nya_log_retention_days + 1;

    nya_array_foreach (entries, entry) {
        if (entry->type != NYA_FILE_TYPE_FILE) continue;

        NYA_CString name = nya_string_to_cstring(&arena, entry->name);

        s64 day = 0;
        if (!_nya_log_day_from_name(name, &day)) continue;
        if (day >= oldest_kept) continue;

        char path[NYA_LOG_DIRECTORY_MAX + 32];
        (void)snprintf(path, sizeof(path), "%s/%s", _nya_log_directory_path, name);

        NYA_Error deleted = nya_filesystem_delete(path);
        if (!deleted.ok) nya_log_warn("Could not delete the expired log file '%s'.", path);
    }
}

NYA_Error nya_log_directory_open(NYA_ConstCString directory, u32 retention_days) {
    if (directory == nullptr) {
        nya_log_file_close();
        _nya_log_directory_path[0] = 0;
        _nya_log_open_day     = -1;
        return NYA_OK;
    }

    if (!nya_filesystem_is_directory(directory)) NYA_TRY(nya_filesystem_create_directory(directory));

    (void)snprintf(_nya_log_directory_path, sizeof(_nya_log_directory_path), "%s", directory);
    _nya_log_retention_days = retention_days;

    const s64 today = _nya_log_day_now();

    char path[NYA_LOG_DIRECTORY_MAX + 32];
    _nya_log_path_for_day(path, sizeof(path), today);

    NYA_TRY(nya_log_file_open(path));
    _nya_log_open_day = today;

    // After the file is open, so a failure in here is reported into the log it is sweeping around.
    _nya_log_retention_sweep();

    return NYA_OK;
}

NYA_ConstCString nya_log_directory(void) {
    return _nya_log_directory_path;
}

void nya_log_directory_roll(void) {
    if (_nya_log_directory_path[0] == 0) return;

    const s64 today = _nya_log_day_now();
    if (today == _nya_log_open_day) return;

    char path[NYA_LOG_DIRECTORY_MAX + 32];
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

b8 nya_crash_observer_remove(NYA_CrashObserver observer, void* user_data) {
    if (observer == nullptr) return false;

    for (u32 i = 0; i < _nya_crash_observer_count; i++) {
        if (_nya_crash_observers[i].callback != observer || _nya_crash_observers[i].user_data != user_data) continue;

        // Shifted down for the reason nya_log_sink_remove shifts: observers run in registration order.
        for (u32 j = i + 1; j < _nya_crash_observer_count; j++) _nya_crash_observers[j - 1] = _nya_crash_observers[j];

        _nya_crash_observer_count--;
        _nya_crash_observers[_nya_crash_observer_count] = (_NYA_CrashObserverEntry){ 0 };

        return true;
    }

    return false;
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

    // Before the sinks, so a sink that crashes leaves the line that provoked it in the report.
    _nya_log_ring_push(level, message, length);

    _nya_log_file_write(level, message, length);

    for (u32 i = 0; i < _nya_log_sink_count; i++) { _nya_log_sinks[i].callback(level, message, length, _nya_log_sinks[i].user_data); }
}

void nya_log_write_stderr(NYA_ConstCString text, u32 length) {
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
        NYA_CRASH_SOURCE_NAME_MAP[info->source],
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
        nya_log_write_stderr((NYA_ConstCString)buffer, length);
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
        nya_log_write_stderr(message, (u32)strlen(message));
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
            NYA_CRASH_SOURCE_NAME_MAP[info->source],
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
