#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL3/SDL_error.h"
// for SDL_CleanupTLS alone: nothing here is started by SDL any more. See _nya_http_thread_end.
#include "SDL3/SDL_thread.h"
#include "SDL3/SDL_timer.h"

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_ceiling.h"
#include "nyangine/base/base_compare.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_thread.h"
#include "nyangine/http/http_server.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/os/os_random.h"
#include "SDL3_net/SDL_net.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How long the listener thread sleeps between passes while anything is in flight.
 *
 * A worker finishing is not something SDL_net can be woken by, so while there is an answer to write
 * the listener polls instead of sleeping on the sockets. One millisecond is the same order as the job
 * scheduler's busy tick and is below what a client can measure.
 * */
#define _NYA_HTTP_LISTENER_BUSY_MS 1

/** And how long it waits on the sockets when there is nothing outstanding, so an idle server is free. */
#define _NYA_HTTP_LISTENER_IDLE_MS 20

/** How long a worker sits on the semaphore before looking at the stop flag again. */
#define _NYA_HTTP_WORKER_WAIT_MS 20

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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
    NET_StreamSocket* socket;

    /** What has arrived and not yet been consumed. Bounded by its own size and nothing else. */
    u8  received[NYA_HTTP_MAX_REQUEST_BYTES];
    u64 received_size;

    /** Monotonic nanoseconds of the last byte read or answer written; drives the idle timeout. */
    u64 active_at_ns;

    /** Answered with `Connection: close`, so it is dropped as soon as the answer is queued. */
    b8 closing;

    /** Answered with a 101, so the bytes on it are frames and http_websocket_server.c owns them. */
    b8 upgraded;

    /** The peer, as SDL_net spells it. What the per address limits key on, and never a forwarded header. */
    char address[NYA_HTTP_MAX_ADDRESS];
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

    NET_Server* listener;

    u16 port;
    u32 max_connections;
    u32 max_connections_per_address;
    u32 requests_per_second;
    u32 request_burst;

    _NYA_HttpRateBucket buckets[NYA_HTTP_MAX_RATE_BUCKETS];
    u32                 bucket_count;

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

    /*
     * ── the threaded half, all of it null and zero while `workers` is zero ──
     */

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Null when the server is off, which is what makes every call here a no-op and costs nothing. */
NYA_INTERNAL _NYA_HttpState* _NYA_HTTP = nullptr;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One drain pass: accept, then read, answer and time out every connection once. */
NYA_INTERNAL void _nya_http_pass(void);

/** Takes whatever the listener has, up to NYA_HTTP_MAX_ACCEPTS_PER_TICK. */
NYA_INTERNAL void _nya_http_accept(void);

/** Reads what has arrived on one connection. False when the connection is finished with. */
NYA_INTERNAL b8 _nya_http_receive(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot) __attr_no_discard;

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

/** What every thread here runs on its way out, to give SDL back what it keeps per thread. */
NYA_INTERNAL void _nya_http_thread_end(void);

/** Starts the listener and the pool. Leaves nothing running when it fails. */
NYA_INTERNAL NYA_Error _nya_http_threads_start(_NYA_HttpState* state) __attr_no_discard;

/** Joins every worker inside NYA_HTTP_SHUTDOWN_GRACE_MS and returns how many were still inside a handler. */
NYA_INTERNAL u32 _nya_http_workers_join(_NYA_HttpState* state) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_system_http_init(NYA_HttpConfig config) {
    if (_NYA_HTTP != nullptr) return nya_error(NYA_ERROR_ALREADY_EXISTS, "the HTTP server is already running");

    if (config.port == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an HTTP server needs a port");

    /*
     * A secret that is present and too short is refused here rather than at the first request: a
     * server that came up and only fails when someone tries to log in is a server nobody tested.
     */
    if (config.secret != nullptr && config.secret_size > 0 &&
        (config.secret_size < NYA_HTTP_MIN_SECRET_BYTES || config.secret_size > NYA_HTTP_MAX_SECRET_BYTES)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a signing secret is %d to %d bytes", NYA_HTTP_MIN_SECRET_BYTES, NYA_HTTP_MAX_SECRET_BYTES);
    }

    if (config.layer_count > NYA_HTTP_MAX_LAYERS) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a server carries at most %d root layers", NYA_HTTP_MAX_LAYERS);
    }

    if (!NET_Init()) return nya_error(NYA_ERROR_NOT_OK, "SDL_net could not start: %s", SDL_GetError());

    /*
     * Loopback unless the caller named something else. Resolution is asynchronous in SDL_net, and a
     * name that needs the network is not something an engine should block a frame on, so the wait is
     * bounded and a slow one is a failure to start rather than a stall.
     */
    NYA_ConstCString requested = config.address[0] != '\0' ? config.address : "127.0.0.1";

    NET_Address* address = NET_ResolveHostname(requested);

    if (address == nullptr || NET_WaitUntilResolved(address, 1000) != 1) {
        NYA_Error failed = nya_error(NYA_ERROR_IO, "'%s' is not an address this machine can bind: %s", requested, SDL_GetError());

        NET_UnrefAddress(address);
        NET_Quit();

        return failed;
    }

    NET_Server* listener = NET_CreateServer(address, config.port, 0);

    NET_UnrefAddress(address);

    if (listener == nullptr) {
        NYA_Error failed = nya_error(NYA_ERROR_IO, "port %u could not be bound: %s", (u32)config.port, SDL_GetError());

        NET_Quit();

        return failed;
    }

    NYA_Arena* arena = nya_arena_create(.name = "http");

    if (arena == nullptr) {
        NET_DestroyServer(listener);
        NET_Quit();

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the HTTP server");
    }

    _NYA_HttpState* state = nya_arena_alloc(arena, sizeof(_NYA_HttpState));

    if (state == nullptr) {
        nya_arena_destroy(arena);
        NET_DestroyServer(listener);
        NET_Quit();

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

    // clamped rather than refused: a program asking for more threads than this has made a guess about
    // the machine, and a server that will not start is a worse answer than a server with eight workers.
    state->workers = nya_min(config.workers, (u32)NYA_HTTP_MAX_WORKERS);

    /*
     * One exchange per connection when threaded, so the connection table is what bounds the work in
     * flight and a slot never has to be found; one for the whole server otherwise, which is the single
     * request and response buffer this has always had.
     */
    state->slot_count = state->workers > 0 ? state->max_connections : 1;
    state->slots      = nya_arena_alloc(arena, sizeof(_NYA_HttpSlot) * state->slot_count);

    if (state->slots == nullptr) {
        nya_arena_destroy(arena);
        NET_DestroyServer(listener);
        NET_Quit();

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the HTTP server's exchanges");
    }

    nya_memset(state->slots, 0, sizeof(_NYA_HttpSlot) * state->slot_count);

    for (u32 index = 0; index < state->slot_count; index++) {
        state->slots[index].arena = nya_arena_create(.name = "http_exchange");

        if (state->slots[index].arena != nullptr) continue;

        for (u32 made = 0; made < index; made++) nya_arena_destroy(state->slots[made].arena);

        nya_arena_destroy(arena);
        NET_DestroyServer(listener);
        NET_Quit();

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

    /*
     * The thread that starts the server is the one its NYA_HTTP_AFFINITY_MAIN handlers run on, and the
     * one every main-thread-only module is checked against. Claimed only when this thread already
     * counts as the main one, so starting a server from a worker cannot move the claim.
     */
    if (nya_thread_main_is_current()) nya_thread_main_claim();

    NYA_Error started = _nya_http_threads_start(state);

    if (!started.ok) {
        for (u32 index = 0; index < state->slot_count; index++) nya_arena_destroy(state->slots[index].arena);

        _NYA_HTTP = nullptr;

        nya_arena_destroy(arena);
        NET_DestroyServer(listener);
        NET_Quit();

        return started;
    }

    // an atomic counter and a plain one have the same bytes; the registry only ever reads it, and it
    // is atomic because the listener thread is what moves it.
    nya_ceiling_register("http_connections", _NYA_HTTP->max_connections, (const u32*)&_NYA_HTTP->connection_count);
    nya_ceiling_register("http_rate_buckets", NYA_HTTP_MAX_RATE_BUCKETS, &_NYA_HTTP->bucket_count);
    nya_ceiling_register("http_websockets", NYA_HTTP_MAX_WEBSOCKETS, &_NYA_HTTP_WEBSOCKET_COUNT);

    if (state->workers > 0) {
        nya_log_info(
            "HTTP server listening on http://%s:%u, on its own thread with %u worker%s",
            requested,
            (u32)config.port,
            state->workers,
            state->workers == 1 ? "" : "s"
        );
    } else {
        nya_log_info("HTTP server listening on http://%s:%u", requested, (u32)config.port);
    }

    return NYA_OK;
}

void nya_system_http_deinit(void) {
    if (_NYA_HTTP == nullptr) return;

    _NYA_HttpState* state = _NYA_HTTP;

    // first, so a drain that arrives while the rest of this is running finds nothing to do.
    atomic_store_explicit(&state->stopping, true, memory_order_release);

    /*
     * The listener first and on its own: once it has returned nothing reads, writes or accepts a
     * socket, so everything below is happening to a table nobody else is looking at.
     */
    if (state->listener_thread != nullptr) nya_thread_join(state->listener_thread);

    // every worker woken at once: one between requests returns immediately, one inside a handler
    // returns when the handler does, and _nya_http_workers_join is where that stops being waited for.
    for (u32 index = 0; index < state->workers; index++) nya_semaphore_post(state->work);

    u32 stuck = _nya_http_workers_join(state);

    for (u32 index = 0; index < NYA_HTTP_MAX_CONNECTIONS; index++) _nya_http_close(&state->connections[index]);

    // after the connections, so every close was reported while its socket was still open.
    _nya_http_websocket_shutdown();

    NET_DestroyServer(state->listener);

    /*
     * A handler that ignored the deadline is still writing into its slot and reading the secret out of
     * this state, so the port goes back and the sockets close, and the memory does not: freeing it
     * would hand a running thread a use-after-free, and wiping the secret would be a write into a
     * buffer another thread is reading. The process is on its way out; a leak until then is the only
     * sound answer, and it is loud.
     */
    if (stuck > 0) {
        nya_log_error(
            "%u HTTP handler(s) were still running %d ms after shutdown started; the port is closed and their memory is leaked on purpose.",
            stuck,
            (s32)NYA_HTTP_SHUTDOWN_GRACE_MS
        );

        _NYA_HTTP = nullptr;

        NET_Quit();

        return;
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

    NET_Quit();
}

void nya_system_http_tick(void) {
    if (_NYA_HTTP == nullptr) return;

    /*
     * With workers the listener thread is doing the sockets, and what is left for the tick is the work
     * that has to happen where the program's own state is: the exchanges whose route asked for it, and
     * every WebSocket. Without them this is the whole drain, exactly as it has always been.
     */
    if (_NYA_HTTP->workers > 0) {
        _nya_http_main_drain();
        _nya_http_websockets_drain();

        return;
    }

    _nya_http_pass();
}

/*
 * ─────────────────────────────────────────────────────────
 * ROUTERS
 * ─────────────────────────────────────────────────────────
 */

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

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

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

/*
 * ─────────────────────────────────────────────────────────
 * SECRETS
 * ─────────────────────────────────────────────────────────
 */

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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_http_pass(void) {
    const b8 threaded = _NYA_HTTP->workers > 0;

    // the whole pass under one lock, and the only other thread that wants it is the tick draining the
    // WebSockets. A pass is non-blocking socket calls over at most eight connections, so what it makes
    // the tick wait for is microseconds; a handler never runs under it.
    nya_mutex_lock(_NYA_HTTP->table_mutex);
    defer nya_mutex_unlock(_NYA_HTTP->table_mutex);

    _nya_http_accept();

    u32 budget = NYA_HTTP_MAX_REQUESTS_PER_TICK;

    for (u32 index = 0; index < NYA_HTTP_MAX_CONNECTIONS; index++) {
        _NYA_HttpConnection* connection = &_NYA_HTTP->connections[index];

        if (connection->socket == nullptr) continue;

        // a connection that upgraded is drained by the websocket table, under its own bounds; it keeps
        // the slot it was accepted into, so it never escapes the ones above. With workers that drain is
        // the tick's, so the listener lets go of the socket here and never looks at it again.
        if (connection->upgraded) {
            if (threaded) continue;

            if (!_nya_http_websocket_tick(connection->socket)) _nya_http_close(connection);
            continue;
        }

        _NYA_HttpSlot* slot = &_NYA_HTTP->slots[threaded ? index : 0];

        if (threaded) {
            u32 slot_state = atomic_load_explicit(&slot->state, memory_order_acquire);

            /*
             * Queued or running: the exchange belongs to somebody else, and so does the connection. It
             * is not read from, not timed out and not closed until the answer is written, which is what
             * makes a slow handler cost its own connection and nothing else.
             */
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

        /*
         * Two reasons to drop a connection that is still open: it has gone quiet in the middle of a
         * request, and it has stopped reading what we already sent. Both are bounds rather than
         * checks, because neither has a version that is safe to wait out.
         */
        /*
         * Read after the work rather than once at the top of the tick: receiving stamps the connection
         * with a fresh reading, so a timestamp taken before it is behind the one being subtracted from
         * it, and the difference of two unsigned times in that order is an enormous number.
         */
        u64 now_ns = nya_clock_get_monotonic_ns();

        if (now_ns > connection->active_at_ns && now_ns - connection->active_at_ns > (u64)NYA_HTTP_IDLE_TIMEOUT_MS * 1000000ULL) {
            _nya_http_close(connection);
            continue;
        }

        s32 pending = NET_GetStreamSocketPendingWrites(connection->socket);

        if (pending < 0 || (u64)pending > NYA_HTTP_MAX_PENDING_WRITE_BYTES) {
            _nya_http_close(connection);
            continue;
        }

        if (connection->closing && pending == 0) _nya_http_close(connection);
    }
}

void _nya_http_accept(void) {
    for (u32 accepted = 0; accepted < NYA_HTTP_MAX_ACCEPTS_PER_TICK; accepted++) {
        NET_StreamSocket* socket = nullptr;

        if (!NET_AcceptClient(_NYA_HTTP->listener, &socket)) {
            nya_log_warn("The HTTP listener failed to accept: %s", SDL_GetError());
            return;
        }

        if (socket == nullptr) return;

        _NYA_HttpConnection* slot = nullptr;

        for (u32 index = 0; index < _NYA_HTTP->max_connections; index++) {
            if (_NYA_HTTP->connections[index].socket != nullptr) continue;

            slot = &_NYA_HTTP->connections[index];
            break;
        }

        // the connection past the last is closed now rather than queued, so a process that loops on
        // connect cannot grow anything here.
        if (slot == nullptr) {
            NET_DestroyStreamSocket(socket);
            continue;
        }

        // the peer as the socket reports it. A header could claim anything, so the limits never read one.
        char         address[NYA_HTTP_MAX_ADDRESS] = { 0 };
        NET_Address* peer                          = NET_GetStreamSocketAddress(socket);
        NYA_ConstCString text                      = peer != nullptr ? NET_GetAddressString(peer) : nullptr;
        (void)snprintf(address, sizeof(address), "%s", text != nullptr ? text : "unknown");
        NET_UnrefAddress(peer);

        // and one address cannot take every slot. Closed rather than queued, like the connection past the last.
        u32 held = 0;
        for (u32 index = 0; index < _NYA_HTTP->max_connections; index++) {
            if (_NYA_HTTP->connections[index].socket != nullptr && strcmp(_NYA_HTTP->connections[index].address, address) == 0) held++;
        }
        if (held >= _NYA_HTTP->max_connections_per_address) {
            NET_DestroyStreamSocket(socket);
            continue;
        }

        *slot = (_NYA_HttpConnection){ .socket = socket, .active_at_ns = nya_clock_get_monotonic_ns() };
        (void)snprintf(slot->address, sizeof(slot->address), "%s", address);

        (void)atomic_fetch_add_explicit(&_NYA_HTTP->connection_count, 1, memory_order_relaxed);
    }
}

b8 _nya_http_receive(_NYA_HttpConnection* connection, _NYA_HttpSlot* slot) {
    u64 room = NYA_HTTP_MAX_REQUEST_BYTES - connection->received_size;

    /*
     * A full buffer with no complete request in it: the peer has sent more than the worst legal
     * request and there is nothing left to wait for.
     */
    if (room == 0) {
        _nya_http_refuse(connection, slot, NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE, "the request could not be parsed", 0);
        return false;
    }

    // SDL_net takes an int. The buffer is fifteen kilobytes, so this cannot narrow; the clamp is here
    // so that stays true if the bound ever grows.
    s32 wanted = room > (u64)S32_MAX ? S32_MAX : (s32)room;

    s32 read = NET_ReadFromStreamSocket(connection->socket, connection->received + connection->received_size, wanted);

    if (read < 0) return false;

    if (read == 0) return true;

    connection->received_size += (u64)read;
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

        // shifted before the answer, so a handler cannot see a half-consumed stream and a pipelined
        // second request is already at the front when the first one's answer is queued.
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

        // the table as it is right now, so a merge from the frame decides the next request and never
        // this one. Read under the lock this pass already holds.
        slot->router_count = _NYA_HTTP->router_count;
        for (u32 router = 0; router < _NYA_HTTP->router_count; router++) slot->routers[router] = _NYA_HTTP->routers[router];

        if (threaded) {
            // handed over, and the connection is not read from again until the answer comes back.
            _nya_http_queue(_NYA_HTTP, index, slot);
            return true;
        }

        /*
         * The upgrade is answered here rather than through the router: what follows a 101 is frames and
         * not a response, so it cannot go through nya_http_response_head. Everything before this point
         * still happened to it — it was accepted under the connection bounds, parsed by the same parser
         * and has spent its token — and the rest of the handshake's rules are the websocket's.
         */
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
    // everything the exchange allocates dies with it. Cleared at the start rather than the end so a
    // response body that points into it is still valid while it is being written.
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

    // every line the request causes carries its id, so a report quoting X-Request-Id finds all of them.
    // The tag is this thread's, so two workers never land on each other's lines.
    char tag[NYA_LOG_TAG_MAX_LENGTH] = { 0 };
    (void)snprintf(tag, sizeof(tag), "req=%s", slot->response.request_id);
    nya_log_tag_set(tag);
    defer nya_log_tag_clear();

    slot->answer = _NYA_HTTP_ANSWER_WRITE;
    slot->status = nya_http_router_dispatch(&exchange, slot->routers, slot->router_count, state->layers, state->layer_count);
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

    // the answer is the other half of "active": without this a connection whose handler took longer
    // than the idle timeout would be closed the moment its answer went out.
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

    // never keep-alive: a stream the parser gave up on cannot be resynchronised, and guessing where
    // the next request starts is the request smuggling bug this refuses in the first place. A peer
    // over its budget is closed too, since anything it pipelined behind the refused request is unread.
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

    nya_assert(head_size <= (u64)S32_MAX, "the head buffer is four kilobytes");

    if (!NET_WriteToStreamSocket(connection->socket, head, (s32)head_size)) return false;

    // a HEAD carries the Content-Length its GET would have and none of the bytes, which is what makes
    // it a HEAD rather than a GET nobody read.
    if (head_only || response->body_size == 0) return true;

    nya_assert(response->body_size <= (u64)S32_MAX, "the response buffer is sixty four kilobytes");

    return NET_WriteToStreamSocket(connection->socket, response->body, (s32)response->body_size);
}

void _nya_http_request_id(const _NYA_HttpState* state, OUT char* out) {
    u8 bits[8] = { 0 };

    // an id only has to be unique, not secret, so a CSPRNG that fails falls back to the request count
    // rather than refusing the request.
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

    // a new address starts full. Past the table's bound the budget touched longest ago makes room; see the bound.
    if (bucket == nullptr) {
        if (_NYA_HTTP->bucket_count < NYA_HTTP_MAX_RATE_BUCKETS) {
            bucket = &_NYA_HTTP->buckets[_NYA_HTTP->bucket_count++];
        } else {
            bucket = &_NYA_HTTP->buckets[0];
            for (u32 index = 1; index < NYA_HTTP_MAX_RATE_BUCKETS; index++) {
                if (_NYA_HTTP->buckets[index].refilled_at_ns < bucket->refilled_at_ns) bucket = &_NYA_HTTP->buckets[index];
            }
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
    if (connection->socket == nullptr) return;

    // the report that the socket is gone, while the socket is still the thing that is going.
    if (connection->upgraded) _nya_http_websocket_detach(connection->socket);

    NET_DestroyStreamSocket(connection->socket);

    *connection = (_NYA_HttpConnection){ 0 };

    nya_assert(atomic_load_explicit(&_NYA_HTTP->connection_count, memory_order_relaxed) > 0, "a connection was closed that was never counted");
    (void)atomic_fetch_sub_explicit(&_NYA_HTTP->connection_count, 1, memory_order_relaxed);
}

/*
 * ─────────────────────────────────────────────────────────
 * THE LISTENER AND THE POOL
 * ─────────────────────────────────────────────────────────
 */

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
    // the handshake is not a route, and what it registers into is the WebSocket table, which belongs
    // to whoever ticks; see http_server.h.
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

    // first in, first out, over a table of eight: a shift is four moves and a ring would be state to
    // get wrong for no measurable gain.
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

        if (connection->socket == nullptr || !connection->upgraded) continue;

        if (!_nya_http_websocket_tick(connection->socket)) _nya_http_close(connection);
    }
}

void _nya_http_listener_wait(_NYA_HttpState* state) {
    void* watched[NYA_HTTP_MAX_CONNECTIONS + 1] = { 0 };
    s32   watched_count                         = 0;
    b8    busy                                  = false;

    nya_mutex_lock(state->table_mutex);
    {
        watched[watched_count++] = state->listener;

        for (u32 index = 0; index < NYA_HTTP_MAX_CONNECTIONS; index++) {
            _NYA_HttpConnection* connection = &state->connections[index];

            if (connection->socket == nullptr) continue;

            /*
             * An upgraded socket is the tick's and may be destroyed by it at any moment, so it never
             * goes into a set this thread is about to sleep on. While one is open the listener polls
             * instead, which costs a wakeup a millisecond and is the price of the socket being
             * somebody else's.
             */
            if (connection->upgraded) {
                busy = true;
                continue;
            }

            // an answer that is being worked on cannot wake a socket, and bytes already buffered will
            // not arrive again; both are a reason to come straight back rather than sleep.
            if (atomic_load_explicit(&state->slots[index].state, memory_order_acquire) != _NYA_HTTP_SLOT_IDLE) busy = true;
            if (connection->received_size > 0) busy = true;

            watched[watched_count++] = connection->socket;
        }
    }
    nya_mutex_unlock(state->table_mutex);

    if (busy) {
        SDL_Delay(_NYA_HTTP_LISTENER_BUSY_MS);
        return;
    }

    (void)NET_WaitUntilInputAvailable(watched, watched_count, _NYA_HTTP_LISTENER_IDLE_MS);
}

void _nya_http_listener_thread(void* data) {
    _NYA_HttpState* state = (_NYA_HttpState*)data;

    while (!atomic_load_explicit(&state->stopping, memory_order_acquire)) {
        _nya_http_pass();
        _nya_http_listener_wait(state);
    }

    _nya_http_thread_end();
}

void _nya_http_worker_thread(void* data) {
    _NYA_HttpState* state = (_NYA_HttpState*)data;

    /*
     * The state is handed over rather than read from the global, because a handler that outruns the
     * shutdown deadline is still here after _NYA_HTTP has been cleared and its memory deliberately
     * leaked; see nya_system_http_deinit.
     */
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

    _nya_http_thread_end();
}

/*
 * These threads are the engine's own and SDL has never heard of them, but the sockets they work are
 * SDL_net's, and SDL keeps a per thread error buffer for whoever calls into it. SDL frees that for the
 * threads it started itself and at SDL_Quit for the main one, so a thread of ours that has talked to
 * SDL_net has to say when it is done or that buffer is still allocated when the process ends. It goes
 * when the sockets stop being SDL's.
 */
void _nya_http_thread_end(void) {
    SDL_CleanupTLS();
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

    // the workers first: a listener with nobody to hand an exchange to would queue one before the pool
    // exists, and starting the pool afterwards would be a race for no reason.
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

        while (!nya_thread_is_finished(thread) && nya_clock_get_monotonic_ns() < deadline_ns) SDL_Delay(1);

        /*
         * Still inside a handler at the deadline. There is no way to stop a thread from out here that
         * is not worse than the problem — it may be holding a lock, or halfway through a write — so it
         * is let go of and the deadline is kept by everybody else.
         */
        if (!nya_thread_is_finished(thread)) {
            nya_thread_abandon(thread);
            stuck++;
            continue;
        }

        nya_thread_join(thread);
    }

    return stuck;
}
