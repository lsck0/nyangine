#if OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "nyangine/base/base_basic.h"
#include "nyangine/nyangine.h"

#if NYA_BACKTRACE_SUPPORTED
#include "backtrace.h"
#endif

#if OS_LINUX
#include <signal.h>
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Alternate stack size. Must stay generous: a stack overflow is symbolized on this stack. */
#define _NYA_BACKTRACE_ALT_STACK_SIZE (SIGSTKSZ * 4)

NYA_INTERNAL b8 _nya_backtrace_initialized = false;

#if NYA_BACKTRACE_SUPPORTED
NYA_INTERNAL struct backtrace_state* _nya_backtrace_state = nullptr;

/** Cursor state threaded through the libbacktrace callbacks. */
typedef struct {
    NYA_Backtrace* backtrace;
} _NYA_BacktraceCursor;

NYA_INTERNAL void _nya_backtrace_error_callback(void* data, const char* message, int error_number);
NYA_INTERNAL int  _nya_backtrace_frame_callback(void* data, uintptr_t pc, const char* file, int line, const char* function);
#endif // NYA_BACKTRACE_SUPPORTED

NYA_INTERNAL void _nya_backtrace_install_fault_handlers(void);
NYA_INTERNAL void _nya_backtrace_restore_fault_handlers(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_backtrace_init(void) {
    if (_nya_backtrace_initialized) return;
    _nya_backtrace_initialized = true;

#if NYA_BACKTRACE_SUPPORTED
    // A null filename makes libbacktrace resolve the running executable itself.
    _nya_backtrace_state = backtrace_create_state(nullptr, true, _nya_backtrace_error_callback, nullptr);

    // Warm the symbolization state. Parsing debug info lazily inside a fault handler would mean
    // mmapping and allocating in async signal context, so we pay that cost here instead.
    NYA_Backtrace warmup = { 0 };
    nya_backtrace_capture(&warmup, 0);
#endif // NYA_BACKTRACE_SUPPORTED

    _nya_backtrace_install_fault_handlers();
}

void nya_backtrace_deinit(void) {
    if (!_nya_backtrace_initialized) return;
    _nya_backtrace_initialized = false;

    _nya_backtrace_restore_fault_handlers();

    // The symbolization state is deliberately kept. It is never freed by libbacktrace anyway, and
    // a crash during shutdown should still produce a readable trace.
}

void nya_backtrace_capture(OUT NYA_Backtrace* out_backtrace, u32 skip) {
    nya_assert(out_backtrace != nullptr);

    out_backtrace->count = 0;

#if NYA_BACKTRACE_SUPPORTED
    if (_nya_backtrace_state == nullptr) return;

    // +1 drops nya_backtrace_capture itself, so skip == 0 means "start at my caller".
    _NYA_BacktraceCursor cursor = { .backtrace = out_backtrace };
    (void)backtrace_full(_nya_backtrace_state, (int)skip + 1, _nya_backtrace_frame_callback, _nya_backtrace_error_callback, &cursor);
#else
    nya_unused(skip);
#endif // NYA_BACKTRACE_SUPPORTED
}

u32 nya_backtrace_format(const NYA_Backtrace* backtrace, OUT u8* buffer, u32 capacity) {
    nya_assert(backtrace != nullptr);
    nya_assert(buffer != nullptr);

    if (capacity == 0) return 0;

    buffer[0]  = '\0';
    u32 length = 0;

    if (backtrace->count == 0) {
        s32 written = snprintf((char*)buffer, capacity, "  <no stack trace available>\n");
        if (written <= 0) return 0;

        // Clamped, like the loop below already does. snprintf reports what it *would* have written,
        // so a capacity under the length of that string returned a count past the end of the buffer
        // — and the header promises bytes actually written. A caller adding it to an offset then
        // indexes outside its own buffer. _nya_crash_report is the only caller today and has about
        // ten kibibytes spare, so this was a latent contract break rather than a live overflow.
        return (u32)written < capacity ? (u32)written : capacity - 1;
    }

    for (u32 i = 0; i < backtrace->count; i++) {
        const NYA_BacktraceFrame* frame = &backtrace->frames[i];

        u32 remaining = capacity - length;
        if (remaining <= 1) break;

        s32 written = snprintf(
            (char*)&buffer[length],
            remaining,
            "  #%-2u %s\n        %s:%u\n",
            i,
            frame->function ? frame->function : "??",
            frame->file ? frame->file : "??",
            frame->line
        );
        if (written <= 0) break;

        // snprintf returns what it *would* have written, so clamp on truncation.
        length += ((u32)written < remaining) ? (u32)written : remaining - 1;
    }

    return length;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if NYA_BACKTRACE_SUPPORTED
NYA_INTERNAL void _nya_backtrace_error_callback(void* data, const char* message, int error_number) {
    // Symbolization failures must never themselves crash or allocate. A frame we cannot resolve is
    // simply left out, so there is nothing useful to do here.
    nya_unused(data, message, error_number);
}

NYA_INTERNAL int _nya_backtrace_frame_callback(void* data, uintptr_t pc, const char* file, int line, const char* function) {
    _NYA_BacktraceCursor* cursor    = (_NYA_BacktraceCursor*)data;
    NYA_Backtrace*        backtrace = cursor->backtrace;

    if (backtrace->count >= NYA_BACKTRACE_DEPTH_MAX) return 1; // non-zero stops the walk

    // The strings belong to libbacktrace's debug info mapping and outlive every caller, so storing
    // the pointers is safe and keeps this function allocation free.
    backtrace->frames[backtrace->count++] = (NYA_BacktraceFrame){
        .address  = (u64)pc,
        .function = function,
        .file     = file,
        .line     = (u32)(line > 0 ? line : 0),
    };

    return 0;
}
#endif // NYA_BACKTRACE_SUPPORTED

/*
 * ─────────────────────────────────────────────────────────
 * FAULT HANDLERS
 * ─────────────────────────────────────────────────────────
 */

#if OS_LINUX

NYA_INTERNAL const int _NYA_BACKTRACE_FAULT_SIGNALS[]    = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
NYA_INTERNAL const u32 _NYA_BACKTRACE_FAULT_SIGNAL_COUNT = sizeof(_NYA_BACKTRACE_FAULT_SIGNALS) / sizeof(_NYA_BACKTRACE_FAULT_SIGNALS[0]);

/** Alternate stack, kept alive for the whole process. A stack overflow runs the handler on it. */
NYA_INTERNAL u8 _nya_backtrace_alt_stack[_NYA_BACKTRACE_ALT_STACK_SIZE];

NYA_INTERNAL void _nya_backtrace_fault_handler(int signal_number, siginfo_t* info, void* context) {
    nya_unused(context);

    // Reset to the default action immediately. If anything below faults again the process dies
    // straight away instead of re-entering this handler forever.
    (void)signal(signal_number, SIG_DFL);

    u64 fault_address = (info != nullptr) ? (u64)(uintptr_t)info->si_addr : 0;
    _nya_crash_raise_fault((s32)signal_number, fault_address);
}

NYA_INTERNAL void _nya_backtrace_install_fault_handlers(void) {
    // Run the handler on its own stack so a stack overflow is still reportable. Without this the
    // handler would need stack space that a stack overflow by definition does not have.
    stack_t alt_stack = {
        .ss_sp    = _nya_backtrace_alt_stack,
        .ss_size  = sizeof(_nya_backtrace_alt_stack),
        .ss_flags = 0,
    };
    (void)sigaltstack(&alt_stack, nullptr);

    struct sigaction action = { 0 };
    action.sa_sigaction     = _nya_backtrace_fault_handler;
    action.sa_flags         = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

    for (u32 i = 0; i < _NYA_BACKTRACE_FAULT_SIGNAL_COUNT; i++) { (void)sigaction(_NYA_BACKTRACE_FAULT_SIGNALS[i], &action, nullptr); }
}

NYA_INTERNAL void _nya_backtrace_restore_fault_handlers(void) {
    for (u32 i = 0; i < _NYA_BACKTRACE_FAULT_SIGNAL_COUNT; i++) { (void)signal(_NYA_BACKTRACE_FAULT_SIGNALS[i], SIG_DFL); }
}

#elif OS_WINDOWS

NYA_INTERNAL LPTOP_LEVEL_EXCEPTION_FILTER _nya_backtrace_previous_filter = nullptr;

NYA_INTERNAL LONG WINAPI _nya_backtrace_fault_filter(EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;

    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_ILLEGAL_INSTRUCTION: break;
        default:                            return EXCEPTION_CONTINUE_SEARCH;
    }

    // For an access violation the second parameter holds the address that was touched.
    u64 fault_address = 0;
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        fault_address = (u64)info->ExceptionRecord->ExceptionInformation[1];
    }

    _nya_crash_raise_fault((s32)code, fault_address);
}

NYA_INTERNAL void _nya_backtrace_install_fault_handlers(void) {
    // SetUnhandledExceptionFilter rather than a vectored handler on purpose: a first chance
    // vectored hook would steal exceptions that SDL and GPU drivers raise and handle internally.
    _nya_backtrace_previous_filter = SetUnhandledExceptionFilter(_nya_backtrace_fault_filter);
}

NYA_INTERNAL void _nya_backtrace_restore_fault_handlers(void) {
    (void)SetUnhandledExceptionFilter(_nya_backtrace_previous_filter);
    _nya_backtrace_previous_filter = nullptr;
}

#else

NYA_INTERNAL void _nya_backtrace_install_fault_handlers(void) {
    // No fault interception on this platform.
}

NYA_INTERNAL void _nya_backtrace_restore_fault_handlers(void) {
    // No fault interception on this platform.
}

#endif // OS_LINUX
