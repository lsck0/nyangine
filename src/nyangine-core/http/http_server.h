/**
 * @file http_server.h
 *
 * The listener: a TCP port, a handful of connections, and either one drain a frame or a thread of its
 * own with a pool of workers behind it. Nothing is allocated, opened or started until
 * nya_system_http_init is called.
 *
 * ```
 * nya_system_http_init            binds the port and starts accepting
 * nya_system_http_deinit          closes everything; every call below is a no-op again
 * nya_system_http_tick            drains the sockets, or the main thread's share of them
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
 * ── one drain a frame, or a thread ──
 *
 * `workers` in the config picks, and zero is the default:
 *
 * - **Zero.** What this has always been. nya_system_http_tick accepts, reads, answers and closes on
 *   whoever calls it, at most NYA_HTTP_MAX_REQUESTS_PER_TICK requests per call, and returns. No
 *   thread exists, so nothing is concurrent, nothing needs a lock and the whole exchange is
 *   reproducible step by step. This is the mode a test, a simulation and a program with no
 *   concurrency problem to solve want.
 * - **One or more.** A listener thread owns the sockets: it accepts, reads, parses, spends the rate
 *   limit token, hands the parsed request to a worker, and writes what comes back. `workers` worker
 *   threads run the layers, the extractor and the handler, each on its own arena. Requests are
 *   answered as fast as they arrive rather than as often as the frame runs, and an Argon2id hash or a
 *   slow query costs the frame nothing.
 *
 * **A route says which thread it runs on** through NYA_HttpAffinity: NYA_HTTP_AFFINITY_WORKER, the
 * default, or NYA_HTTP_AFFINITY_MAIN for a handler that reads or writes program state. A MAIN
 * exchange is queued and answered inside nya_system_http_tick, so a threaded server still has to be
 * ticked — from the frame, which init hooks for you, or from the program's own loop.
 *
 * **The promise a worker route makes is enforced, not documented.** The modules that belong to the
 * frame call nya_thread_main_only (base_thread.h), so a handler that forgot its MAIN crashes the first
 * time it runs, naming what it reached for, rather than corrupting a table under the frame.
 *
 * ── what a hostile peer may do ──
 *
 * Anything it likes, and none of it may cost this process more than the fixed buffers in
 * http_types.h. A thread does not raise a single bound: everything below holds in both modes, and
 * every bound is global to the server rather than per worker, because the connection table, the rate
 * limit buckets and the counters all live on the listener thread and are only ever touched there.
 *
 * It may connect and send nothing: the connection is dropped after NYA_HTTP_IDLE_TIMEOUT_MS. It may
 * send a byte a second forever: the head has to arrive inside NYA_HTTP_MAX_HEAD_BYTES and inside that
 * timeout, both of which it will fail. It may pipeline: a connection has at most one exchange in
 * flight and the next request is not even parsed until the last one is written, and at most
 * NYA_HTTP_MAX_REQUESTS_PER_TICK exchanges start per drain pass, so the in-flight work is bounded by
 * the connection table however fast a peer sends. It may stop reading: the answer is queued, and a
 * connection with more than NYA_HTTP_MAX_PENDING_WRITE_BYTES outstanding is dropped rather than
 * buffered further. It may send nonsense: every refusal is a status and a close, and nothing it sends
 * reaches an assertion.
 *
 * What a slow handler can hold up: its own connection, which is not read from or timed out while its
 * answer is being written, and — for a MAIN route only — the frame. What it cannot hold up: every
 * other connection, the accept loop, the idle timeouts, or shutdown past the deadline below.
 *
 * A connection that asks to be upgraded stops being an HTTP one and becomes a WebSocket on the same
 * socket, in the same slot, under the same per address cap; see http_websocket_server.h, which is where
 * the handshake and the bounds on what follows it are. **A WebSocket belongs to the tick in both
 * modes**: the handshake is answered on the ticking thread and every frame after it is read, dispatched
 * and written there, so `on_open`, `on_message` and `on_close` run where a program's own state lives and
 * nya_http_websocket_broadcast_text stays a call the frame makes. The listener thread hands the socket
 * over at the 101 and never touches it again.
 *
 * One address may hold NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS connections; the next is closed at
 * accept. Each address spends one token per request from a bucket of `request_burst` refilled at
 * `requests_per_second`; an empty bucket answers 429 with Retry-After and closes. The address is the
 * socket's peer, never a forwarded header, so behind a proxy every client shares the proxy's budget.
 *
 * What is *not* here yet: TLS, and any request bound above the ones in http_types.h. Until TLS
 * lands, bind to loopback and put a proxy in front.
 *
 * ── thread safety ──
 *
 * With `workers` at zero: none, and everything runs on the thread that drains.
 *
 * With workers: nya_system_http_init, nya_system_http_deinit and nya_system_http_tick are the main
 * thread's, and merging or unmerging a router, the introspection calls and nya_http_websocket_* are
 * safe to call from it while the server runs. A worker sees the routers as they were when its
 * exchange was queued, so a merge never changes the table under a dispatch. Everything else — the
 * connection table, the buckets, the sockets — is the listener thread's alone.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/http/http_auth.h"
#include "nyangine-core/http/http_router.h"
#include "nyangine-core/http/http_types.h"

// CONSTANTS

/**
 * How long a connection may sit without a complete request before it is dropped.
 *
 * Five seconds is far more than a local client needs and far less than a slowloris wants. It is the
 * bound that turns "hold every connection open forever" from an attack into a wait.
 * */
#define NYA_HTTP_IDLE_TIMEOUT_MS 5000

/**
 * Requests answered, or started, in one drain pass across every connection.
 *
 * The frame is the thing being protected. Sixteen is past anything a dashboard polling once a second
 * produces and is a number a pipelining peer cannot exceed at this program's expense. A threaded
 * server counts the same way on its listener thread, so a pass starts at most this many exchanges and
 * a tick answers at most this many NYA_HTTP_AFFINITY_MAIN ones.
 * */
#define NYA_HTTP_MAX_REQUESTS_PER_TICK 16

/**
 * Worker threads one server may run, whatever the config or the core count says.
 *
 * Each worker costs an exchange arena and a stack and can only ever be working on one request, so
 * this is also the number of handlers that can be running at once. Eight is more parallelism than a
 * machine serving one program's own interface has work for, and the connection table is eight.
 * */
#define NYA_HTTP_MAX_WORKERS 8

/**
 * How long nya_system_http_deinit waits for a handler that is still running before it stops waiting.
 *
 * Two seconds is far longer than any handler that is not broken and short enough that a program's
 * shutdown is not held hostage by one. Past it the sockets are closed and the port is given back
 * anyway, and the state the stuck handler is still writing into is deliberately leaked rather than
 * freed under it; see nya_system_http_deinit.
 * */
#define NYA_HTTP_SHUTDOWN_GRACE_MS 2000

/**
 * Connections accepted in one tick. Bounded for the same reason: a process that loops on connect gets
 * this much of the frame and no more.
 * */
#define NYA_HTTP_MAX_ACCEPTS_PER_TICK 4

/**
 * Bytes this server may hold queued for one connection before it is dropped.
 *
 * A peer decides how fast it reads, so a peer that connects, asks for the OpenAPI
 * document repeatedly and never reads would be an unbounded allocation. This is where that stops:
 * four of the largest response, and then the connection goes.
 * */
#define NYA_HTTP_MAX_PENDING_WRITE_BYTES ((u64)NYA_HTTP_MAX_RESPONSE_BYTES * 4ULL)

// TYPES

typedef struct NYA_HttpConfig NYA_HttpConfig;

struct NYA_HttpConfig {
    /** Required. */
    u16 port;

    /**
     * What to bind. Empty, which is the default, is loopback only: a metrics interface that is
     * reachable from the network by default is a decision nobody made on purpose.
     * */
    char address[NYA_HTTP_MAX_ADDRESS];

    /**
     * Worker threads, 0..NYA_HTTP_MAX_WORKERS, clamped rather than refused.
     *
     * Zero, the default, is one drain a frame on the thread that ticks: no thread is started, nothing
     * is concurrent, and a test or a simulation gets the same bytes in the same order every run. Any
     * other number starts a listener thread and that many workers; see the note at the top of this
     * file for what a route may then do, and NYA_HttpAffinity for how it says so.
     *
     * Each worker adds an exchange's worth of fixed buffers, so the memory a server holds is
     * `max_connections` request and response buffers either way plus one arena per worker.
     * */
    u32 workers;

    /** Connections at once, 1..NYA_HTTP_MAX_CONNECTIONS. Zero means the maximum. */
    u32 max_connections;

    /** Connections one address may hold, 1..max_connections. Zero means NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS. */
    u32 max_connections_per_address;

    /**
     * An address's request budget: refilled at this many a second, holding at most `request_burst`. Past it
     * the answer is 429 with Retry-After and the connection closes. Zero means the NYA_HTTP_DEFAULT_ one.
     * */
    u32 requests_per_second;
    u32 request_burst;

    /**
     * The token signing secret, copied at init so the caller's buffer can be wiped straight after.
     *
     * Optional: without one, every route with `auth` set answers 503 and the open ones are unaffected,
     * which is what a local debug interface wants. A secret shorter than NYA_HTTP_MIN_SECRET_BYTES is
     * refused at init rather than accepted and quietly useless.
     * */
    const u8* secret;
    u64       secret_size;

    /**
     * The PEM certificate chain and private key to serve TLS with, or empty for plain HTTP.
     *
     * Both or neither: a path for one and not the other is refused at init rather than starting a
     * server that is not the one asked for. With them, every connection is TLS and there is no plain
     * HTTP port beside it — a server that answered both on one port would be a server whose security
     * depends on which one a client happened to speak.
     *
     * A build with no TLS library refuses them too, saying so, rather than quietly serving plaintext
     * on a port whose name says https; see tls.h.
     * */
    NYA_ConstCString certificate_path;
    NYA_ConstCString key_path;

    /** Layers around every route, outermost first. Copied; the array need not outlive the call. */
    const NYA_HttpLayerFn* layers;
    u32                    layer_count;
};

// FUNCTIONS

// SYSTEM FUNCTIONS

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
 * Bounded, and it returns with nothing of this server's still running. The listener thread is asked to
 * stop and joined first, so no socket is read, written or accepted after that point and none is left
 * half closed; then the workers are woken and given NYA_HTTP_SHUTDOWN_GRACE_MS between them to finish
 * the handler they are inside. A handler that has finished by then is joined normally and everything
 * is freed.
 *
 * A handler still running at the deadline cannot be stopped from outside — there is no safe way to
 * kill a thread that may be holding a lock — so the deadline is honoured the only way that is sound:
 * the sockets are closed and the port is given back, the thread is detached, and this server's arena
 * and its exchange buffers are leaked on purpose rather than freed while that handler is still writing
 * into them. It is logged as an error naming how many, because it is a bug in the handler.
 *
 * Merged routers do not survive it: a router is mounted on a running server, so restarting the server
 * is a fresh mount. That is the opposite of core_control's exposures, and deliberately: an exposure
 * describes the program, a mount describes the server.
 * */
NYA_API void nya_system_http_deinit(void);

/**
 * With no workers: accepts, reads, answers and closes, within every bound at the top of this file.
 * With workers: answers the NYA_HTTP_AFFINITY_MAIN exchanges the listener has queued, and drains the
 * WebSockets. Never blocks either way.
 *
 * Called for you once a frame, by the app's own "http" system, where input is drained and for the same
 * reason the control socket is there: a request is input like a keypress, so it lands at the same point
 * in the frame. The app calls down into this module rather than this module registering a hook up in the
 * app, because core sits above http and an HTTP server that reaches into the frame loop cannot be linked
 * without one.
 *
 * So a program with no app drives this itself, which is the whole of what a headless tool or a test has
 * to do differently, and there is no window and no frame loop hiding in the requirement to serve.
 * */
NYA_API void nya_system_http_tick(void);

// ROUTERS

/**
 * Mounts one resource's router at the root, after checking it with nya_http_router_check.
 *
 * Safe to call on a running threaded server: an exchange already in flight keeps the table it was
 * queued against, so a mount or an unmount decides the next request rather than this one.
 *
 * `router` is not copied and must outlive the mount, which a `static const` table does for free.
 * NYA_ERROR_ALREADY_EXISTS for a router already mounted, NYA_ERROR_OUT_OF_MEMORY past
 * NYA_HTTP_MAX_ROUTERS, and whatever nya_http_router_check found for a table that is not well formed.
 * */
NYA_API NYA_Error nya_http_server_merge(const NYA_HttpRouter* router) __attr_no_discard;

/** Removes a mount. A router that was never mounted is a no-op. */
NYA_API void nya_http_server_unmerge(const NYA_HttpRouter* router);

// INTROSPECTION

NYA_API b8 nya_http_server_is_running(void) __attr_no_discard;

/** The bound port, or zero when the server is not running. */
NYA_API u16 nya_http_server_port(void) __attr_no_discard;

NYA_API u32 nya_http_server_connection_count(void) __attr_no_discard;

/** Requests answered since init, refusals included. Reset by deinit. */
NYA_API u64 nya_http_server_request_count(void) __attr_no_discard;

/** What is mounted. The table http_openapi.h walks to generate the document. */
NYA_API u32                   nya_http_server_router_count(void) __attr_no_discard;
NYA_API const NYA_HttpRouter* nya_http_server_router_at(u32 index) __attr_no_discard;

// SECRETS

/**
 * Reads a signing secret out of the environment variable `variable`.
 *
 * The environment and nowhere else: a secret in a config file is a secret in the repository one bad
 * commit later, and a default secret is no secret at all. NYA_ERROR_NOT_FOUND when the variable is
 * unset, NYA_ERROR_INVALID_ARGUMENT when its value is shorter than NYA_HTTP_MIN_SECRET_BYTES or
 * longer than `capacity`.
 * */
NYA_API NYA_Error nya_http_secret_from_environment(NYA_ConstCString variable, OUT u8* buffer, u64 capacity, OUT u64* out_size) __attr_no_discard;
