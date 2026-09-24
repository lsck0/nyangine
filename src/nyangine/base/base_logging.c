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
    NYA_LogRecordSink callback;
    void*             user_data;
} _NYA_LogRecordSinkEntry;

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
NYA_INTERNAL _NYA_LogRecordSinkEntry _nya_log_record_sinks[NYA_LOG_SINK_MAX]      = { 0 };
NYA_INTERNAL u32                     _nya_log_record_sink_count                   = 0;
NYA_INTERNAL _NYA_CrashObserverEntry _nya_crash_observers[NYA_CRASH_OBSERVER_MAX] = { 0 };
NYA_INTERNAL u32                     _nya_crash_observer_count                    = 0;

/** What nya_log_tag_set left for this thread. Per thread, so a worker's tag never lands on another's line. */
NYA_INTERNAL thread_local char _nya_log_tag[NYA_LOG_TAG_MAX_LENGTH] = { 0 };

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

/** Appends into a caller's line buffer, bounded and null terminated, advancing `length`. A write that runs out of
 * room is truncated rather than dropped: a human line is read, not parsed, so a cut tail loses nothing a reader
 * cannot see is cut. */
NYA_INTERNAL void _nya_log_line_append(OUT char* buffer, u32 capacity, u32* length, NYA_ConstCString format, ...) __attr_fmt_printf(4, 5);

/** Composes the human line and hands the record to every record sink. The shared tail of both log entry points. */
NYA_INTERNAL void _nya_log_dispatch(
    NYA_LogLevel        level,
    NYA_ConstCString    function,
    NYA_ConstCString    file,
    u32                 line,
    NYA_ConstCString    message,
    const NYA_LogField* fields,
    u32                 count,
    b8                  fields_overflowed
);
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

void nya_log_tag_set(NYA_ConstCString tag) {
    nya_assert(tag != nullptr);

    (void)snprintf(_nya_log_tag, sizeof(_nya_log_tag), "%s", tag);
}

void nya_log_tag_clear(void) {
    _nya_log_tag[0] = '\0';
}

NYA_ConstCString nya_log_tag_get(void) {
    return _nya_log_tag;
}

void nya_log_sink_add(NYA_LogSink sink, void* user_data) {
    // registered on first use, since logging has no init and comes up before everything. Guarded so
    // tests adding and clearing sinks in a loop register once.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("log_sinks", NYA_LOG_SINK_MAX, &_nya_log_sink_count);
        ceiling_registered = true;
    }

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

void nya_log_record_sink_add(NYA_LogRecordSink sink, void* user_data) {
    // See nya_log_sink_add's identical comment; guarded against the registry being full because a program
    // that registers no record sink should never have paid for the ceiling.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        ceiling_registered = true;
        if (nya_ceiling_count() < NYA_CEILING_REGISTRY_MAX) {
            nya_ceiling_register("log_record_sinks", NYA_LOG_SINK_MAX, &_nya_log_record_sink_count);
        }
    }

    if (sink == nullptr) return;
    if (_nya_log_record_sink_count >= NYA_LOG_SINK_MAX) return;

    _nya_log_record_sinks[_nya_log_record_sink_count++] = (_NYA_LogRecordSinkEntry){ .callback = sink, .user_data = user_data };
}

b8 nya_log_record_sink_remove(NYA_LogRecordSink sink, void* user_data) {
    if (sink == nullptr) return false;

    for (u32 i = 0; i < _nya_log_record_sink_count; i++) {
        if (_nya_log_record_sinks[i].callback != sink || _nya_log_record_sinks[i].user_data != user_data) continue;

        // Shifted down rather than swapped, so record sinks stay in registration order as the line sinks do.
        for (u32 j = i + 1; j < _nya_log_record_sink_count; j++) _nya_log_record_sinks[j - 1] = _nya_log_record_sinks[j];

        _nya_log_record_sink_count--;
        _nya_log_record_sinks[_nya_log_record_sink_count] = (_NYA_LogRecordSinkEntry){ 0 };

        return true;
    }

    return false;
}

/*
 * ─────────────────────────────────────────────────────────
 * RING
 * ─────────────────────────────────────────────────────────
 */

/** Copies one rendered line into the ring, evicting the oldest once it is full. */
NYA_INTERNAL void _nya_log_ring_push(NYA_LogLevel level, NYA_ConstCString message, u32 length) {
    /*
     * See nya_log_sink_add's identical comment.
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
    // See nya_log_sink_add's identical comment.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("crash_observers", NYA_CRASH_OBSERVER_MAX, &_nya_crash_observer_count);
        ceiling_registered = true;
    }

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

void _nya_log_line_append(OUT char* buffer, u32 capacity, u32* length, NYA_ConstCString format, ...) {
    if (*length + 1 >= capacity) return;

    u32 room = capacity - *length;

    va_list args;
    va_start(args, format);
    s32 written = vsnprintf(&buffer[*length], room, format, args);
    va_end(args);

    if (written < 0) return;

    *length += ((u32)written < room) ? (u32)written : room - 1;
}

/** One field as ` key=value`, human side. Unquoted: a person reads this, and a machine reads the JSON instead. */
NYA_INTERNAL void _nya_log_field_append_human(OUT char* buffer, u32 capacity, u32* length, const NYA_LogField* field) {
    NYA_ConstCString key = field->key != nullptr ? field->key : "?";

    switch (field->kind) {
        case NYA_LOG_FIELD_STRING:
            _nya_log_line_append(buffer, capacity, length, " %s=%s", key, field->as_string != nullptr ? field->as_string : "(null)");
            break;
        case NYA_LOG_FIELD_INT:   _nya_log_line_append(buffer, capacity, length, " %s=%lld", key, (long long)field->as_int); break;
        case NYA_LOG_FIELD_FLOAT: _nya_log_line_append(buffer, capacity, length, " %s=%g", key, field->as_float); break;
        case NYA_LOG_FIELD_BOOL:  _nya_log_line_append(buffer, capacity, length, " %s=%s", key, field->as_bool ? "true" : "false"); break;
        default:                  break;
    }
}

void _nya_log_dispatch(
    NYA_LogLevel        level,
    NYA_ConstCString    function,
    NYA_ConstCString    file,
    u32                 line,
    NYA_ConstCString    message,
    const NYA_LogField* fields,
    u32                 count,
    b8                  fields_overflowed
) {
    char buffer[NYA_LOG_MESSAGE_MAX_LENGTH];
    s32  written = _nya_log_tag[0] != '\0'
                       ? snprintf(buffer, sizeof(buffer), "[%s] [%s] %s (%s:%u): ", _NYA_LOG_LEVEL_NAME_MAP[level], _nya_log_tag, function, file, line)
                       : snprintf(buffer, sizeof(buffer), "[%s] %s (%s:%u): ", _NYA_LOG_LEVEL_NAME_MAP[level], function, file, line);
    if (written < 0) return;

    u32 length = (u32)written < sizeof(buffer) ? (u32)written : sizeof(buffer) - 1;

    _nya_log_line_append(buffer, sizeof(buffer), &length, "%s", message != nullptr ? message : "");

    for (u32 i = 0; i < count; i++) _nya_log_field_append_human(buffer, sizeof(buffer), &length, &fields[i]);

    // Visible on the human line rather than only in the record: a reader of the ring sees the fields were
    // refused rather than reading a line that quietly carries none.
    if (fields_overflowed) _nya_log_line_append(buffer, sizeof(buffer), &length, "%s", " (fields dropped: over NYA_LOG_FIELD_MAX)");

    _nya_log_emit(level, buffer, length);

    // The structured seam. The human line above is already in the ring and the file, so a record sink is a
    // second rendering of the same event and never the only copy of it.
    if (_nya_log_record_sink_count > 0) {
        NYA_LogRecord record = {
            .level             = level,
            .function          = function,
            .file              = file,
            .line              = line,
            .tag               = _nya_log_tag,
            .message           = message != nullptr ? message : "",
            .fields            = fields,
            .field_count       = count,
            .fields_overflowed = fields_overflowed,
        };

        for (u32 i = 0; i < _nya_log_record_sink_count; i++) _nya_log_record_sinks[i].callback(&record, _nya_log_record_sinks[i].user_data);
    }
}

void _nya_log_message(NYA_LogLevel level, NYA_ConstCString function, NYA_ConstCString file, u32 line, NYA_ConstCString format, ...) {
    if (level < _nya_log_level_current) return;

    // Formatted first, then dispatched as a fieldless record, so a plain nya_log_* line reaches a record
    // sink too, as one JSON object with a message and no fields.
    char    body[NYA_LOG_MESSAGE_MAX_LENGTH];
    va_list args;
    va_start(args, format);
    s32 written = vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    if (written < 0) body[0] = '\0';

    _nya_log_dispatch(level, function, file, line, body, nullptr, 0, false);
}

void _nya_log_fields(
    NYA_LogLevel     level,
    NYA_ConstCString function,
    NYA_ConstCString file,
    u32              line,
    NYA_ConstCString message,
    const NYA_LogField* fields,
    u32              count
) {
    if (level < _nya_log_level_current) return;

    // Refused whole, never a prefix: a truncated set of fields is a record that looks complete and is not.
    b8 overflowed = count > NYA_LOG_FIELD_MAX;
    if (overflowed) {
        fields = nullptr;
        count  = 0;
    }

    _nya_log_dispatch(level, function, file, line, message, fields, count, overflowed);
}

/*
 * ─────────────────────────────────────────────────────────
 * JSON RENDERING
 * ─────────────────────────────────────────────────────────
 *
 * Hand rolled rather than through serde, because base sits below it: a base module cannot reach up to
 * nya_serialize, and a JSON line must render with no allocation anyway. Every append reserves two trailing
 * bytes for the closing brace and the terminator, so a member that does not fit fails cleanly and the object
 * is still closed and still parses. This is the same fail-whole-or-not-at-all discipline http_log's record
 * builder uses, applied a member at a time.
 */

/** Copies `n` bytes when they fit alongside the two reserved bytes. False, having written nothing, otherwise. */
NYA_INTERNAL b8 _nya_log_json_raw(OUT char* out, u64 capacity, u32* length, NYA_ConstCString bytes, u64 n) {
    if ((u64)*length + n + 2 > capacity) return false;

    nya_memcpy(out + *length, bytes, n);
    *length += (u32)n;

    return true;
}

/** Writes `text` as a quoted, escaped JSON string. False, mid-write, when a unit does not fit. */
NYA_INTERNAL b8 _nya_log_json_string(OUT char* out, u64 capacity, u32* length, NYA_ConstCString text) {
    if (text == nullptr) text = "";
    if (!_nya_log_json_raw(out, capacity, length, "\"", 1)) return false;

    for (const u8* p = (const u8*)text; *p != '\0'; p++) {
        switch (*p) {
            case '"':  if (!_nya_log_json_raw(out, capacity, length, "\\\"", 2)) return false; continue;
            case '\\': if (!_nya_log_json_raw(out, capacity, length, "\\\\", 2)) return false; continue;
            case '\n': if (!_nya_log_json_raw(out, capacity, length, "\\n", 2)) return false; continue;
            case '\r': if (!_nya_log_json_raw(out, capacity, length, "\\r", 2)) return false; continue;
            case '\t': if (!_nya_log_json_raw(out, capacity, length, "\\t", 2)) return false; continue;
            default:   break;
        }

        // JSON forbids a raw control character in a string; only a \u escape can carry it.
        if (*p < 0x20) {
            char esc[8];
            s32  written = snprintf(esc, sizeof(esc), "\\u%04x", (u32)*p);
            if (written < 0 || !_nya_log_json_raw(out, capacity, length, esc, (u64)written)) return false;
            continue;
        }

        char c = (char)*p;
        if (!_nya_log_json_raw(out, capacity, length, &c, 1)) return false;
    }

    return _nya_log_json_raw(out, capacity, length, "\"", 1);
}

/** Writes `"key":`, with a leading comma unless it is the first member. */
NYA_INTERNAL b8 _nya_log_json_key(OUT char* out, u64 capacity, u32* length, b8 first, NYA_ConstCString key) {
    if (!first && !_nya_log_json_raw(out, capacity, length, ",", 1)) return false;
    if (!_nya_log_json_string(out, capacity, length, key)) return false;

    return _nya_log_json_raw(out, capacity, length, ":", 1);
}

/** A `"key":"value"` member, rolled back whole when it does not fit so a partial member never lands. */
NYA_INTERNAL b8 _nya_log_json_member_string(OUT char* out, u64 capacity, u32* length, b8* first, NYA_ConstCString key, NYA_ConstCString value) {
    u32 checkpoint = *length;

    if (!_nya_log_json_key(out, capacity, length, *first, key) || !_nya_log_json_string(out, capacity, length, value)) {
        *length = checkpoint;
        return false;
    }

    *first = false;
    return true;
}

/** A `"key":literal` member, the literal already valid JSON (a number or a boolean), written unquoted. */
NYA_INTERNAL b8 _nya_log_json_member_literal(OUT char* out, u64 capacity, u32* length, b8* first, NYA_ConstCString key, NYA_ConstCString literal) {
    u32 checkpoint = *length;

    if (!_nya_log_json_key(out, capacity, length, *first, key) || !_nya_log_json_raw(out, capacity, length, literal, strlen(literal))) {
        *length = checkpoint;
        return false;
    }

    *first = false;
    return true;
}

u32 nya_log_record_render_json(const NYA_LogRecord* record, OUT char* out, u64 capacity) {
    if (out == nullptr || capacity == 0) return 0;

    // Smaller than "{}" and a terminator cannot hold valid JSON, so return the empty string rather than a
    // brace with nowhere to close it.
    if (record == nullptr || capacity < 3) {
        out[0] = '\0';
        return 0;
    }

    u32 length    = 0;
    out[length++] = '{';

    b8 first = true;

    NYA_ConstCString level_name = (u32)record->level < NYA_LOG_LEVEL_COUNT ? _NYA_LOG_LEVEL_NAME_MAP[record->level] : "?";
    _nya_log_json_member_string(out, capacity, &length, &first, "level", level_name);
    _nya_log_json_member_string(out, capacity, &length, &first, "function", record->function);
    _nya_log_json_member_string(out, capacity, &length, &first, "file", record->file);

    char line_text[16];
    (void)snprintf(line_text, sizeof(line_text), "%u", record->line);
    _nya_log_json_member_literal(out, capacity, &length, &first, "line", line_text);

    if (record->tag != nullptr && record->tag[0] != '\0') _nya_log_json_member_string(out, capacity, &length, &first, "tag", record->tag);

    _nya_log_json_member_string(out, capacity, &length, &first, "message", record->message);

    for (u32 i = 0; i < record->field_count; i++) {
        const NYA_LogField* field = &record->fields[i];
        NYA_ConstCString     key   = field->key != nullptr ? field->key : "?";

        b8 ok = true;
        switch (field->kind) {
            case NYA_LOG_FIELD_STRING: ok = _nya_log_json_member_string(out, capacity, &length, &first, key, field->as_string); break;

            case NYA_LOG_FIELD_INT: {
                char value[24];
                (void)snprintf(value, sizeof(value), "%lld", (long long)field->as_int);
                ok = _nya_log_json_member_literal(out, capacity, &length, &first, key, value);
                break;
            }

            case NYA_LOG_FIELD_FLOAT: {
                char value[32];
                (void)snprintf(value, sizeof(value), "%g", field->as_float);
                ok = _nya_log_json_member_literal(out, capacity, &length, &first, key, value);
                break;
            }

            case NYA_LOG_FIELD_BOOL: ok = _nya_log_json_member_literal(out, capacity, &length, &first, key, field->as_bool ? "true" : "false"); break;

            default: break;
        }

        // A member that would overflow stops the fields here rather than writing half of one; the object is
        // still closed below, so a reader always gets a whole record or a shorter whole record, never a torn one.
        if (!ok) break;
    }

    out[length++] = '}';
    out[length]   = '\0';

    return length;
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

    // Opt-in supervised restart, after the report is written and the log flushed: a supervisor armed
    // through NYA_SUPERVISE re-execs this process here rather than letting it die. It returns only when
    // it decides not to — off by default, out of restart budget, or the exec itself failed — and then
    // the crash surfaces through the exit below, exactly as it did before this existed. Safe on the
    // fault path: the re-exec uses only async-signal-safe calls. See base_supervisor.h.
    _nya_supervisor_on_fatal(info->fault_path);

    // _exit on the fault path: atexit handlers and stdio flushing are not async signal safe, and
    // everything we had to say has already gone out through write(2).
    if (info->fault_path) _exit(EXIT_FAILURE);

    exit(EXIT_FAILURE);
}
