#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_ceiling.h"
#include "nyangine-std/base/base_compare.h"
#include "nyangine-std/base/base_logging.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-std/base/base_thread.h"
#include "nyangine-core/http/http_server.h"
#include "nyangine-std/base/base_clock.h"
#include "nyangine-std/os/os_random.h"
#include "nyangine-std/os/os_time.h"

// CONSTANTS

/**
 * How long the listener thread sleeps between passes while anything is in flight.
 *
 * A worker finishing is not something a socket wait can be woken by, so while there is an answer to write
 * the listener polls instead of sleeping on the sockets. One millisecond is the same order as the job
 * scheduler's busy tick and is below what a client can measure.
 * */
#define _NYA_HTTP_LISTENER_BUSY_MS 1

/** And how long it waits on the sockets when there is nothing outstanding, so an idle server is free. */
#define _NYA_HTTP_LISTENER_IDLE_MS 20

/** How long a worker sits on the semaphore before looking at the stop flag again. */
#define _NYA_HTTP_WORKER_WAIT_MS 20

// TYPES

typedef struct _NYA_HttpConnection _NYA_HttpConnection;
typedef struct _NYA_HttpSlot       _NYA_HttpSlot;
typedef struct _NYA_HttpState      _NYA_HttpState;

/**
 * Where one exchange has got to, which is also who owns its slot.
 *
 * The listener fills a slot and publishes _QUEUED; whoever runs it publishes _RUNNING and then _DONE;
 * the listener writes the answer and publishes _IDLE. Nobody touches a slot outside the state it owns,
 * so this one atomic is the whole handover and there is no lock on the path a request takes.
 * */
typedef enum {
    /** Nobody's. The listener may fill it. */
    _NYA_HTTP_SLOT_IDLE = 0,

    /** Filled and waiting for a worker or for the tick. */
    _NYA_HTTP_SLOT_QUEUED,

    /** Somebody is inside the layers and the handler. */
    _NYA_HTTP_SLOT_RUNNING,

    /** Answered. The listener's again, to write and release. */
    _NYA_HTTP_SLOT_DONE,
} _NYA_HttpSlotState;

/** What the listener is supposed to do with a finished slot. */
typedef enum {
    /** Write the response that is in it. */
    _NYA_HTTP_ANSWER_WRITE = 0,

    /** Nothing: the 101 went out where the answer would have, and the connection is a WebSocket now. */
    _NYA_HTTP_ANSWER_UPGRADED,
} _NYA_HttpAnswer;

/** One accepted connection and the bytes of a request that have arrived on it so far. */
struct _NYA_HttpConnection {
    NYA_OsSocket socket;

    /** What has arrived and not yet been consumed. Bounded by its own size and nothing else. */
    u8  received[NYA_HTTP_MAX_REQUEST_BYTES];
    u64 received_size;

    /**
     * What has been answered and not yet taken by the host.
     *
     * A socket takes what fits in its own buffer and no more, so a peer that stops reading leaves the
     * rest here. That is what makes the bound below a bound on this program rather than a hope about
     * the host: past NYA_HTTP_MAX_PENDING_WRITE_BYTES outstanding the connection goes, which is the
     * same rule the answer path has always had, now that the queue is this program's own.
     * */
    u8  sending[NYA_HTTP_MAX_PENDING_WRITE_BYTES];
    u64 sending_size;
    u64 sent;

    /** Monotonic nanoseconds of the last byte read or answer written; drives the idle timeout. */
    u64 active_at_ns;

    /** Answered with `Connection: close`, so it is dropped as soon as the answer is queued. */
    b8 closing;

    /** Answered with a 101, so the bytes on it are frames and http_websocket_server.c owns them. */
    b8 upgraded;

    /** The peer, as the socket reports it. What the per address limits key on, and never a forwarded header. */
    char address[NYA_HTTP_MAX_ADDRESS];

    /**
     * The TLS session over this socket, or null on a server with no certificate.
     *
     * Everything above this field is the same either way: the bytes in `received` are plaintext
     * whichever they arrived as, so the parser, the router and every bound never learn there was TLS.
     * The two places that do are _nya_http_receive and _nya_http_flush.
     * */
    NYA_TlsSession* tls;

    /**
     * The handshake asked to write rather than to read, which is the one time a connection with
     * nothing queued still has to be woken on writability. See _nya_http_wait.
     * */
    b8 tls_wants_write;
};

/**
 * One exchange being answered: the parsed request, the buffers it is answered out of, and the arena it
 * may allocate from.
 *
 * One per connection with workers, so the table is what bounds the work in flight and two exchanges
 * never share a byte; one for the whole server without them, which is what it has always been.
 * */
struct _NYA_HttpSlot {
    /** Emptied at the start of every exchange. Nothing here outlives the request it was built for. */
    NYA_Arena* arena;

    NYA_HttpRequest  request;
    NYA_HttpResponse response;
    u8               response_body[NYA_HTTP_MAX_RESPONSE_BYTES];

    /**
     * The mounted routers as they were when this exchange was queued.
     *
     * Copied rather than read through the server, so a merge from the frame never changes the table
     * under a handler and a worker needs no lock to route.
     * */
    const NYA_HttpRouter* routers[NYA_HTTP_MAX_ROUTERS];
    u32                   router_count;

    /** The peer's address, copied for the same reason the routers are. */
    char address[NYA_HTTP_MAX_ADDRESS];

    /** What the peer had already sent past this request, which is what refuses a frame before the 101. */
    u64 trailing;

    NYA_HttpStatus  status;
    _NYA_HttpAnswer answer;

    b8 keep_alive;
    b8 head_only;

    /** The handshake, answered where the WebSocket table lives rather than on a worker. */
    b8 upgrade;

    /** The connection goes once this answer is out, whatever it said about keep-alive. */
    b8 close_after;

    /** The handover. See _NYA_HttpSlotState. */
    atomic u32 state;
};

/** One address's request budget: a token bucket, refilled continuously and spent one token per request. */
typedef struct {
    char address[NYA_HTTP_MAX_ADDRESS];
    f64  tokens;
    u64  refilled_at_ns;
} _NYA_HttpRateBucket;

struct _NYA_HttpState {
    NYA_Arena* allocator;

    NYA_OsSocket listener;

    u16 port;
    u32 max_connections;
    u32 max_connections_per_address;
    u32 requests_per_second;
    u32 request_burst;

    _NYA_HttpRateBucket buckets[NYA_HTTP_MAX_RATE_BUCKETS];
    u32                 bucket_count;

    /**
     * What every connection's TLS session is made from, or null on a server with no certificate.
     *
     * One context for the listener rather than one per connection: it holds the certificate, the key
     * and the cipher list, none of which is per peer. See tls.h.
     * */
    NYA_TlsContext* tls;

    /** Copied from the config, so the caller may wipe theirs. Zeroed by deinit. */
    u8  secret[NYA_HTTP_MAX_SECRET_BYTES];
    u64 secret_size;

    NYA_HttpLayerFn layers[NYA_HTTP_MAX_LAYERS];
    u32             layer_count;

    const NYA_HttpRouter* routers[NYA_HTTP_MAX_ROUTERS];
    u32                   router_count;

    _NYA_HttpConnection connections[NYA_HTTP_MAX_CONNECTIONS];

    /** One per connection when threaded, one in total when not. See _NYA_HttpSlot. */
    _NYA_HttpSlot* slots;
    u32            slot_count;

    /** What nya_ceiling_register publishes, so the overlay can show how full the table is. */
    atomic u32 connection_count;

    atomic u64 request_count;

    // ── the threaded half, all of it null and zero while `workers` is zero ──

    /** Worker threads, and therefore handlers that can be running at once. Zero is the unthreaded mode. */
    u32 workers;

    NYA_Thread*    listener_thread;
    NYA_Thread*    worker_threads[NYA_HTTP_MAX_WORKERS];
    NYA_Semaphore* work;

    /** Guards the two queues below and nothing else. Taken after `table_mutex`, never before it. */
    NYA_Mutex* queue_mutex;

    /** Guards the connection table, the connection count and the mounted routers. */
    NYA_Mutex* table_mutex;

    /** Connection indices waiting to be answered. One entry per connection at most, so the table bounds them. */
    u32 worker_queue[NYA_HTTP_MAX_CONNECTIONS];
    u32 worker_queued;
    u32 main_queue[NYA_HTTP_MAX_CONNECTIONS];
    u32 main_queued;

    /** Set by deinit; every thread here checks it and returns. */
    atomic b8 stopping;
};

// STATE

/** Null when the server is off, which is what makes every call here a no-op and costs nothing. */
NYA_INTERNAL _NYA_HttpState* _NYA_HTTP = nullptr;

// PRIVATE API DECLARATION

/** One drain pass: accept, then read, answer and time out every connection once. */
NYA_INTERNAL void _nya_http_pass(void);

/** Takes whatever the listener has, up to NYA_HTTP_MAX_ACCEPTS_PER_TICK. */
NYA_INTERNAL void _nya_http_accept(void);

/** Reads what has arrived on one connection. False when the connection is finished with. */
NYA_INTERNAL b8 _nya_http_receive(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot) __attr_no_discard;

/**
 * Queues `size` bytes for `connection` and sends what the host will take.
 *
 * False when the peer is gone or owes more than NYA_HTTP_MAX_PENDING_WRITE_BYTES, which is the bound
 * on what one connection may make this program hold: a peer that asks for the largest answer over and
 * over and never reads is the case it exists for.
 * */
/**
 * Moves a connection's TLS handshake along, and answers whether the connection is still worth keeping.
 *
 * True for a handshake that finished *and* for one that is still going: both are a connection that has
 * done nothing wrong. False is a handshake that failed or a peer that went away, which the caller
 * answers by closing. A connection with no TLS on it is true and costs nothing.
 * */
NYA_INTERNAL b8 _nya_http_tls_ready(_NYA_HttpConnection* connection) __attr_no_discard;

NYA_INTERNAL b8 _nya_http_push(_NYA_HttpConnection* connection, const u8* data, u64 size) __attr_no_discard;

/**
 * Pushes whatever is queued for `connection` into the socket, and compacts what is left.
 *
 * False when the connection has failed. A partial write is the ordinary case and not a failure: the
 * host takes what fits in its buffer, and the rest waits for the next pass.
 * */
NYA_INTERNAL b8 _nya_http_flush(_NYA_HttpConnection* connection) __attr_no_discard;

/** Parses and answers, or queues, as many complete requests as `budget` allows. False when the connection is finished. */
NYA_INTERNAL b8 _nya_http_handle(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot, u32* budget) __attr_no_discard;

/** Runs the layers, the extractor and the handler into `slot`. What a worker and the tick both call. */
NYA_INTERNAL void _nya_http_dispatch(_NYA_HttpState* state, _NYA_HttpSlot* slot);

/** Dispatches one parsed request and writes the answer, on one thread. False when the connection is finished. */
NYA_INTERNAL b8 _nya_http_answer(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot) __attr_no_discard;

/** Writes what a finished slot holds and hands the slot back. False when the connection is finished. */
NYA_INTERNAL b8 _nya_http_complete(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot) __attr_no_discard;

/** Renders a status with a problem body into `slot`'s response. A nonzero `retry_after_s` becomes `Retry-After`. */
NYA_INTERNAL void _nya_http_problem(_NYA_HttpSlot* slot, NYA_HttpStatus status, NYA_ConstCString detail, u32 retry_after_s);

/** Writes one status with a problem body and nothing else, for a request that never became one. */
NYA_INTERNAL void _nya_http_refuse(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot, NYA_HttpStatus status, NYA_ConstCString detail, u32 retry_after_s);

/** Answers the WebSocket handshake. Runs where the WebSocket table lives, which is the ticking thread. */
NYA_INTERNAL void _nya_http_upgrade(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot);

/** A fresh request id: 64 bits from the CSPRNG as hex. */
NYA_INTERNAL void _nya_http_request_id(const _NYA_HttpState* state, OUT char* out);

/** Spends one token from `address`'s bucket. False when it is empty, with the seconds until one refills. */
NYA_INTERNAL b8 _nya_http_rate_take(NYA_ConstCString address, OUT u32* out_retry_after_s) __attr_no_discard;

/** Renders and queues a response. False when the socket has failed. */
NYA_INTERNAL b8 _nya_http_write(_NYA_HttpConnection* connection, const NYA_HttpResponse* response, NYA_HttpStatus status, b8 keep_alive, b8 head_only)
    __attr_no_discard;

/** Closes one connection and frees its slot. Idempotent. */
NYA_INTERNAL void _nya_http_close(_NYA_HttpConnection* connection);

/* ── the threaded half ── */

/** Hands a filled slot to a worker, or to the tick when its route says so. */
NYA_INTERNAL void _nya_http_queue(_NYA_HttpState* state, u32 index, _NYA_HttpSlot* slot);


/** Whether this exchange has to run where the program's own state is. */
NYA_INTERNAL b8 _nya_http_runs_on_main(const _NYA_HttpSlot* slot) __attr_no_discard;

NYA_INTERNAL void _nya_http_queue_push(u32* queue, u32* count, u32 index);
NYA_INTERNAL b8   _nya_http_queue_pop(u32* queue, u32* count, OUT u32* out_index) __attr_no_discard;

/** Answers the exchanges that asked for the main thread, up to the per tick budget. */
NYA_INTERNAL void _nya_http_main_drain(void);

/** Reads, dispatches and flushes every upgraded connection. The tick's in both modes; see the header. */
NYA_INTERNAL void _nya_http_websockets_drain(void);

/** Sleeps between passes: on the sockets when there is nothing outstanding, briefly when there is. */
NYA_INTERNAL void _nya_http_listener_wait(_NYA_HttpState* state);

NYA_INTERNAL void _nya_http_listener_thread(void* data);
NYA_INTERNAL void _nya_http_worker_thread(void* data);

/** Starts the listener and the pool. Leaves nothing running when it fails. */
NYA_INTERNAL NYA_Error _nya_http_threads_start(_NYA_HttpState* state) __attr_no_discard;

/** Joins every worker inside NYA_HTTP_SHUTDOWN_GRACE_MS and returns how many were still inside a handler. */
NYA_INTERNAL u32 _nya_http_workers_join(_NYA_HttpState* state) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

// SYSTEM FUNCTIONS

/**
 * The address a config binds: what it named, or loopback when it named nothing. The default is
 * 127.0.0.1 and not every interface, so a server nobody asked to be reachable off the box — a metrics
 * endpoint above all — is not. Opting out is naming a public address on purpose. One place, so the
 * default cannot drift between where it is chosen and where it is tested.
 * */
NYA_INTERNAL NYA_ConstCString _nya_http_bind_address(const NYA_HttpConfig* config) {
    nya_assert(config != nullptr);
    return config->address[0] != '\0' ? config->address : "127.0.0.1";
}

NYA_Error nya_system_http_init(NYA_HttpConfig config) {
    if (_NYA_HTTP != nullptr) return nya_error(NYA_ERROR_ALREADY_EXISTS, "the HTTP server is already running");

    if (config.port == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an HTTP server needs a port");

    // A secret that's present but too short is refused here rather than at the first request: a server that comes up and only fails when someone logs in is a server nobody tested.
    if (config.secret != nullptr && config.secret_size > 0 &&
        (config.secret_size < NYA_HTTP_MIN_SECRET_BYTES || config.secret_size > NYA_HTTP_MAX_SECRET_BYTES)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a signing secret is %d to %d bytes", NYA_HTTP_MIN_SECRET_BYTES, NYA_HTTP_MAX_SECRET_BYTES);
    }

    if (config.layer_count > NYA_HTTP_MAX_LAYERS) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a server carries at most %d root layers", NYA_HTTP_MAX_LAYERS);
    }

    b8 wants_certificate = config.certificate_path != nullptr && config.certificate_path[0] != '\0';
    b8 wants_key         = config.key_path != nullptr && config.key_path[0] != '\0';

    if (wants_certificate != wants_key) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "TLS needs both a certificate and a key, or neither");
    }

    if (wants_certificate && !nya_tls_available()) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "this build has no TLS library, and will not serve plaintext in its place");
    }

    if (nya_os_socket_start() != NYA_OS_SOCKET_OK) return nya_error(NYA_ERROR_NOT_OK, "the host's socket library could not start");

    // Loopback unless the caller named something else, so a server nobody asked to be reachable isn't; resolved here not on a thread — this is a start-up call, and a name that won't resolve is a server that won't start either way.
    NYA_ConstCString requested = _nya_http_bind_address(&config);

    NYA_OsAddress address = { 0 };

    if (nya_os_address_resolve(requested, config.port, NYA_OS_ADDRESS_NONE, &address) != NYA_OS_SOCKET_OK) {
        NYA_Error failed = nya_error(NYA_ERROR_IO, "'%s' is not an address this machine can bind", requested);

        nya_os_socket_stop();

        return failed;
    }

    NYA_OsSocket       listener = NYA_OS_SOCKET_NONE;
    NYA_OsSocketStatus opened   = nya_os_socket_open_at(NYA_OS_SOCKET_LISTENER, address, 0, &listener);

    if (opened != NYA_OS_SOCKET_OK) {
        NYA_Error failed = opened == NYA_OS_SOCKET_IN_USE ? nya_error(NYA_ERROR_IO, "port %u is already taken", (u32)config.port)
                                                          : nya_error(NYA_ERROR_IO, "port %u could not be bound", (u32)config.port);

        nya_os_socket_stop();

        return failed;
    }

    NYA_Arena* arena = nya_arena_create(.name = "http");

    if (arena == nullptr) {
        nya_os_socket_close(listener);
        nya_os_socket_stop();

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the HTTP server");
    }

    _NYA_HttpState* state = nya_arena_alloc(arena, sizeof(_NYA_HttpState));

    if (state == nullptr) {
        nya_arena_destroy(arena);
        nya_os_socket_close(listener);
        nya_os_socket_stop();

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the HTTP server");
    }

    // field by field after a memset: a compound literal of the whole state is a 200 KB stack temporary unoptimized.
    nya_memset(state, 0, sizeof(*state));
    state->allocator = arena;
    state->listener  = listener;
    state->port      = config.port;
    state->max_connections =
        config.max_connections == 0 || config.max_connections > NYA_HTTP_MAX_CONNECTIONS ? NYA_HTTP_MAX_CONNECTIONS : config.max_connections;
    state->max_connections_per_address = config.max_connections_per_address != 0 ? config.max_connections_per_address : NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS;
    state->requests_per_second         = config.requests_per_second != 0 ? config.requests_per_second : NYA_HTTP_DEFAULT_REQUESTS_PER_SECOND;
    state->request_burst               = config.request_burst != 0 ? config.request_burst : NYA_HTTP_DEFAULT_REQUEST_BURST;
    state->max_connections_per_address = nya_min(state->max_connections_per_address, state->max_connections);

    // clamped rather than refused: a program asking for more threads than this has guessed about the machine, and a server that won't start is worse than one with eight workers.
    state->workers = nya_min(config.workers, (u32)NYA_HTTP_MAX_WORKERS);

    // One exchange per connection when threaded, so the connection table bounds the work in flight and a slot never has to be found; one for the whole server otherwise, the single request/response buffer this has always had.
    state->slot_count = state->workers > 0 ? state->max_connections : 1;
    state->slots      = nya_arena_alloc(arena, sizeof(_NYA_HttpSlot) * state->slot_count);

    if (state->slots == nullptr) {
        nya_arena_destroy(arena);
        nya_os_socket_close(listener);
        nya_os_socket_stop();

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the HTTP server's exchanges");
    }

    nya_memset(state->slots, 0, sizeof(_NYA_HttpSlot) * state->slot_count);

    // The certificate is read here rather than on the first connection: a wrong path, a key that doesn't match its certificate, and an unreadable file are all things to find out while a person watches the server start, not on someone's first request.
    if (wants_certificate) {
        NYA_Error ready = nya_tls_context_create(arena, &state->tls, .certificate_path = config.certificate_path, .key_path = config.key_path);

        if (!ready.ok) {
            for (u32 made = 0; made < state->slot_count; made++) nya_arena_destroy(state->slots[made].arena);

            nya_arena_destroy(arena);
            nya_os_socket_close(listener);
            nya_os_socket_stop();

            return ready;
        }
    }

    for (u32 index = 0; index < state->slot_count; index++) {
        state->slots[index].arena = nya_arena_create(.name = "http_exchange");

        if (state->slots[index].arena != nullptr) continue;

        for (u32 made = 0; made < index; made++) nya_arena_destroy(state->slots[made].arena);

        nya_arena_destroy(arena);
        nya_os_socket_close(listener);
        nya_os_socket_stop();

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the HTTP server's scratch");
    }

    if (config.secret != nullptr && config.secret_size > 0) {
        memcpy(state->secret, config.secret, config.secret_size);
        state->secret_size = config.secret_size;
    }

    for (u32 index = 0; index < config.layer_count; index++) {
        if (config.layers[index] == nullptr) continue;

        state->layers[state->layer_count++] = config.layers[index];
    }

    _NYA_HTTP = state;

    // The thread that starts the server is the one its NYA_HTTP_AFFINITY_MAIN handlers run on and every main-thread-only module is checked against; claimed only when this thread already counts as main, so starting from a worker can't move the claim.
    if (nya_thread_main_is_current()) nya_thread_main_claim();

    NYA_Error started = _nya_http_threads_start(state);

    if (!started.ok) {
        for (u32 index = 0; index < state->slot_count; index++) nya_arena_destroy(state->slots[index].arena);

        _NYA_HTTP = nullptr;

        nya_arena_destroy(arena);
        nya_os_socket_close(listener);
        nya_os_socket_stop();

        return started;
    }

    // an atomic counter and a plain one have the same bytes; the registry only ever reads it, and it's atomic because the listener thread is what moves it.
    nya_ceiling_register("http_connections", _NYA_HTTP->max_connections, (const u32*)&_NYA_HTTP->connection_count);
    nya_ceiling_register("http_rate_buckets", NYA_HTTP_MAX_RATE_BUCKETS, &_NYA_HTTP->bucket_count);
    nya_ceiling_register("http_websockets", NYA_HTTP_MAX_WEBSOCKETS, &_NYA_HTTP_WEBSOCKET_COUNT);

    // Every response says so, and only over TLS; see nya_http_hsts_set.
    nya_http_hsts_set(state->tls != nullptr);

    NYA_ConstCString scheme = state->tls != nullptr ? "https" : "http";

    if (state->tls != nullptr) nya_log_info("TLS is on: %s", nya_tls_version());

    if (state->workers > 0) {
        nya_log_info(
            "HTTP server listening on %s://%s:%u, on its own thread with %u worker%s",
            scheme,
            requested,
            (u32)config.port,
            state->workers,
            state->workers == 1 ? "" : "s"
        );
    } else {
        nya_log_info("HTTP server listening on %s://%s:%u", scheme, requested, (u32)config.port);
    }

    return NYA_OK;
}

void nya_system_http_deinit(void) {
    if (_NYA_HTTP == nullptr) return;

    _NYA_HttpState* state = _NYA_HTTP;

    // Retract the ceilings init registered: two point into `state`, which this frees, so a metrics query after deinit would read freed memory. Done up front so both the graceful and leak-on-purpose paths drop them, and so a re-init doesn't register a second copy.
    nya_ceiling_unregister("http_connections");
    nya_ceiling_unregister("http_rate_buckets");
    nya_ceiling_unregister("http_websockets");

    // first, so a drain that arrives while the rest of this is running finds nothing to do.
    atomic_store_explicit(&state->stopping, true, memory_order_release);

    // The listener first and on its own: once it has returned nothing reads, writes or accepts a socket, so everything below happens to a table nobody else is looking at.
    if (state->listener_thread != nullptr) nya_thread_join(state->listener_thread);

    // every worker woken at once: one between requests returns immediately, one inside a handler returns when the handler does, and _nya_http_workers_join is where that stops being waited for.
    for (u32 index = 0; index < state->workers; index++) nya_semaphore_post(state->work);

    u32 stuck = _nya_http_workers_join(state);

    for (u32 index = 0; index < NYA_HTTP_MAX_CONNECTIONS; index++) _nya_http_close(&state->connections[index]);

    // after the connections, so every close was reported while its socket was still open.
    _nya_http_websocket_shutdown();

    nya_os_socket_close(state->listener);
    state->listener = NYA_OS_SOCKET_NONE;

    // A handler that ignored the deadline is still writing into its slot and reading the secret out of this state, so the port goes back and the sockets close but the memory doesn't: freeing it would hand a running thread a use-after-free, and wiping the secret a write into a buffer another thread reads. The process is on its way out; a leak until then is the only sound answer, and it's loud.
    if (stuck > 0) {
        nya_log_error(
            "%u HTTP handler(s) were still running %d ms after shutdown started; the port is closed and their memory is leaked on purpose.",
            stuck,
            (s32)NYA_HTTP_SHUTDOWN_GRACE_MS
        );

        _NYA_HTTP = nullptr;

        nya_os_socket_stop();

        return;
    }

    nya_http_hsts_set(false);

    // after every connection closed, since each of those released a session out of this context.
    if (state->tls != nullptr) {
        nya_tls_context_destroy(state->tls);
        state->tls = nullptr;
    }

    // the secret leaves no copy behind in a freed region waiting to be handed out again.
    memset(state->secret, 0, sizeof(state->secret));

    for (u32 index = 0; index < state->slot_count; index++) nya_arena_destroy(state->slots[index].arena);

    nya_semaphore_destroy(state->work);
    nya_mutex_destroy(state->queue_mutex);
    nya_mutex_destroy(state->table_mutex);

    NYA_Arena* arena = state->allocator;
    _NYA_HTTP        = nullptr;

    nya_arena_destroy(arena);

    nya_os_socket_stop();
}

void nya_system_http_tick(void) {
    if (_NYA_HTTP == nullptr) return;

    // With workers the listener thread does the sockets, and what's left for the tick is the work that must happen where the program's own state is: the exchanges whose route asked for it, and every WebSocket. Without them this is the whole drain, as it has always been.
    if (_NYA_HTTP->workers > 0) {
        _nya_http_main_drain();
        _nya_http_websockets_drain();

        return;
    }

    _nya_http_pass();
}

// ROUTERS

NYA_Error nya_http_server_merge(const NYA_HttpRouter* router) {
    if (_NYA_HTTP == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "the HTTP server is not running");

    NYA_TRY(nya_http_router_check(router));

    nya_mutex_lock(_NYA_HTTP->table_mutex);
    defer nya_mutex_unlock(_NYA_HTTP->table_mutex);

    for (u32 index = 0; index < _NYA_HTTP->router_count; index++) {
        if (_NYA_HTTP->routers[index] == router) return nya_error(NYA_ERROR_ALREADY_EXISTS, "'%s' is already mounted", router->name);
    }

    if (_NYA_HTTP->router_count >= NYA_HTTP_MAX_ROUTERS) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "at most %d routers can be mounted", NYA_HTTP_MAX_ROUTERS);
    }

    _NYA_HTTP->routers[_NYA_HTTP->router_count++] = router;

    return NYA_OK;
}

void nya_http_server_unmerge(const NYA_HttpRouter* router) {
    if (_NYA_HTTP == nullptr || router == nullptr) return;

    nya_mutex_lock(_NYA_HTTP->table_mutex);
    defer nya_mutex_unlock(_NYA_HTTP->table_mutex);

    for (u32 index = 0; index < _NYA_HTTP->router_count; index++) {
        if (_NYA_HTTP->routers[index] != router) continue;

        for (u32 shift = index; shift + 1 < _NYA_HTTP->router_count; shift++) _NYA_HTTP->routers[shift] = _NYA_HTTP->routers[shift + 1];

        _NYA_HTTP->router_count--;
        _NYA_HTTP->routers[_NYA_HTTP->router_count] = nullptr;

        return;
    }
}

// INTROSPECTION

b8 nya_http_server_is_running(void) {
    return _NYA_HTTP != nullptr;
}

u16 nya_http_server_port(void) {
    return _NYA_HTTP != nullptr ? _NYA_HTTP->port : 0;
}

u32 nya_http_server_connection_count(void) {
    return _NYA_HTTP != nullptr ? atomic_load_explicit(&_NYA_HTTP->connection_count, memory_order_relaxed) : 0;
}

u64 nya_http_server_request_count(void) {
    return _NYA_HTTP != nullptr ? atomic_load_explicit(&_NYA_HTTP->request_count, memory_order_relaxed) : 0;
}

u32 nya_http_server_router_count(void) {
    if (_NYA_HTTP == nullptr) return 0;

    nya_mutex_lock(_NYA_HTTP->table_mutex);
    defer nya_mutex_unlock(_NYA_HTTP->table_mutex);

    return _NYA_HTTP->router_count;
}

const NYA_HttpRouter* nya_http_server_router_at(u32 index) {
    if (_NYA_HTTP == nullptr) return nullptr;

    nya_mutex_lock(_NYA_HTTP->table_mutex);
    defer nya_mutex_unlock(_NYA_HTTP->table_mutex);

    if (index >= _NYA_HTTP->router_count) return nullptr;

    return _NYA_HTTP->routers[index];
}

// SECRETS

NYA_Error nya_http_secret_from_environment(NYA_ConstCString variable, u8* buffer, u64 capacity, u64* out_size) {
    nya_assert(variable != nullptr);
    nya_assert(buffer != nullptr);
    nya_assert(out_size != nullptr);

    *out_size = 0;
    memset(buffer, 0, capacity);

    const char* value = getenv(variable);

    if (value == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "%s is not set", variable);

    u64 size = strlen(value);

    if (size < NYA_HTTP_MIN_SECRET_BYTES)
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s is shorter than %d bytes", variable, NYA_HTTP_MIN_SECRET_BYTES);

    if (size > capacity) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%s is longer than %llu bytes", variable, (unsigned long long)capacity);

    nya_memcpy(buffer, value, size);
    *out_size = size;

    return NYA_OK;
}

// PRIVATE API IMPLEMENTATION

void _nya_http_pass(void) {
    const b8 threaded = _NYA_HTTP->workers > 0;

    // the whole pass under one lock, and the only other thread that wants it is the tick draining the WebSockets; a pass is non-blocking socket calls over at most eight connections, so it makes the tick wait microseconds, and a handler never runs under it.
    nya_mutex_lock(_NYA_HTTP->table_mutex);
    defer nya_mutex_unlock(_NYA_HTTP->table_mutex);

    _nya_http_accept();

    u32 budget = NYA_HTTP_MAX_REQUESTS_PER_TICK;

    for (u32 index = 0; index < NYA_HTTP_MAX_CONNECTIONS; index++) {
        _NYA_HttpConnection* connection = &_NYA_HTTP->connections[index];

        if (connection->socket.handle == 0) continue;

        // a connection that upgraded is drained by the websocket table under its own bounds; it keeps the slot it was accepted into, so it never escapes the ones above. With workers that drain is the tick's, so the listener lets go of the socket here and never looks at it again.
        if (connection->upgraded) {
            if (threaded) continue;

            if (!_nya_http_websocket_tick(connection->socket)) _nya_http_close(connection);
            continue;
        }

        _NYA_HttpSlot* slot = &_NYA_HTTP->slots[threaded ? index : 0];

        if (threaded) {
            u32 slot_state = atomic_load_explicit(&slot->state, memory_order_acquire);

            // Queued or running: the exchange belongs to somebody else, and so does the connection — not read from, not timed out and not closed until the answer is written, which makes a slow handler cost its own connection and nothing else.
            if (slot_state == _NYA_HTTP_SLOT_QUEUED || slot_state == _NYA_HTTP_SLOT_RUNNING) continue;

            if (slot_state == _NYA_HTTP_SLOT_DONE) {
                if (!_nya_http_complete(connection, slot)) {
                    _nya_http_close(connection);
                    continue;
                }

                // the answer was a 101, so it is the tick's from here.
                if (connection->upgraded) continue;
            }
        }

        if (!_nya_http_receive(connection, slot)) {
            _nya_http_close(connection);
            continue;
        }

        if (!_nya_http_handle(connection, slot, &budget)) {
            _nya_http_close(connection);
            continue;
        }

        // an exchange that was just handed over owns the connection until its answer is written.
        if (threaded && atomic_load_explicit(&slot->state, memory_order_acquire) != _NYA_HTTP_SLOT_IDLE) continue;

        // Two reasons to drop a still-open connection: it has gone quiet mid-request, and it has stopped reading what we already sent. Both are bounds rather than checks, because neither has a version safe to wait out.
        // Read after the work rather than once at the top of the tick: receiving stamps the connection with a fresh reading, so a timestamp taken before it is behind the one being subtracted from it, and the difference of two unsigned times in that order is an enormous number.
        u64 now_ns = nya_clock_get_monotonic_ns();

        if (now_ns > connection->active_at_ns && now_ns - connection->active_at_ns > (u64)NYA_HTTP_IDLE_TIMEOUT_MS * 1000000ULL) {
            _nya_http_close(connection);
            continue;
        }

        // whatever the peer will take of what it is owed, and the rest stays queued for the next pass.
        if (!_nya_http_flush(connection)) {
            _nya_http_close(connection);
            continue;
        }

        if (connection->closing && connection->sending_size == connection->sent) _nya_http_close(connection);
    }
}

void _nya_http_accept(void) {
    for (u32 accepted = 0; accepted < NYA_HTTP_MAX_ACCEPTS_PER_TICK; accepted++) {
        NYA_OsSocket  socket = NYA_OS_SOCKET_NONE;
        NYA_OsAddress peer   = { 0 };

        NYA_OsSocketStatus taken = nya_os_socket_accept(_NYA_HTTP->listener, &socket, &peer);

        // nobody waiting is the ordinary end of this loop rather than a failure.
        if (taken == NYA_OS_SOCKET_WOULD_BLOCK) return;

        if (taken != NYA_OS_SOCKET_OK) {
            nya_log_warn("The HTTP listener failed to accept.");
            return;
        }

        _NYA_HttpConnection* slot = nullptr;

        for (u32 index = 0; index < _NYA_HTTP->max_connections; index++) {
            if (_NYA_HTTP->connections[index].socket.handle != 0) continue;

            slot = &_NYA_HTTP->connections[index];
            break;
        }

        // the connection past the last is closed now rather than queued, so a process that loops on connect can't grow anything here.
        if (slot == nullptr) {
            nya_os_socket_close(socket);
            continue;
        }

        // the peer as the socket reports it. A header could claim anything, so the limits never read one.
        char address[NYA_HTTP_MAX_ADDRESS] = { 0 };

        // without the port: the limits are about a machine, and its next connection comes from another port.
        if (!nya_os_address_text(peer, false, address, sizeof(address))) (void)snprintf(address, sizeof(address), "%s", "unknown");

        // and one address cannot take every slot. Closed rather than queued, like the connection past the last.
        u32 held = 0;
        for (u32 index = 0; index < _NYA_HTTP->max_connections; index++) {
            if (_NYA_HTTP->connections[index].socket.handle != 0 && strcmp(_NYA_HTTP->connections[index].address, address) == 0) held++;
        }
        if (held >= _NYA_HTTP->max_connections_per_address) {
            nya_os_socket_close(socket);
            continue;
        }

        // a small answer goes now rather than waiting for company, which is what a request/response protocol wants: there's nothing else coming to share the packet with.
        (void)nya_os_socket_set_no_delay(socket, true);

        // Cleared rather than assigned a compound literal: a connection holds its read and write buffers inline, so building one on the stack first is a quarter of a megabyte of frame.
        nya_memset(slot, 0, sizeof(*slot));

        slot->socket       = socket;
        slot->active_at_ns = nya_clock_get_monotonic_ns();
        (void)snprintf(slot->address, sizeof(slot->address), "%s", address);

        // Nothing is read or written here: the handshake happens on the drain passes that follow, so one peer opening a connection slowly can't hold up the accept loop.
        if (_NYA_HTTP->tls != nullptr) {
            NYA_Error started = nya_tls_session_create(_NYA_HTTP->tls, socket, &slot->tls);

            if (!started.ok) {
                nya_log_warn("A TLS session could not be started: %s", (NYA_ConstCString)started.message);

                nya_os_socket_close(socket);
                nya_memset(slot, 0, sizeof(*slot));

                continue;
            }
        }

        (void)atomic_fetch_add_explicit(&_NYA_HTTP->connection_count, 1, memory_order_relaxed);
    }
}

b8 _nya_http_tls_ready(_NYA_HttpConnection* connection) {
    if (connection->tls == nullptr) return true;
    if (nya_tls_is_established(connection->tls)) return true;

    connection->tls_wants_write = false;

    switch (nya_tls_handshake(connection->tls)) {
        case NYA_TLS_OK:
            connection->active_at_ns = nya_clock_get_monotonic_ns();
            return true;

        // Still going, and the idle timeout bounds how long that may take: a peer that opens a connection and never finishes a handshake is a peer that has gone quiet.
        case NYA_TLS_WANT_READ: return true;

        case NYA_TLS_WANT_WRITE:
            connection->tls_wants_write = true;
            return true;

        case NYA_TLS_CLOSED: return false;

        case NYA_TLS_FAILED:
        default:
            // Logged at debug rather than warn: a failed handshake is what a port scanner, a browser that dislikes this certificate, and someone speaking plain HTTP to an https port all look like, none a fault of this server's worth a log line.
            nya_log_debug("A TLS handshake from %s failed: %s", connection->address, nya_tls_error(connection->tls));
            return false;
    }
}

b8 _nya_http_push(_NYA_HttpConnection* connection, const u8* data, u64 size) {
    if (connection->socket.handle == 0) return false;
    if (size == 0) return true;

    // what is already owed, moved to the front, so the room below is the room that is actually left.
    if (!_nya_http_flush(connection)) return false;

    if (connection->sending_size + size > sizeof(connection->sending)) return false;

    nya_memcpy(connection->sending + connection->sending_size, data, size);
    connection->sending_size += size;

    return _nya_http_flush(connection);
}

b8 _nya_http_flush(_NYA_HttpConnection* connection) {
    if (connection->socket.handle == 0) return false;

    // Nothing may be written before the handshake is done, and the handshake is what a fresh TLS connection's first pass through here is actually doing.
    if (connection->tls != nullptr && !_nya_http_tls_ready(connection)) return false;

    while (connection->sent < connection->sending_size) {
        u64 wrote = 0;

        if (connection->tls != nullptr) {
            NYA_TlsProgress progress =
                nya_tls_send(connection->tls, connection->sending + connection->sent, connection->sending_size - connection->sent, &wrote);

            // A TLS write may want to read, because a record can't be written until whatever the peer is mid-send has been taken; both are "come back later" and neither is a failure — the tick returns here when the socket is ready.
            if (progress == NYA_TLS_WANT_READ) break;

            if (progress == NYA_TLS_WANT_WRITE) {
                connection->tls_wants_write = true;
                break;
            }

            if (progress != NYA_TLS_OK) return false;
        } else {
            NYA_OsSocketStatus status =
                nya_os_socket_send(connection->socket, connection->sending + connection->sent, connection->sending_size - connection->sent, &wrote);

            // The host's buffer is full, a peer reading slowly rather than gone: what's left stays queued and the bound above decides when that stops being fine.
            if (status == NYA_OS_SOCKET_WOULD_BLOCK) break;
            if (status != NYA_OS_SOCKET_OK) return false;
        }

        connection->sent         += wrote;
        connection->active_at_ns  = nya_clock_get_monotonic_ns();
    }

    if (connection->sent == 0) return true;

    // Compacted only once the socket has stopped taking bytes, so a whole answer that goes at once costs no copy: the common case leaves the queue empty rather than moving anything.
    u64 left = connection->sending_size - connection->sent;

    if (left > 0) nya_memmove(connection->sending, connection->sending + connection->sent, left);

    connection->sending_size = left;
    connection->sent         = 0;

    return true;
}

b8 _nya_http_receive(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot) {
    u64 room = NYA_HTTP_MAX_REQUEST_BYTES - connection->received_size;

    // A full buffer with no complete request in it: the peer has sent more than the worst legal request and there's nothing left to wait for.
    if (room == 0) {
        _nya_http_refuse(connection, slot, NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE, "the request could not be parsed", 0);
        return false;
    }

    u64 read = 0;

    if (connection->tls != nullptr) {
        if (!_nya_http_tls_ready(connection)) return false;

        // Still handshaking: there is nothing to read yet and nothing has gone wrong.
        if (!nya_tls_is_established(connection->tls)) return true;

        NYA_TlsProgress progress = nya_tls_receive(connection->tls, connection->received + connection->received_size, room, &read);

        if (progress == NYA_TLS_WANT_READ) return true;

        if (progress == NYA_TLS_WANT_WRITE) {
            connection->tls_wants_write = true;
            return true;
        }

        if (progress != NYA_TLS_OK) return false;
    } else {
        NYA_OsSocketStatus status = nya_os_socket_receive(connection->socket, connection->received + connection->received_size, room, &read);

        // Nothing waiting is the ordinary answer; the end of the stream and a broken connection are both the connection going, which is what false means to the caller.
        if (status == NYA_OS_SOCKET_WOULD_BLOCK) return true;
        if (status != NYA_OS_SOCKET_OK) return false;
    }

    connection->received_size += read;
    connection->active_at_ns   = nya_clock_get_monotonic_ns();

    return true;
}

b8 _nya_http_handle(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot, u32* budget) {
    const b8  threaded = _NYA_HTTP->workers > 0;
    const u32 index    = (u32)(connection - _NYA_HTTP->connections);

    while (*budget > 0 && connection->received_size > 0 && !connection->closing) {
        u64            consumed = 0;
        NYA_HttpStatus refusal  = NYA_HTTP_STATUS_NONE;

        NYA_HttpParse parsed = nya_http_request_parse(connection->received, connection->received_size, &slot->request, &consumed, &refusal);

        if (parsed == NYA_HTTP_PARSE_INCOMPLETE) return true;

        if (parsed == NYA_HTTP_PARSE_REFUSED) {
            _nya_http_refuse(connection, slot, refusal, "the request could not be parsed", 0);
            return false;
        }

        // counted once a request has parsed, so a slow sender is the idle timeout's to deal with and not this.
        u32 retry_after_s = 0;
        if (!_nya_http_rate_take(connection->address, &retry_after_s)) {
            _nya_http_refuse(connection, slot, NYA_HTTP_STATUS_TOO_MANY_REQUESTS, "this address has sent more requests than it may; see Retry-After", retry_after_s);
            return false;
        }

        nya_assert(consumed > 0 && consumed <= connection->received_size, "the parser reported consuming more than it was given");

        // shifted before the answer, so a handler can't see a half-consumed stream and a pipelined second request is already at the front when the first one's answer is queued.
        connection->received_size -= consumed;

        if (connection->received_size > 0) memmove(connection->received, connection->received + consumed, connection->received_size);

        (*budget)--;
        (void)atomic_fetch_add_explicit(&_NYA_HTTP->request_count, 1, memory_order_relaxed);

        slot->keep_alive  = slot->request.keep_alive;
        slot->head_only   = slot->request.method == NYA_HTTP_METHOD_HEAD;
        slot->trailing    = connection->received_size;
        slot->upgrade     = _nya_http_websocket_is_upgrade(&slot->request);
        slot->close_after = false;
        slot->answer      = _NYA_HTTP_ANSWER_WRITE;

        (void)snprintf(slot->address, sizeof(slot->address), "%s", connection->address);

        // the table as it is right now, so a merge from the frame decides the next request and never this one. Read under the lock this pass already holds.
        slot->router_count = _NYA_HTTP->router_count;
        for (u32 router = 0; router < _NYA_HTTP->router_count; router++) slot->routers[router] = _NYA_HTTP->routers[router];

        if (threaded) {
            // handed over, and the connection is not read from again until the answer comes back.
            _nya_http_queue(_NYA_HTTP, index, slot);
            return true;
        }

        // The upgrade is answered here rather than through the router: what follows a 101 is frames not a response, so it can't go through nya_http_response_head. Everything before still happened to it (accepted under the connection bounds, parsed by the same parser, its token spent), and the rest of the handshake's rules are the websocket's.
        if (slot->upgrade) {
            _nya_http_upgrade(connection, slot);

            if (slot->answer == _NYA_HTTP_ANSWER_UPGRADED) return true;

            return _nya_http_complete(connection, slot);
        }

        if (!_nya_http_answer(connection, slot)) return false;

        if (connection->closing) return true;
    }

    return true;
}

void _nya_http_dispatch(_NYA_HttpState* state, _NYA_HttpSlot* slot) {
    // everything the exchange allocates dies with it. Cleared at the start rather than the end so a response body that points into it is still valid while it's being written.
    nya_arena_free_all(slot->arena);

    nya_http_response_create(&slot->response, slot->response_body, sizeof(slot->response_body));

    _nya_http_request_id(state, slot->response.request_id);

    NYA_HttpExchange exchange = {
        .request     = &slot->request,
        .response    = &slot->response,
        .arena       = slot->arena,
        .secret      = state->secret_size > 0 ? state->secret : nullptr,
        .secret_size = state->secret_size,
        .now_s       = nya_clock_get_timestamp_s(),
        .started_ns  = nya_clock_get_monotonic_ns(),
        .address     = slot->address,
    };

    // every line the request causes carries its id, so a report quoting X-Request-Id finds all of them; the tag is this thread's, so two workers never land on each other's lines.
    char tag[NYA_LOG_TAG_MAX_LENGTH] = { 0 };
    (void)snprintf(tag, sizeof(tag), "req=%s", slot->response.request_id);
    nya_log_tag_set(tag);
    defer nya_log_tag_clear();

    slot->answer = _NYA_HTTP_ANSWER_WRITE;
    slot->status = nya_http_router_dispatch(&exchange, slot->routers, slot->router_count, state->layers, state->layer_count);

    // The one negotiated content coding, decided by http_message.c which owns the response bytes: it's handed the client's Accept-Encoding and compresses the body in place when worth it, setting Content-Encoding and Vary and fixing Content-Length, with the exchange arena as scratch. A HEAD keeps the coding so its headers match the GET's; only the body is held back, downstream.
    (void)nya_http_response_compress(&slot->response, slot->arena, nya_http_request_header(&slot->request, "accept-encoding"));
}

b8 _nya_http_answer(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot) {
    _nya_http_dispatch(_NYA_HTTP, slot);

    return _nya_http_complete(connection, slot);
}

b8 _nya_http_complete(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot) {
    const b8 upgraded    = slot->answer == _NYA_HTTP_ANSWER_UPGRADED;
    const b8 close_after = slot->close_after;
    const b8 keep_alive  = slot->keep_alive;

    b8 written = true;

    if (!upgraded) {
        written = _nya_http_write(connection, &slot->response, slot->status, keep_alive, slot->head_only);

        nya_http_response_destroy(&slot->response);
    }

    // the answer is the other half of "active": without this a connection whose handler took longer than the idle timeout would be closed the moment its answer went out.
    connection->active_at_ns = nya_clock_get_monotonic_ns();

    slot->answer      = _NYA_HTTP_ANSWER_WRITE;
    slot->close_after = false;

    // published last: from here the listener may fill this slot again.
    atomic_store_explicit(&slot->state, _NYA_HTTP_SLOT_IDLE, memory_order_release);

    if (upgraded) return true;

    if (!written || close_after) return false;

    if (!keep_alive) connection->closing = true;

    return true;
}

void _nya_http_upgrade(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot) {
    NYA_ConstCString detail  = "";
    NYA_HttpStatus   refusal = _nya_http_websocket_upgrade(connection->socket, connection->address, &slot->request, slot->trailing, &detail);

    if (refusal != NYA_HTTP_STATUS_NONE) {
        _nya_http_problem(slot, refusal, detail, 0);

        slot->status      = refusal;
        slot->answer      = _NYA_HTTP_ANSWER_WRITE;
        slot->keep_alive  = false;
        slot->head_only   = false;
        slot->close_after = true;

        return;
    }

    // under the lock, because the listener reads this flag to know the socket is not its any more.
    nya_mutex_lock(_NYA_HTTP->table_mutex);
    connection->upgraded = true;
    nya_mutex_unlock(_NYA_HTTP->table_mutex);

    slot->answer = _NYA_HTTP_ANSWER_UPGRADED;
}

void _nya_http_problem(_NYA_HttpSlot* slot, NYA_HttpStatus status, NYA_ConstCString detail, u32 retry_after_s) {
    nya_assert(detail != nullptr);

    nya_arena_free_all(slot->arena);

    nya_http_response_create(&slot->response, slot->response_body, sizeof(slot->response_body));

    _nya_http_request_id(_NYA_HTTP, slot->response.request_id);

    NYA_Error written = nya_http_response_printf(
        &slot->response,
        NYA_HTTP_MEDIA_JSON,
        "{\"status\":%d,\"error\":\"%s\",\"detail\":\"%s\"}",
        (s32)status,
        nya_http_status_text(status),
        detail
    );

    if (!written.ok) nya_http_response_reset(&slot->response);

    // a client told to slow down is told when to come back.
    if (retry_after_s > 0) {
        char seconds[16] = { 0 };
        (void)snprintf(seconds, sizeof(seconds), "%u", retry_after_s);
        if (!nya_http_response_header(&slot->response, "Retry-After", seconds).ok) nya_http_response_reset(&slot->response);
    }
}

void _nya_http_refuse(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot, NYA_HttpStatus status, NYA_ConstCString detail, u32 retry_after_s) {
    _nya_http_problem(slot, status, detail, retry_after_s);

    // never keep-alive: a stream the parser gave up on can't be resynchronised, and guessing where the next request starts is the request-smuggling bug this refuses in the first place; a peer over its budget is closed too, since anything it pipelined behind the refused request is unread.
    (void)_nya_http_write(connection, &slot->response, status, false, false);

    nya_http_response_destroy(&slot->response);
}

b8 _nya_http_write(_NYA_HttpConnection* connection, const NYA_HttpResponse* response, NYA_HttpStatus status, b8 keep_alive, b8 head_only) {
    u8  head[NYA_HTTP_MAX_RESPONSE_HEAD_BYTES] = { 0 };
    u64 head_size                              = 0;

    NYA_Error rendered = nya_http_response_head(response, status, keep_alive, nya_instant_now(), head, sizeof(head), &head_size);

    if (!rendered.ok) {
        nya_log_error("A %d response head could not be rendered; dropping the connection.", (s32)status);
        return false;
    }

    if (!_nya_http_push(connection, head, head_size)) return false;

    // a HEAD carries the Content-Length its GET would have and none of the bytes, which makes it a HEAD rather than a GET nobody read.
    if (head_only || response->body_size == 0) return true;

    return _nya_http_push(connection, response->body, response->body_size);
}

void _nya_http_request_id(const _NYA_HttpState* state, OUT char* out) {
    u8 bits[8] = { 0 };

    // an id only has to be unique, not secret, so a CSPRNG that fails falls back to the request count rather than refusing the request.
    if (!nya_os_random_bytes(bits, sizeof(bits))) {
        u64 counted = atomic_load_explicit(&state->request_count, memory_order_relaxed);

        for (u32 index = 0; index < sizeof(bits); index++) bits[index] = (u8)(counted >> (index * 8));
    }

    for (u32 index = 0; index < sizeof(bits); index++) (void)snprintf(out + ((u64)index * 2), 3, "%02x", bits[index]);
}

b8 _nya_http_rate_take(NYA_ConstCString address, OUT u32* out_retry_after_s) {
    nya_assert(address != nullptr && out_retry_after_s != nullptr);

    u64 now_ns = nya_clock_get_monotonic_ns();

    _NYA_HttpRateBucket* bucket = nullptr;
    for (u32 index = 0; index < _NYA_HTTP->bucket_count; index++) {
        if (strcmp(_NYA_HTTP->buckets[index].address, address) == 0) bucket = &_NYA_HTTP->buckets[index];
    }

    // a new address starts full; past the table's bound one budget makes room, and which one is the whole of whether this table can be used against the addresses already in it (see below).
    if (bucket == nullptr) {
        if (_NYA_HTTP->bucket_count < NYA_HTTP_MAX_RATE_BUCKETS) {
            bucket = &_NYA_HTTP->buckets[_NYA_HTTP->bucket_count++];
        } else {
            // Only a budget refilled all the way makes room: an address whose budget is spent is one being refused right now, and evicting it would hand it a fresh burst — so someone spraying source addresses could clear the table and start over at will, the eviction attack this bound used to have. Among the full ones the stalest goes, since they're all equally not being refused.
            _NYA_HttpRateBucket* victim = nullptr;

            for (u32 index = 0; index < NYA_HTTP_MAX_RATE_BUCKETS; index++) {
                _NYA_HttpRateBucket* candidate = &_NYA_HTTP->buckets[index];

                f64 idle_s    = (f64)(now_ns - candidate->refilled_at_ns) / 1e9;
                f64 projected = candidate->tokens + (idle_s * (f64)_NYA_HTTP->requests_per_second);

                if (projected < (f64)_NYA_HTTP->request_burst) continue;
                if (victim == nullptr || candidate->refilled_at_ns < victim->refilled_at_ns) victim = candidate;
            }

            // Every budget in the table is spent: the table is full of addresses being refused, and this one waits with them rather than taking a slot from one.
            if (victim == nullptr) {
                *out_retry_after_s = 1;
                return false;
            }

            bucket = victim;
        }

        *bucket = (_NYA_HttpRateBucket){ .tokens = (f64)_NYA_HTTP->request_burst, .refilled_at_ns = now_ns };
        (void)snprintf(bucket->address, sizeof(bucket->address), "%s", address);
    }

    f64 elapsed_s          = (f64)(now_ns - bucket->refilled_at_ns) / 1e9;
    bucket->tokens         = nya_min(bucket->tokens + (elapsed_s * (f64)_NYA_HTTP->requests_per_second), (f64)_NYA_HTTP->request_burst);
    bucket->refilled_at_ns = now_ns;

    if (bucket->tokens >= 1.0) {
        bucket->tokens -= 1.0;
        return true;
    }

    // a whole token's refill, rounded up, since Retry-After is in whole seconds and early is refused again.
    *out_retry_after_s = (u32)ceil((1.0 - bucket->tokens) / (f64)_NYA_HTTP->requests_per_second);
    if (*out_retry_after_s == 0) *out_retry_after_s = 1;

    return false;
}

void _nya_http_close(_NYA_HttpConnection* connection) {
    if (connection->socket.handle == 0) return;

    // The session before the socket, and no close_notify with it: a connection dropped here is one that timed out, misbehaved or is finished, none worth a round trip with a peer that may not answer. A peer that cares whether it got everything has Content-Length.
    if (connection->tls != nullptr) {
        nya_tls_session_destroy(connection->tls);
        connection->tls = nullptr;
    }

    // the report that the socket is gone, while the socket is still the thing that is going.
    if (connection->upgraded) _nya_http_websocket_detach(connection->socket);

    nya_os_socket_close(connection->socket);

    // by memset, for the reason in _nya_http_accept.
    nya_memset(connection, 0, sizeof(*connection));

    nya_assert(atomic_load_explicit(&_NYA_HTTP->connection_count, memory_order_relaxed) > 0, "a connection was closed that was never counted");
    (void)atomic_fetch_sub_explicit(&_NYA_HTTP->connection_count, 1, memory_order_relaxed);
}

// THE LISTENER AND THE POOL

void _nya_http_queue(_NYA_HttpState* state, u32 index, _NYA_HttpSlot* slot) {
    const b8 on_main = _nya_http_runs_on_main(slot);

    // published before the index reaches a queue, so whoever pops it sees a filled slot.
    atomic_store_explicit(&slot->state, _NYA_HTTP_SLOT_QUEUED, memory_order_release);

    nya_mutex_lock(state->queue_mutex);
    _nya_http_queue_push(on_main ? state->main_queue : state->worker_queue, on_main ? &state->main_queued : &state->worker_queued, index);
    nya_mutex_unlock(state->queue_mutex);

    if (!on_main) nya_semaphore_post(state->work);
}

b8 _nya_http_runs_on_main(const _NYA_HttpSlot* slot) {
    // the handshake is not a route, and what it registers into is the WebSocket table, which belongs to whoever ticks; see http_server.h.
    if (slot->upgrade) return true;

    b8 path_exists = false;

    const NYA_HttpRoute* route =
        nya_http_router_find(slot->routers, slot->router_count, slot->request.method, slot->request.path, &path_exists);

    // an unmatched path answers 404 out of the router, which touches nothing of the program's.
    return route != nullptr && route->affinity == NYA_HTTP_AFFINITY_MAIN;
}

void _nya_http_queue_push(u32* queue, u32* count, u32 index) {
    nya_assert(*count < NYA_HTTP_MAX_CONNECTIONS, "more exchanges are queued than there are connections to have produced them");

    queue[(*count)++] = index;
}

b8 _nya_http_queue_pop(u32* queue, u32* count, OUT u32* out_index) {
    if (*count == 0) return false;

    *out_index = queue[0];

    // first in, first out, over a table of eight: a shift is four moves and a ring would be state to get wrong for no measurable gain.
    (*count)--;
    for (u32 index = 0; index < *count; index++) queue[index] = queue[index + 1];

    return true;
}

void _nya_http_main_drain(void) {
    _NYA_HttpState* state = _NYA_HTTP;

    // the same budget the unthreaded drain answers under, for the same reason: these run on the frame.
    for (u32 answered = 0; answered < NYA_HTTP_MAX_REQUESTS_PER_TICK; answered++) {
        u32 index = 0;

        nya_mutex_lock(state->queue_mutex);
        b8 taken = _nya_http_queue_pop(state->main_queue, &state->main_queued, &index);
        nya_mutex_unlock(state->queue_mutex);

        if (!taken) return;

        _NYA_HttpSlot* slot = &state->slots[index];

        atomic_store_explicit(&slot->state, _NYA_HTTP_SLOT_RUNNING, memory_order_release);

        if (slot->upgrade) {
            _nya_http_upgrade(&state->connections[index], slot);
        } else {
            _nya_http_dispatch(state, slot);
        }

        // and back to the listener, which writes it.
        atomic_store_explicit(&slot->state, _NYA_HTTP_SLOT_DONE, memory_order_release);
    }
}

void _nya_http_websockets_drain(void) {
    nya_mutex_lock(_NYA_HTTP->table_mutex);
    defer nya_mutex_unlock(_NYA_HTTP->table_mutex);

    for (u32 index = 0; index < NYA_HTTP_MAX_CONNECTIONS; index++) {
        _NYA_HttpConnection* connection = &_NYA_HTTP->connections[index];

        if (connection->socket.handle == 0 || !connection->upgraded) continue;

        if (!_nya_http_websocket_tick(connection->socket)) _nya_http_close(connection);
    }
}

void _nya_http_listener_wait(_NYA_HttpState* state) {
    NYA_OsSocketWait watched[NYA_HTTP_MAX_CONNECTIONS + 1] = { 0 };
    u32              watched_count                         = 0;
    b8               busy                                  = false;

    nya_mutex_lock(state->table_mutex);
    {
        watched[watched_count++] = (NYA_OsSocketWait){ .socket = state->listener, .readable = true };

        for (u32 index = 0; index < NYA_HTTP_MAX_CONNECTIONS; index++) {
            _NYA_HttpConnection* connection = &state->connections[index];

            if (connection->socket.handle == 0) continue;

            // An upgraded socket is the tick's and may be destroyed by it at any moment, so it never goes into a set this thread is about to sleep on; while one is open the listener polls instead, which costs a wakeup a millisecond and is the price of the socket being somebody else's.
            if (connection->upgraded) {
                busy = true;
                continue;
            }

            // an answer being worked on can't wake a socket, and bytes already buffered won't arrive again; both are a reason to come straight back rather than sleep.
            if (atomic_load_explicit(&state->slots[index].state, memory_order_acquire) != _NYA_HTTP_SLOT_IDLE) busy = true;
            if (connection->received_size > 0) busy = true;

            // a connection with an answer the peer hasn't taken wakes on room to write as well, so a slow reader is served as fast as it'll read rather than at the idle timeout.
            b8 owes = connection->sending_size > connection->sent;

            // and so does one whose TLS handshake is waiting on room to write, the one case where a connection owing nothing still has something to say.
            watched[watched_count++] =
                (NYA_OsSocketWait){ .socket = connection->socket, .readable = true, .writable = owes || connection->tls_wants_write };
        }
    }
    nya_mutex_unlock(state->table_mutex);

    if (busy) {
        nya_os_time_sleep_ms(_NYA_HTTP_LISTENER_BUSY_MS);
        return;
    }

    u32 ready = 0;
    (void)nya_os_socket_wait(watched, watched_count, _NYA_HTTP_LISTENER_IDLE_MS, &ready);
}

void _nya_http_listener_thread(void* data) {
    _NYA_HttpState* state = (_NYA_HttpState*)data;

    while (!atomic_load_explicit(&state->stopping, memory_order_acquire)) {
        _nya_http_pass();
        _nya_http_listener_wait(state);
    }

}

void _nya_http_worker_thread(void* data) {
    _NYA_HttpState* state = (_NYA_HttpState*)data;

    // The state is handed over rather than read from the global, because a handler that outruns the shutdown deadline is still here after _NYA_HTTP has been cleared and its memory deliberately leaked; see nya_system_http_deinit.
    while (!atomic_load_explicit(&state->stopping, memory_order_acquire)) {
        if (!nya_semaphore_wait_timeout(state->work, _NYA_HTTP_WORKER_WAIT_MS)) continue;

        u32 index = 0;

        nya_mutex_lock(state->queue_mutex);
        b8 taken = _nya_http_queue_pop(state->worker_queue, &state->worker_queued, &index);
        nya_mutex_unlock(state->queue_mutex);

        if (!taken) continue;

        _NYA_HttpSlot* slot = &state->slots[index];

        atomic_store_explicit(&slot->state, _NYA_HTTP_SLOT_RUNNING, memory_order_release);

        _nya_http_dispatch(state, slot);

        // and back to the listener, which writes it.
        atomic_store_explicit(&slot->state, _NYA_HTTP_SLOT_DONE, memory_order_release);
    }

}

NYA_Error _nya_http_threads_start(_NYA_HttpState* state) {
    if (state->workers == 0) return NYA_OK;

    NYA_Mutex*     table_mutex = nullptr;
    NYA_Mutex*     queue_mutex = nullptr;
    NYA_Semaphore* work        = nullptr;

    NYA_Error table_lock = nya_mutex_create(state->allocator, &table_mutex);
    NYA_Error queue_lock = nya_mutex_create(state->allocator, &queue_mutex);
    NYA_Error work_count = nya_semaphore_create(state->allocator, 0, &work);

    if (!table_lock.ok || !queue_lock.ok || !work_count.ok) {
        nya_semaphore_destroy(work);
        nya_mutex_destroy(queue_mutex);
        nya_mutex_destroy(table_mutex);

        // whichever of the three actually refused, since the caller wants the reason and not the order.
        if (!table_lock.ok) return table_lock;
        if (!queue_lock.ok) return queue_lock;

        return work_count;
    }

    // published together, so the state never holds a lock the failure above has just given back.
    state->table_mutex = table_mutex;
    state->queue_mutex = queue_mutex;
    state->work        = work;

    // the workers first: a listener with nobody to hand an exchange to would queue one before the pool exists, and starting the pool afterwards would be a race for no reason.
    for (u32 index = 0; index < state->workers; index++) {
        NYA_Error worker = nya_thread_spawn(state->allocator, _nya_http_worker_thread, state, "HTTP Worker", &state->worker_threads[index]);

        if (worker.ok) continue;

        atomic_store_explicit(&state->stopping, true, memory_order_release);

        for (u32 running = 0; running < index; running++) nya_semaphore_post(state->work);
        for (u32 running = 0; running < index; running++) nya_thread_join(state->worker_threads[running]);

        nya_semaphore_destroy(state->work);
        nya_mutex_destroy(state->queue_mutex);
        nya_mutex_destroy(state->table_mutex);

        nya_memset(state->worker_threads, 0, sizeof(state->worker_threads));
        state->work        = nullptr;
        state->queue_mutex = nullptr;
        state->table_mutex = nullptr;
        atomic_store_explicit(&state->stopping, false, memory_order_release);

        return worker;
    }

    NYA_Error listener = nya_thread_spawn(state->allocator, _nya_http_listener_thread, state, "HTTP Listener", &state->listener_thread);

    if (!listener.ok) {
        atomic_store_explicit(&state->stopping, true, memory_order_release);

        for (u32 running = 0; running < state->workers; running++) nya_semaphore_post(state->work);
        for (u32 running = 0; running < state->workers; running++) nya_thread_join(state->worker_threads[running]);

        nya_semaphore_destroy(state->work);
        nya_mutex_destroy(state->queue_mutex);
        nya_mutex_destroy(state->table_mutex);

        nya_memset(state->worker_threads, 0, sizeof(state->worker_threads));
        state->work        = nullptr;
        state->queue_mutex = nullptr;
        state->table_mutex = nullptr;
        atomic_store_explicit(&state->stopping, false, memory_order_release);

        return listener;
    }

    return NYA_OK;
}

u32 _nya_http_workers_join(_NYA_HttpState* state) {
    u32 stuck = 0;

    const u64 deadline_ns = nya_clock_get_monotonic_ns() + ((u64)NYA_HTTP_SHUTDOWN_GRACE_MS * 1000000ULL);

    for (u32 index = 0; index < state->workers; index++) {
        NYA_Thread* thread = state->worker_threads[index];

        if (thread == nullptr) continue;

        state->worker_threads[index] = nullptr;

        while (!nya_thread_is_finished(thread) && nya_clock_get_monotonic_ns() < deadline_ns) nya_os_time_sleep_ms(1);

        // Still inside a handler at the deadline: there's no way to stop a thread from out here that isn't worse than the problem (it may hold a lock, or be halfway through a write), so it's let go of and the deadline is kept by everybody else.
        if (!nya_thread_is_finished(thread)) {
            nya_thread_abandon(thread);
            stuck++;
            continue;
        }

        nya_thread_join(thread);
    }

    return stuck;
}
