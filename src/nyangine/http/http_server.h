/**
 * @file http_server.h
 *
 * The listener: a TCP port, a handful of connections, and one drain a frame that reads whatever has
 * arrived, answers it, and returns. No thread, no blocking call, and nothing allocated or opened
 * until nya_system_http_init is called.
 *
 * ```
 * nya_system_http_init            binds the port and starts accepting
 * nya_system_http_deinit          closes everything; every call below is a no-op again
 * nya_system_http_tick            drains the sockets. Registered on the frame for you
 *
 * nya_http_server_merge           mounts one resource's router at the root
 * nya_http_server_unmerge         the pair. A removed router answers 404 immediately
 *
 * nya_http_server_is_running      whether the port is bound
 * nya_http_server_port            which port, for the line that tells a person where to look
 * nya_http_server_connection_count
 * nya_http_server_request_count   how many requests have been answered since init
 * nya_http_server_router_count / _at   what is mounted, which is what the schema is generated from
 *
 * nya_http_secret_from_environment  a signing secret out of an environment variable, bounded
 * ```
 *
 * ```c
 * u8  secret[NYA_HTTP_MAX_SECRET_BYTES] = { 0 };
 * u64 secret_size                       = 0;
 *
 * // optional: without it the authenticated routes answer 503 and the open ones still work.
 * (void)nya_http_secret_from_environment("NYANGINE_HTTP_SECRET", secret, sizeof(secret), &secret_size);
 *
 * NYA_EXPECT(nya_system_http_init((NYA_HttpConfig){
 *     .port        = 7777,
 *     .secret      = secret,
 *     .secret_size = secret_size,
 *     .layers      = (const NYA_HttpLayerFn[]){ nya_http_layer_log },
 *     .layer_count = 1,
 * }));
 * defer nya_system_http_deinit();
 *
 * NYA_EXPECT(nya_http_server_merge(nya_http_metrics_router()));
 * ```
 *
 * ── what a hostile peer may do ──
 *
 * Anything it likes, and none of it may cost this process more than the fixed buffers in
 * http_types.h. It may connect and send nothing: the connection is dropped after
 * NYA_HTTP_IDLE_TIMEOUT_MS. It may send a byte a second forever: the head has to arrive inside
 * NYA_HTTP_MAX_HEAD_BYTES and inside that timeout, both of which it will fail. It may pipeline: at
 * most NYA_HTTP_MAX_REQUESTS_PER_TICK are answered per frame across every connection, so a peer
 * cannot take the frame. It may stop reading: the answer is queued, and a connection with more than
 * NYA_HTTP_MAX_PENDING_WRITE_BYTES outstanding is dropped rather than buffered further. It may send
 * nonsense: every refusal is a status and a close, and nothing it sends reaches an assertion.
 *
 * What is *not* here, deliberately: TLS, rate limiting per address, and any request bound above the
 * ones in http_types.h. Those belong to a proxy in front. Bind to loopback and put one there.
 *
 * Thread safety: none. Everything here runs on the thread that called init.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/http/http_auth.h"
#include "nyangine/http/http_router.h"
#include "nyangine/http/http_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Longest bind address, terminator included. An IPv6 address in full is 45 characters. */
#define NYA_HTTP_MAX_ADDRESS 48

/**
 * How long a connection may sit without a complete request before it is dropped.
 *
 * Five seconds is far more than a local client needs and far less than a slowloris wants. It is the
 * bound that turns "hold every connection open forever" from an attack into a wait.
 * */
#define NYA_HTTP_IDLE_TIMEOUT_MS 5000

/**
 * Requests answered in one tick, across every connection.
 *
 * The frame is the thing being protected. Sixteen is past anything a dashboard polling once a second
 * produces and is a number a pipelining peer cannot exceed at this program's expense.
 * */
#define NYA_HTTP_MAX_REQUESTS_PER_TICK 16

/**
 * Connections accepted in one tick. Bounded for the same reason: a process that loops on connect gets
 * this much of the frame and no more.
 * */
#define NYA_HTTP_MAX_ACCEPTS_PER_TICK 4

/**
 * Bytes SDL_net may hold queued for one connection before it is dropped.
 *
 * SDL_net's send queue grows to whatever it is handed, so a peer that connects, asks for the OpenAPI
 * document repeatedly and never reads would be an unbounded allocation. This is where that stops:
 * four of the largest response, and then the connection goes.
 * */
#define NYA_HTTP_MAX_PENDING_WRITE_BYTES ((u64)NYA_HTTP_MAX_RESPONSE_BYTES * 4ULL)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_HttpConfig NYA_HttpConfig;

struct NYA_HttpConfig {
    /** Required. */
    u16 port;

    /**
     * What to bind. Empty, which is the default, is loopback only: a metrics interface that is
     * reachable from the network by default is a decision nobody made on purpose.
     * */
    char address[NYA_HTTP_MAX_ADDRESS];

    /** Connections at once, 1..NYA_HTTP_MAX_CONNECTIONS. Zero means the maximum. */
    u32 max_connections;

    /**
     * The token signing secret, copied at init so the caller's buffer can be wiped straight after.
     *
     * Optional: without one, every route with `auth` set answers 503 and the open ones are unaffected,
     * which is what a local debug interface wants. A secret shorter than NYA_HTTP_MIN_SECRET_BYTES is
     * refused at init rather than accepted and quietly useless.
     * */
    const u8* secret;
    u64       secret_size;

    /** Layers around every route, outermost first. Copied; the array need not outlive the call. */
    const NYA_HttpLayerFn* layers;
    u32                    layer_count;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Binds `config.port` and starts accepting, and hooks the drain onto NYA_EVENT_HANDLING_STARTED so a
 * program running the engine's frame loop needs no second call.
 *
 * NYA_ERROR_ALREADY_EXISTS when this program is already listening, NYA_ERROR_IO when the port is
 * taken, NYA_ERROR_INVALID_ARGUMENT for a port of zero or a secret that is present and too short.
 * Fails before anything is allocated, so a failure leaves the subsystem exactly as off as it was.
 * */
NYA_API NYA_Error nya_system_http_init(NYA_HttpConfig config) __attr_no_discard;

/**
 * Closes every connection and unbinds the port. Idempotent, and a no-op when init was never called.
 *
 * Merged routers do not survive it: a router is mounted on a running server, so restarting the server
 * is a fresh mount. That is the opposite of core_control's exposures, and deliberately: an exposure
 * describes the program, a mount describes the server.
 * */
NYA_API void nya_system_http_deinit(void);

/**
 * Accepts, reads, answers and closes, within every bound at the top of this file. Never blocks.
 *
 * Called for you once a frame. Public for the two cases that need it directly: a headless program
 * with no frame loop, and a test driving the exchange one step at a time.
 * */
NYA_API void nya_system_http_tick(void);

/*
 * ─────────────────────────────────────────────────────────
 * ROUTERS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Mounts one resource's router at the root, after checking it with nya_http_router_check.
 *
 * `router` is not copied and must outlive the mount, which a `static const` table does for free.
 * NYA_ERROR_ALREADY_EXISTS for a router already mounted, NYA_ERROR_OUT_OF_MEMORY past
 * NYA_HTTP_MAX_ROUTERS, and whatever nya_http_router_check found for a table that is not well formed.
 * */
NYA_API NYA_Error nya_http_server_merge(const NYA_HttpRouter* router) __attr_no_discard;

/** Removes a mount. A router that was never mounted is a no-op. */
NYA_API void nya_http_server_unmerge(const NYA_HttpRouter* router);

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

NYA_API b8 nya_http_server_is_running(void) __attr_no_discard;

/** The bound port, or zero when the server is not running. */
NYA_API u16 nya_http_server_port(void) __attr_no_discard;

NYA_API u32 nya_http_server_connection_count(void) __attr_no_discard;

/** Requests answered since init, refusals included. Reset by deinit. */
NYA_API u64 nya_http_server_request_count(void) __attr_no_discard;

/** What is mounted. The table http_openapi.h walks to generate the document. */
NYA_API u32                   nya_http_server_router_count(void) __attr_no_discard;
NYA_API const NYA_HttpRouter* nya_http_server_router_at(u32 index) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * SECRETS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Reads a signing secret out of the environment variable `variable`.
 *
 * The environment and nowhere else: a secret in a config file is a secret in the repository one bad
 * commit later, and a default secret is no secret at all. NYA_ERROR_NOT_FOUND when the variable is
 * unset, NYA_ERROR_INVALID_ARGUMENT when its value is shorter than NYA_HTTP_MIN_SECRET_BYTES or
 * longer than `capacity`.
 * */
NYA_API NYA_Error nya_http_secret_from_environment(NYA_ConstCString variable, OUT u8* buffer, u64 capacity, OUT u64* out_size) __attr_no_discard;
