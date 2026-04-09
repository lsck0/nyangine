/**
 * @file base_attributes.h
 *
 * Convenience macros around compiler attributes.
 * This should be synced to `.clang-format`.
 * */
#pragma once

#ifndef __has_attribute
#error "__has_attribute macro not available."
#endif

#if __has_attribute(cleanup)
#define __attr_cleanup(func) __attribute__((cleanup(func)))
#else
#error "attribute 'cleanup' not available."
#endif

#if __has_attribute(constructor)
#define __attr_constructor __attribute__((constructor))
#else
#error "attribute 'constructor' not available."
#endif

#if __has_attribute(destructor)
#define __attr_destructor __attribute__((destructor))
#else
#error "attribute 'destructor' not available."
#endif

#if __has_attribute(format)
#define __attr_fmt_printf(fmt_idx, varargs_idx) __attribute__((format(printf, fmt_idx, varargs_idx)))
#define __attr_fmt_scanf(fmt_idx, varargs_idx)  __attribute__((format(scanf, fmt_idx, varargs_idx)))
#else
#error "attribute 'format' not available."
#endif

#if __has_attribute(malloc)
#define __attr_malloc __attribute__((malloc))
#else
#error "attribute 'malloc' not available."
#endif

#if __has_attribute(matrix_type)
#define __attr_matrix(rows, cols) __attribute__((matrix_type(rows, cols)))
#else
#error "attribute 'matrix_type' not available."
#endif

#if __has_attribute(no_sanitize)
#define __attr_no_sanitize(x) __attribute__((no_sanitize(x)))
#else
#error "attribute 'no_sanitize' not available."
#endif

#if __has_attribute(warn_unused_result)
#define __attr_no_discard __attribute__((warn_unused_result))
#else
#error "attribute 'warn_unused_result' not available."
#endif

#if __has_attribute(noreturn)
#define __attr_noreturn __attribute__((noreturn))
#else
#error "attribute 'noreturn' not available."
#endif

#if __has_attribute(overloadable)
#define __attr_overloaded __attribute__((overloadable))
#else
#error "attribute 'overloadable' not available."
#endif

#if __has_attribute(unused)
#define __attr_allow_unused __attribute__((unused))
#else
#error "attribute 'unused' not available."
#endif

#if __has_attribute(ext_vector_type)
#define __attr_vector(x) __attribute__((ext_vector_type(x)))
#else
#error "attribute 'ext_vector_type' not available."
#endif
