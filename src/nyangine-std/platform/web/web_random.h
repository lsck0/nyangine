/**
 * @file web_random.h
 *
 * The browser's cryptographic random source, and nothing else. One function.
 *
 * ```c
 * u8 key[32];
 * if (!nya_web_random_bytes(key, sizeof(key))) {
 *     nya_log_error("the browser refused entropy; not generating an identity");
 *     return;
 * }
 * ```
 *
 * This is the web sibling of os/os_random.h. It fills a buffer from `crypto.getRandomValues`, the Web
 * Crypto CSPRNG every browser exposes, reached through emscripten's JS bridge. Like os_random it cannot
 * be seeded and cannot be replayed: it is what a nonce, a masking key or a long-term key comes from, not
 * math/math_random.h's seeded generator for gameplay.
 *
 * `crypto.getRandomValues` refuses a request larger than 65536 bytes in one call, so this loops in
 * chunks of that size the way os_random loops `getrandom` — and it keeps os_random's own ceiling of
 * NYA_OS_RANDOM_MAX_BYTES on the whole request, since every real caller wants a key or a nonce and a
 * larger ask is a mistake.
 *
 * Off wasm this answers from `nya_os_random_bytes`, so the same call compiles and runs in the native
 * tree.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Fills `out` with `size` unpredictable bytes from the browser's `crypto.getRandomValues`.
 *
 * False when the browser's source failed, when `out` is null, or when `size` is zero or past
 * NYA_OS_RANDOM_MAX_BYTES — the same refusals os_random makes, checked here because this layer sits
 * below the assertion machinery. `out` holds nothing meaningful on a false return and must not be used.
 * */
NYA_API b8 nya_web_random_bytes(OUT u8* out, u64 size) __attr_no_discard;
