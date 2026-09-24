/**
 * @file web.h
 *
 * The browser primitives the client-side (CSR) path stands on, gathered under one include.
 *
 * These are the non-renderer half of the web platform: the small, orthogonal seams a wasm module needs
 * from the host it runs in — a clock, a random source, a persistent store, the two async I/O seams (fetch
 * and a client WebSocket), and the canvas input queue. Each is its own file and its own concern, sibling
 * to os/os_wasm.c (which
 * answers the page/time/random the headless serialize demo reaches) but pitched at the client rather than
 * the allocator: `performance.now` over `clock_gettime`, `crypto.getRandomValues` framed as a CSPRNG,
 * localStorage as key/value, `fetch` and `WebSocket` as polled seams.
 *
 * Every one has a native fallback so the whole set compiles and, where it can, runs in the native tree:
 * the clock and random answer from os/, the store keeps a run-lifetime table, and the two I/O seams
 * refuse (there is no `fetch` or browser `WebSocket` off wasm — the native tree has http_client, the curl
 * plugin and the net transports for that). Nothing here is a renderer; a WebGL backend is a separate
 * effort that sits on top of these.
 * */
#pragma once

#include "nyangine/platform/web/web_clock.h"
#include "nyangine/platform/web/web_fetch.h"
#include "nyangine/platform/web/web_input.h"
#include "nyangine/platform/web/web_random.h"
#include "nyangine/platform/web/web_socket.h"
#include "nyangine/platform/web/web_storage.h"
