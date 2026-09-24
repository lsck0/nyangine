/**
 * @file http_livereload.h
 *
 * A development-only live reload: a WebSocket the page holds open, a watch that tells one mount of the
 * web bundle from the next, and the one message that turns a rebuild into a reloaded tab.
 *
 * ```
 * nya_http_livereload_available   whether this build has it at all — false in a shipping build
 * nya_http_livereload_route_add     mounts /livereload, the socket the page listens on
 * nya_http_livereload_route_remove  the pair
 * nya_http_livereload_router      the /livereload.js the page includes, to merge like any other router
 *
 * nya_http_livereload_signal      compare a fingerprint to the last one; push "reload" when it moved
 * nya_http_livereload_poll        the same, over nya_http_static_fingerprint — the turnkey call
 * nya_http_livereload_reset       forget the recorded fingerprint, so the next signal is a first sight
 *
 * nya_http_livereload_client_js   the client snippet as a string, which the router serves
 * ```
 *
 * ```c
 * // once, after the server is up and the bundle is mounted
 * if (nya_http_livereload_available()) {
 *     NYA_EXPECT(nya_http_livereload_route_add());
 *     NYA_EXPECT(nya_http_server_merge(nya_http_livereload_router()));
 *     defer nya_http_server_unmerge(nya_http_livereload_router());
 *     defer nya_http_livereload_route_remove();
 * }
 *
 * // every tick, or on the asset system's changed hook: cheap, and pushes only when the bundle moved
 * (void)nya_http_livereload_poll();
 * ```
 *
 * The page opts in with one line the CSP already allows, since the script is same-origin and not inline:
 * `<script src="/livereload.js"></script>`.
 *
 * ── why it is off in a shipping build, and how ──
 *
 * A page that reloads itself when the server's files change is a development tool and a liability
 * anywhere else: it holds a socket open for the life of the tab and reloads on a message anyone who
 * reached the port could send. So the whole of it is compiled out of a release and a Steam build —
 * NYA_SHIPPING_BUILD — where nya_http_livereload_available is a compile-time false, route_add refuses
 * with NYA_ERROR_NOT_SUPPORTED, the router is empty, and a poll pushes nothing. The functions still link,
 * so a program calls them unconditionally and the build decides whether they do anything; guarding the
 * mount with nya_http_livereload_available keeps the socket and the script out of the shipped surface
 * entirely. It rides the same line asset hot reload does: development has it, what ships does not.
 *
 * ── the watch fires on a change, never on a clock ──
 *
 * There is no timer here and nothing polls a socket for news. A signal is a comparison of two numbers:
 * the fingerprint it is handed against the one it recorded last. Equal, and it does nothing; different,
 * and it pushes one "reload" to everyone on /livereload and records the new one. The first fingerprint it
 * ever sees is recorded and nothing is pushed, so a page that just connected is not reloaded out from
 * under itself. That makes the reload a function of the bundle actually having moved, not of time
 * passing, which is the difference between a tool and a tab that flickers.
 *
 * nya_http_static_fingerprint is the fingerprint the turnkey nya_http_livereload_poll reads, so a rebuild
 * that changes a file's bytes changes its hash, changes the fingerprint, and reloads the page — and a
 * poll over a bundle that did not move is a couple of comparisons and no push. A program that watches
 * source files instead of the served bytes folds their modification times into a u64 of its own and hands
 * that to nya_http_livereload_signal; the watch does not care where the number came from, only that it
 * moves exactly when the thing it stands for does.
 *
 * ── thread safety ──
 *
 * None, and none is needed: everything here runs on the thread that calls nya_system_http_tick, the same
 * one the WebSocket server, its routes and nya_http_websocket_broadcast_text already run on. A program
 * polls from its own loop, which is that thread. See http_websocket_server.h.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/http/http_router.h"

// CONSTANTS

/** The WebSocket the page holds open. One stream, push only: the server speaks and the page listens. */
#define NYA_HTTP_LIVERELOAD_PATH "/livereload"

/** Where the client snippet is served, so a page includes it same-origin under the bundle's CSP. */
#define NYA_HTTP_LIVERELOAD_SCRIPT_PATH "/livereload.js"

/** The one text message a change pushes. The client reloads on exactly this and ignores anything else. */
#define NYA_HTTP_LIVERELOAD_MESSAGE "reload"

// FUNCTIONS

/**
 * Whether this build has live reload at all: true in a development build, false in a shipping one.
 *
 * A compile-time constant, so a program guards the mount with it and the socket and the script are gone
 * from a release rather than merely dormant. Everything below is a no-op or a refusal when this is false.
 * */
NYA_API b8 nya_http_livereload_available(void) __attr_no_discard;

/**
 * Mounts /livereload on the running server, so a page may upgrade on it and be pushed to.
 *
 * The server has to be up, as every WebSocket route does; see http_websocket_server.h. NYA_ERROR_NOT_FOUND
 * when it is not, NYA_ERROR_ALREADY_EXISTS when it is already mounted, and NYA_ERROR_NOT_SUPPORTED in a
 * shipping build, where there is nothing to mount.
 * */
NYA_API NYA_Error nya_http_livereload_route_add(void) __attr_no_discard;

/** Unmounts /livereload, closing whoever is on it with 1001. A route that was never mounted is a no-op. */
NYA_API void nya_http_livereload_route_remove(void);

/**
 * The router that serves the client snippet at /livereload.js. Merge it before the server serves, the way
 * every other router here is merged; static storage, so it outlives any mount.
 *
 * Empty in a shipping build, so merging it is harmless and serves nothing.
 * */
NYA_API const NYA_HttpRouter* nya_http_livereload_router(void) __attr_no_discard;

/**
 * Compares `fingerprint` to the one recorded last and, when they differ, pushes NYA_HTTP_LIVERELOAD_MESSAGE
 * to every page on /livereload and records the new one. Returns true exactly when it pushed.
 *
 * The first fingerprint ever seen is recorded and returns false with nothing pushed, so a page is not
 * reloaded the instant it connects. Equal fingerprints return false and push nothing. Always false in a
 * shipping build. This is the whole of the change detection: a program that folds its own watch — source
 * file times, a build id — into a u64 hands it here.
 * */
NYA_API b8 nya_http_livereload_signal(u64 fingerprint);

/**
 * nya_http_livereload_signal over nya_http_static_fingerprint: the turnkey watch of the served bundle.
 *
 * Call it from the program's loop or from the asset system's changed hook. It is a couple of comparisons
 * and no allocation when the bundle has not moved, so calling it every tick costs nothing. Returns what
 * nya_http_livereload_signal returns.
 * */
NYA_API b8 nya_http_livereload_poll(void);

/**
 * Forgets the recorded fingerprint, so the next signal is a first sight again: recorded, not pushed.
 *
 * For a test between cases, and for a program that remounts a different bundle and does not want the swap
 * to read as a change to reload on.
 * */
NYA_API void nya_http_livereload_reset(void);

/**
 * The client snippet, as a NUL-terminated string: a tiny script that opens the /livereload socket,
 * reloads the page on NYA_HTTP_LIVERELOAD_MESSAGE, and reconnects with a bounded backoff if it drops.
 *
 * What nya_http_livereload_router serves. Exposed so a test can check it and a program that would rather
 * bundle it than serve it can. Empty in a shipping build.
 * */
NYA_API NYA_ConstCString nya_http_livereload_client_js(void) __attr_no_discard;
