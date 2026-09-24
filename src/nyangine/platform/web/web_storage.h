/**
 * @file web_storage.h
 *
 * A persistent key → bytes store for the client, over the browser's localStorage.
 *
 * ```c
 * u8 token[64];
 * // ... fill token ...
 * nya_web_storage_set("session", token, sizeof(token));
 *
 * u8  read[64];
 * s64 length = nya_web_storage_get("session", read, sizeof(read));
 * if (length >= 0) use(read, (u64)length);
 * ```
 *
 * ── why localStorage, not OPFS ──
 *
 * A wasm module has two ways to keep bytes across page loads: the Web Storage API (localStorage) and the
 * Origin Private File System (OPFS). This uses localStorage, and the reason is the shape of the calls
 * above: they are synchronous. localStorage's `getItem`/`setItem` return a value there and then, which
 * lets these functions return one too — the blocking key/value shape the rest of the engine reads
 * settings and small state in. OPFS's read and write are Promises; blocking on them from the main thread
 * needs either ASYNCIFY (which this build deliberately does not enable, see os/os_wasm.c) or a dedicated
 * worker with `createSyncAccessHandle`, and either one turns a two-line accessor into an async subsystem.
 * The cost side agrees: localStorage is meant for exactly this — a handful of small, durable per-origin
 * values (a session token, a UI preference, a device id) — and its few-megabyte per-origin budget is far
 * past what client state needs. OPFS earns its complexity for large or streamed files, which is a later,
 * separate primitive; the CSR front's small durable state belongs here.
 *
 * Values are arbitrary bytes. localStorage holds strings, so a value is base64-encoded on the way in and
 * decoded on the way out; a caller never sees the encoding. Keys are UTF-8 C strings and are namespaced
 * with a `nya:` prefix inside the store, so this shares an origin with other localStorage users without
 * colliding with them.
 *
 * ── off wasm ──
 *
 * There is no browser to persist to, so the native fallback is an in-process table that lives for the
 * run and no longer: it makes the interface usable and testable in the native tree, but it is not
 * persistent there and does not pretend to be. Real persistence is the wasm localStorage path.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Longest key this stores, terminator included. Keys are short identifiers, never user data. */
#define NYA_WEB_STORAGE_KEY_MAX 128

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Stores `size` bytes of `value` under `key`, replacing whatever was there.
 *
 * False when `key` is null or longer than NYA_WEB_STORAGE_KEY_MAX, when `value` is null with a non-zero
 * `size`, or when the browser refused the write (a full or disabled store). A zero `size` stores an
 * empty value, which `nya_web_storage_get` then reports as length 0 rather than absent.
 * */
NYA_API b8 nya_web_storage_set(NYA_ConstCString key, const u8* value, u64 size) __attr_no_discard;

/**
 * Reads the value stored under `key` into `buffer`, writing at most `capacity` bytes.
 *
 * Returns the value's full length in bytes — which may be larger than `capacity`, in which case only the
 * first `capacity` bytes were written and the caller can size a buffer and read again — or -1 when `key`
 * is absent or invalid. A stored empty value returns 0. Never writes past `capacity`.
 * */
NYA_API s64 nya_web_storage_get(NYA_ConstCString key, OUT u8* buffer, u64 capacity) __attr_no_discard;

/** Removes `key` from the store. True when it was there, false when it was already absent or invalid. */
NYA_API b8 nya_web_storage_delete(NYA_ConstCString key) __attr_no_discard;
