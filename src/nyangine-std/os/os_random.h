/**
 * @file os_random.h
 *
 * The operating system's random source, and nothing else. One function.
 *
 * ```c
 * u8 key[32];
 * if (!nya_os_random_bytes(key, sizeof(key))) {
 *     nya_log_error("the system random source failed; not generating an identity");
 *     return nya_error(NYA_ERROR_NOT_OK, "no entropy");
 * }
 * ```
 *
 * This is not math_random.h. That one is a seeded generator for gameplay: reproducible from a seed,
 * which is the whole point of it. This one cannot be seeded and cannot be replayed, and is what
 * anything another process must not be able to predict comes from: a long term key, a nonce, a
 * websocket masking key.
 *
 * `getrandom` on Linux, `BCryptGenRandom` on Windows. Both draw from the kernel pool, so neither
 * opens a file and neither can be exhausted by a caller.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

/**
 * Most bytes one call may ask for. Every caller in the engine wants a key, a nonce or a mask, so the
 * largest real request is 32 bytes; this leaves two orders of magnitude of headroom and still bounds
 * the loop and the Windows `ULONG` cast. A larger request is a programming mistake, not a runtime
 * condition, so it asserts rather than returning false.
 * */
#define NYA_OS_RANDOM_MAX_BYTES 4096

// FUNCTIONS

/**
 * Fills `out` with `size` unpredictable bytes.
 *
 * False when the operating system's source failed, which is an operating error rather than a reason
 * to crash: a headless box with a starved pool is a real thing, and the caller decides whether it
 * can continue without the bytes. `out` holds nothing meaningful then and must not be used.
 *
 * False as well when `out` is null or `size` is zero or past NYA_OS_RANDOM_MAX_BYTES. Those are
 * programming mistakes rather than runtime conditions, but this layer is below the assertion
 * machinery, so it refuses instead: the caller checks the return either way.
 * */
NYA_API b8 nya_os_random_bytes(OUT u8* out, u64 size) __attr_no_discard;
