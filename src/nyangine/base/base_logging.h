#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum {
    NYA_LOG_LEVEL_TRACE,
    NYA_LOG_LEVEL_DEBUG,
    NYA_LOG_LEVEL_INFO,
    NYA_LOG_LEVEL_WARN,
    NYA_LOG_LEVEL_ERROR,
    NYA_LOG_LEVEL_PANIC,
    NYA_LOG_LEVEL_COUNT,
} NYA_LogLevel;

/**
 * Panic Hook Function
 *
 * This will be executed before the panic function crashes the program.
 * If it returns true the crash will be prevented.
 * */
typedef u8 (*NYA_PanicHook)(const char* function, const char* file, u32 line, const char* format, va_list args);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#define nya_trace(format, ...) _nya_log_message(NYA_LOG_LEVEL_TRACE, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_debug(format, ...) _nya_log_message(NYA_LOG_LEVEL_DEBUG, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_info(format, ...)  _nya_log_message(NYA_LOG_LEVEL_INFO, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_warn(format, ...)  _nya_log_message(NYA_LOG_LEVEL_WARN, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_error(format, ...) _nya_log_message(NYA_LOG_LEVEL_ERROR, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))
#define nya_panic(format, ...) _nya_log_message(NYA_LOG_LEVEL_PANIC, __FUNCTION__, __FILE__, __LINE__, format __VA_OPT__(, __VA_ARGS__))

NYA_API NYA_EXTERN NYA_LogLevel nya_log_level_get(void);
NYA_API NYA_EXTERN void         nya_log_level_set(NYA_LogLevel level);
NYA_API NYA_EXTERN void         nya_panic_hook_set(NYA_PanicHook hook);
NYA_API NYA_EXTERN void         nya_panic_prevent_set(jmp_buf* jmp);
NYA_API NYA_EXTERN b8           nya_panic_prevent_happened(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_API NYA_EXTERN void _nya_log_message(NYA_LogLevel level, const char* function, const char* file, u32 line, const char* format, ...)
    __attr_fmt_printf(5, 6);
