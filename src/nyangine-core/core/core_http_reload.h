/**
 * @file core_http_reload.h
 *
 * One call that makes a server's route handlers survive a code hot reload.
 *
 * ```c
 * nya_http_router_reloadable();   // once at startup, after the callback system is up
 * ```
 *
 * The http router can carry a `handler_callback` token in place of a raw handler pointer, but it
 * cannot turn one back into a function on its own: the named-callback registry that survives a reload
 * lives in core, a rank above http, so the router asks for it through a resolver a program installs
 * with nya_http_router_resolvers_set. This is that install, wrapping nya_callback_get, so a program
 * that wants reload-safe handlers writes one line rather than two resolvers of its own. See
 * http_router.h for the token, core_callback.h for the registry.
 * */
#pragma once

#include "nyangine-std/base/base_basic.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Installs the stock resolvers that turn a route's `handler_callback` / `handler_identified_callback`
 * token back into a function on every dispatch, by asking the named-callback registry (nya_callback_get).
 *
 * Call it once at startup, after nya_system_callback_init, and a route may then carry
 * `.handler_callback = nya_callback(fn)` instead of `.handler = fn` and keep answering after a code
 * reload swaps the image out from under a raw pointer. Routes that use the plain pointers are untouched.
 * */
NYA_API void nya_http_router_reloadable(void);
