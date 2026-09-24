/**
 * @file web_fetch.h
 *
 * One HTTP request over the browser's `fetch`, as a polled handle rather than a blocking call.
 *
 * ```c
 * NYA_WebFetch* request = nya_web_fetch_create(arena, "GET", "/api/state", nullptr, 0, nullptr);
 *
 * // once per frame, until it settles:
 * NYA_WebFetchStatus status = nya_web_fetch_poll(request);
 * if (status == NYA_WEB_FETCH_DONE) {
 *     u8  body[4096];
 *     s64 length = nya_web_fetch_body(request, body, sizeof(body));
 *     handle(nya_web_fetch_status_code(request), body, length);
 * }
 * if (status != NYA_WEB_FETCH_PENDING) nya_web_fetch_destroy(request);
 * ```
 *
 * ── why a polled seam, not http/http_client's transport ──
 *
 * The engine already has a typed client (http/http_client.h): a route in, a DTO out, over a
 * NYA_HttpClientTransport a program supplies. That transport is *blocking* — its `perform` returns the
 * reply before the call returns. `fetch` cannot be blocking: it returns a Promise, and a wasm module
 * with no ASYNCIFY (this one, see os/os_wasm.c) cannot wait on it without freezing the tab. So this is
 * the minimal async seam the client's blocking shape rules out: `create` kicks the request off and
 * returns at once, and `poll` reports where it got to, driven from the frame loop the way the engine
 * drives everything else. A later step can wrap this as a NYA_HttpClientTransport that a cooperative
 * caller pumps, so the typed client rides the same seam; that wrapper is not this file.
 *
 * The handle is arena-owned: it is valid until `nya_web_fetch_destroy`, which also releases the
 * browser-side response it was holding. Destroying without polling to a settled state is allowed and
 * abandons the in-flight request.
 *
 * Off wasm there is no `fetch`; the native tree has http_client and the curl plugin for real requests.
 * The fallback here refuses — `create` returns null — so this stays a browser-only seam that still
 * compiles natively.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_WebFetch NYA_WebFetch;

/** Where an in-flight request has got to. */
typedef enum {
    /** The request is on the wire; poll again next frame. */
    NYA_WEB_FETCH_PENDING,

    /** A reply arrived. `nya_web_fetch_status_code` and `nya_web_fetch_body` are readable now. */
    NYA_WEB_FETCH_DONE,

    /** The request failed before any reply — a network error, a refused connection, a CORS refusal. */
    NYA_WEB_FETCH_FAILED,
} NYA_WebFetchStatus;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts a `fetch` for `method` and `url` and returns a handle to poll, or null when `arena`, `method`
 * or `url` is null, when a body is given with no `content_type` to describe it, or off wasm where there
 * is no `fetch`. `body`/`body_size` is the request body, or null/0 for a request that carries none;
 * `content_type` is its media type (`"application/json"`), or null when there is no body. The bytes are
 * copied into the browser at the call, so `body` need not outlive it.
 * */
NYA_API NYA_WebFetch*
nya_web_fetch_create(NYA_Arena* arena, NYA_ConstCString method, NYA_ConstCString url, const u8* body, u64 body_size, NYA_ConstCString content_type)
    __attr_no_discard;

/** Releases `fetch` and the browser-side reply it holds, abandoning the request if it is still in flight. Null is a no-op. */
NYA_API void nya_web_fetch_destroy(NYA_WebFetch* fetch);

/** Where `fetch` has got to. NYA_WEB_FETCH_FAILED for a null handle, so a caller can treat null as a failed request. */
NYA_API NYA_WebFetchStatus nya_web_fetch_poll(NYA_WebFetch* fetch) __attr_no_discard;

/** The HTTP status the reply carried, meaningful once poll has returned NYA_WEB_FETCH_DONE; 0 before then or for a null handle. */
NYA_API u32 nya_web_fetch_status_code(const NYA_WebFetch* fetch) __attr_no_discard;

/**
 * Copies the reply body into `buffer`, at most `capacity` bytes, and returns its full length — which may
 * exceed `capacity`, so a caller can size a buffer and read again — or -1 before the reply is done or for
 * a null handle. Never writes past `capacity`.
 * */
NYA_API s64 nya_web_fetch_body(const NYA_WebFetch* fetch, OUT u8* buffer, u64 capacity) __attr_no_discard;
