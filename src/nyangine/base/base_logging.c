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
    if (_nya_log_file == INVALID_HANDLE_VALUE) return nya_error(NYA_ERROR_IO, "could not open the log file '%s'.", path);
#else
    _nya_log_file = open(path, O_WRONLY | O_CREAT | O_APPEND, 0o644);
    if (_nya_log_file < 0) return nya_error(NYA_ERROR_IO, "could not open the log file '%s': %s.", path, strerror(errno));
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

NYA_Error nya_crash_observer_add(NYA_CrashObserver observer, void* user_data) {
    if (observer == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "crash observer is null.");
    if (_nya_crash_observer_count >= NYA_CRASH_OBSERVER_MAX) {
        return nya_error(NYA_ERROR_NOT_OK, "cannot register more than %d crash observers.", NYA_CRASH_OBSERVER_MAX);
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
