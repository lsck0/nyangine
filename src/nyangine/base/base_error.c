#include "nyangine/base/base_basic.h"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL u32 _nya_error_format_error_trace(const NYA_Error* error, OUT u8* buffer, u32 capacity);
NYA_INTERNAL u32 _nya_error_format_summary(const NYA_Error* error, OUT u8* buffer, u32 capacity);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_error_from_errno(void) {
    NYA_ErrorKind kind;

    switch (errno) {
        case ENOENT:
        case ESRCH:
        case ENXIO:
        case ENODEV:
        case ENOTDIR:      kind = NYA_ERROR_NOT_FOUND; break;

        case EACCES:
        case EPERM:        kind = NYA_ERROR_PERMISSION_DENIED; break;

        case EEXIST:       kind = NYA_ERROR_ALREADY_EXISTS; break;

        case EINVAL:
        case ENAMETOOLONG: kind = NYA_ERROR_INVALID_ARGUMENT; break;

        case ENOMEM:       kind = NYA_ERROR_OUT_OF_MEMORY; break;

        case EIO:          kind = NYA_ERROR_IO; break;

        case ETIMEDOUT:    kind = NYA_ERROR_TIMEOUT; break;

        case ENOSYS:
        case ENOTSUP:      kind = NYA_ERROR_NOT_SUPPORTED; break;

        default:           kind = NYA_ERROR_NOT_OK; break;
    }

    return _nya_error_create(kind, "%s (errno %d)", strerror(errno), errno);
}

u32 nya_error_format(const NYA_Error* error, OUT u8* buffer, u32 capacity) {
    nya_assert(error != nullptr);
    nya_assert(buffer != nullptr);

    if (capacity == 0) return 0;

    u32 length = _nya_error_format_summary(error, buffer, capacity);

    if (error->stack_trace.count > 0 && length + 1 < capacity) {
        s32 written = snprintf((char*)&buffer[length], capacity - length, "\n\nStack Trace:\n");
        if (written > 0) {
            u32 remaining  = capacity - length;
            length        += ((u32)written < remaining) ? (u32)written : remaining - 1;

            if (length + 1 < capacity) length += nya_backtrace_format(&error->stack_trace, &buffer[length], capacity - length);
        }
    }

    return length;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error _nya_error_create(NYA_ErrorKind kind, NYA_ConstCString fmt, ...) {
    nya_assert(fmt != nullptr);
    nya_assert(kind < NYA_ERROR_COUNT);

    NYA_Error error = { .kind = kind };

    va_list args;
    va_start(args, fmt);
    (void)vsnprintf((char*)error.message, sizeof(error.message), fmt, args);
    va_end(args);

#if NYA_ERROR_CAPTURE_STACK
    // Captured here rather than at throw time: by the time an error is thrown the frames it was
    // born in are long gone.
    if (kind != NYA_ERROR_NONE) nya_backtrace_capture(&error.stack_trace, 1);
#endif // NYA_ERROR_CAPTURE_STACK

    return error;
}

void _nya_error_throw(NYA_Error error, NYA_ConstCString function, NYA_ConstCString file, u32 line, NYA_ConstCString format, ...) {
    u8  detail[NYA_CRASH_MESSAGE_MAX_LENGTH];
    u32 length = 0;

    // Caller supplied context first, when there is any.
    if (format[0] != '\0') {
        va_list args;
        va_start(args, format);
        s32 written = vsnprintf((char*)detail, sizeof(detail), format, args);
        va_end(args);

        if (written > 0) {
            length = (u32)written < sizeof(detail) ? (u32)written : (u32)sizeof(detail) - 1;

            if (length + 3 < sizeof(detail)) {
                detail[length++] = '\n';
                detail[length]   = '\0';
            }
        }
    }

    // Then the error itself: kind, message and propagation chain. Deliberately not the stack: it
    // is handed to the sink below instead of being flattened into the message, which keeps the two
    // from being printed twice and from crowding each other out of the buffer.
    if (length + 1 < sizeof(detail)) length += _nya_error_format_summary(&error, &detail[length], (u32)sizeof(detail) - length);

    // The stack the error was *created* with, not the stack of whoever finally threw it. That is
    // the one that says where things actually went wrong.
    const NYA_Backtrace* backtrace = error.stack_trace.count > 0 ? &error.stack_trace : nullptr;

    _nya_crash_raise_with_backtrace(NYA_CRASH_SOURCE_ERROR, function, file, line, (u32)error.kind, backtrace, "%s", (const char*)detail);
}

void _nya_error_push_frame(NYA_Error* error, NYA_ConstCString function, NYA_ConstCString file, u32 line) {
    nya_assert(error != nullptr);
    nya_assert(function != nullptr);
    nya_assert(file != nullptr);

    // Deep propagation chains are truncated rather than overflowing. The innermost frames are the
    // interesting ones, so the tail is what gets dropped.
    if (error->error_trace_count >= NYA_ERROR_TRACE_MAX) return;

    error->error_trace[error->error_trace_count++] = (NYA_BacktraceFrame){
        .address  = 0,
        .function = function,
        .file     = file,
        .line     = line,
    };
}

/** Kind, message and propagation chain. Everything except the captured stack. */
NYA_INTERNAL u32 _nya_error_format_summary(const NYA_Error* error, OUT u8* buffer, u32 capacity) {
    if (capacity == 0) return 0;

    buffer[0] = '\0';

    s32 written = snprintf((char*)buffer, capacity, "%s", NYA_ERRORKIND_NAME_MAP[error->kind]);
    if (written <= 0) return 0;

    u32 length = (u32)written < capacity ? (u32)written : capacity - 1;

    if (error->message[0] != '\0' && length + 1 < capacity) {
        written = snprintf((char*)&buffer[length], capacity - length, ", %s", (const char*)error->message);
        if (written > 0) {
            u32 remaining  = capacity - length;
            length        += ((u32)written < remaining) ? (u32)written : remaining - 1;
        }
    }

    if (length + 1 < capacity) length += _nya_error_format_error_trace(error, &buffer[length], capacity - length);

    return length;
}

NYA_INTERNAL u32 _nya_error_format_error_trace(const NYA_Error* error, OUT u8* buffer, u32 capacity) {
    if (error->error_trace_count == 0 || capacity == 0) return 0;

    s32 written = snprintf((char*)buffer, capacity, "\n\nError Trace:\n");
    if (written <= 0) return 0;

    u32 length = (u32)written < capacity ? (u32)written : capacity - 1;

    for (u32 i = 0; i < error->error_trace_count; i++) {
        const NYA_BacktraceFrame* frame = &error->error_trace[i];

        u32 remaining = capacity - length;
        if (remaining <= 1) break;

        written = snprintf((char*)&buffer[length], remaining, "  #%-2u %s (%s:%u)\n", i, frame->function, frame->file, frame->line);
        if (written <= 0) break;

        length += ((u32)written < remaining) ? (u32)written : remaining - 1;
    }

    return length;
}
