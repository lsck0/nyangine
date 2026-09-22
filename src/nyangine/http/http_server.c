#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "SDL3/SDL_error.h"

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_ceiling.h"
#include "nyangine/base/base_compare.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/base/base_string.h"
#include "nyangine/core/core_event.h"
#include "nyangine/http/http_server.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/os/os_random.h"
#include "SDL3_net/SDL_net.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct _NYA_HttpConnection _NYA_HttpConnection;
typedef struct _NYA_HttpState      _NYA_HttpState;

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

    /** The peer, as SDL_net spells it. What the per address limits key on, and never a forwarded header. */
    char address[NYA_HTTP_MAX_ADDRESS];
};

/** One address's request budget: a token bucket, refilled continuously and spent one token per request. */
typedef struct {
    char address[NYA_HTTP_MAX_ADDRESS];
    f64  tokens;
    u64  refilled_at_ns;
} _NYA_HttpRateBucket;

struct _NYA_HttpState {
    NYA_Arena* allocator;

    /** Emptied at the start of every exchange. Nothing here outlives the request it was built for. */
    NYA_Arena* scratch;

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

    /** What nya_ceiling_register publishes, so the overlay can show how full the table is. */
    u32 connection_count;

    u64 request_count;

    NYA_CallbackHandle frame_hook;

    /** One request and one response buffer for the whole server: the drain answers one at a time. */
    NYA_HttpRequest request;
    u8              response_body[NYA_HTTP_MAX_RESPONSE_BYTES];
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

/** Takes whatever the listener has, up to NYA_HTTP_MAX_ACCEPTS_PER_TICK. */
NYA_INTERNAL void _nya_http_accept(void);

/** Reads what has arrived on one connection. False when the connection is finished with. */
NYA_INTERNAL b8 _nya_http_receive(_NYA_HttpConnection* connection) __attr_no_discard;

/** Parses and answers as many complete requests as `budget` allows. False when the connection is finished. */
NYA_INTERNAL b8 _nya_http_handle(_NYA_HttpConnection* connection, u32* budget) __attr_no_discard;

/** Dispatches one parsed request and writes the answer. False when the connection is finished. */
NYA_INTERNAL b8 _nya_http_answer(_NYA_HttpConnection* connection, b8 keep_alive) __attr_no_discard;

/** Writes one status with a problem body and nothing else, for a request that never became one. A nonzero `retry_after_s` becomes `Retry-After`. */
NYA_INTERNAL void _nya_http_refuse(_NYA_HttpConnection* connection, NYA_HttpStatus status, NYA_ConstCString detail, u32 retry_after_s);

/** A fresh request id: 64 bits from the CSPRNG as hex. */
NYA_INTERNAL void _nya_http_request_id(OUT char* out);

/** Spends one token from `address`'s bucket. False when it is empty, with the seconds until one refills. */
NYA_INTERNAL b8 _nya_http_rate_take(NYA_ConstCString address, OUT u32* out_retry_after_s) __attr_no_discard;

/** Renders and queues a response. False when the socket has failed. */
NYA_INTERNAL b8 _nya_http_write(_NYA_HttpConnection* connection, const NYA_HttpResponse* response, NYA_HttpStatus status, b8 keep_alive, b8 head_only)
    __attr_no_discard;

/** Closes one connection and frees its slot. Idempotent. */
NYA_INTERNAL void _nya_http_close(_NYA_HttpConnection* connection);

/** The frame hook, which is nya_system_http_tick behind the event signature. */
NYA_INTERNAL void _nya_http_on_frame(NYA_Event* event);

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
    state->scratch   = nya_arena_create(.name = "http_exchange");
    state->listener  = listener;
    state->port      = config.port;
    state->max_connections =
        config.max_connections == 0 || config.max_connections > NYA_HTTP_MAX_CONNECTIONS ? NYA_HTTP_MAX_CONNECTIONS : config.max_connections;
    state->max_connections_per_address = config.max_connections_per_address != 0 ? config.max_connections_per_address : NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS;
    state->requests_per_second         = config.requests_per_second != 0 ? config.requests_per_second : NYA_HTTP_DEFAULT_REQUESTS_PER_SECOND;
    state->request_burst               = config.request_burst != 0 ? config.request_burst : NYA_HTTP_DEFAULT_REQUEST_BURST;
    state->max_connections_per_address = nya_min(state->max_connections_per_address, state->max_connections);

    if (state->scratch == nullptr) {
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

    nya_ceiling_register("http_connections", _NYA_HTTP->max_connections, &_NYA_HTTP->connection_count);
    nya_ceiling_register("http_rate_buckets", NYA_HTTP_MAX_RATE_BUCKETS, &_NYA_HTTP->bucket_count);

    /*
     * Drained where input is drained, for the same reason the control socket is: a request is input
     * like a keypress, so it lands at the same point in the frame.
     *
     * Only when there is a frame. A callback and an event hook both live in the app, so a program with
     * no app — a headless tool, a test — cannot register one, and drives nya_system_http_tick itself.
     * Serving over HTTP is not a reason to require a window and a frame loop.
     */
    if (_NYA_APP_INSTANCE.initialized) {
        state->frame_hook = nya_callback(_nya_http_on_frame);

        nya_event_hook_register((NYA_EventHook){
            .event_type = NYA_EVENT_HANDLING_STARTED,
            .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
            .fn         = state->frame_hook,
        });
    } else {
        nya_log_debug("No app is running, so the HTTP drain is the caller's to run; see nya_system_http_tick.");
    }

    nya_log_info("HTTP server listening on http://%s:%u", requested, (u32)config.port);

    return NYA_OK;
}

void nya_system_http_deinit(void) {
    if (_NYA_HTTP == nullptr) return;

    // only if one was registered, and only while the app that owns it is still up; see init.
    if (_NYA_HTTP->frame_hook != 0 && _NYA_APP_INSTANCE.initialized) {
        nya_event_hook_unregister((NYA_EventHook){
            .event_type = NYA_EVENT_HANDLING_STARTED,
            .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
            .fn         = _NYA_HTTP->frame_hook,
        });
    }

    for (u32 index = 0; index < NYA_HTTP_MAX_CONNECTIONS; index++) _nya_http_close(&_NYA_HTTP->connections[index]);

    NET_DestroyServer(_NYA_HTTP->listener);

    // the secret leaves no copy behind in a freed region waiting to be handed out again.
    memset(_NYA_HTTP->secret, 0, sizeof(_NYA_HTTP->secret));

    nya_arena_destroy(_NYA_HTTP->scratch);

    NYA_Arena* arena = _NYA_HTTP->allocator;
    _NYA_HTTP        = nullptr;

    nya_arena_destroy(arena);

    NET_Quit();
}

void nya_system_http_tick(void) {
    if (_NYA_HTTP == nullptr) return;

    _nya_http_accept();

    u32 budget = NYA_HTTP_MAX_REQUESTS_PER_TICK;

    for (u32 index = 0; index < NYA_HTTP_MAX_CONNECTIONS; index++) {
        _NYA_HttpConnection* connection = &_NYA_HTTP->connections[index];

        if (connection->socket == nullptr) continue;

        if (!_nya_http_receive(connection)) {
            _nya_http_close(connection);
            continue;
        }

        if (!_nya_http_handle(connection, &budget)) {
            _nya_http_close(connection);
            continue;
        }

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

/*
 * ─────────────────────────────────────────────────────────
 * ROUTERS
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_http_server_merge(const NYA_HttpRouter* router) {
    if (_NYA_HTTP == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "the HTTP server is not running");

    NYA_TRY(nya_http_router_check(router));

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
    return _NYA_HTTP != nullptr ? _NYA_HTTP->connection_count : 0;
}

u64 nya_http_server_request_count(void) {
    return _NYA_HTTP != nullptr ? _NYA_HTTP->request_count : 0;
}

u32 nya_http_server_router_count(void) {
    return _NYA_HTTP != nullptr ? _NYA_HTTP->router_count : 0;
}

const NYA_HttpRouter* nya_http_server_router_at(u32 index) {
    if (_NYA_HTTP == nullptr || index >= _NYA_HTTP->router_count) return nullptr;

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

        _NYA_HTTP->connection_count++;
    }
}

b8 _nya_http_receive(_NYA_HttpConnection* connection) {
    u64 room = NYA_HTTP_MAX_REQUEST_BYTES - connection->received_size;

    /*
     * A full buffer with no complete request in it: the peer has sent more than the worst legal
     * request and there is nothing left to wait for.
     */
    if (room == 0) {
        _nya_http_refuse(connection, NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE, "the request could not be parsed", 0);
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

b8 _nya_http_handle(_NYA_HttpConnection* connection, u32* budget) {
    while (*budget > 0 && connection->received_size > 0 && !connection->closing) {
        u64            consumed = 0;
        NYA_HttpStatus refusal  = NYA_HTTP_STATUS_NONE;

        NYA_HttpParse parsed = nya_http_request_parse(connection->received, connection->received_size, &_NYA_HTTP->request, &consumed, &refusal);

        if (parsed == NYA_HTTP_PARSE_INCOMPLETE) return true;

        if (parsed == NYA_HTTP_PARSE_REFUSED) {
            _nya_http_refuse(connection, refusal, "the request could not be parsed", 0);
            return false;
        }

        // counted once a request has parsed, so a slow sender is the idle timeout's to deal with and not this.
        u32 retry_after_s = 0;
        if (!_nya_http_rate_take(connection->address, &retry_after_s)) {
            _nya_http_refuse(connection, NYA_HTTP_STATUS_TOO_MANY_REQUESTS, "this address has sent more requests than it may; see Retry-After", retry_after_s);
            return false;
        }

        nya_assert(consumed > 0 && consumed <= connection->received_size, "the parser reported consuming more than it was given");

        b8 keep_alive = _NYA_HTTP->request.keep_alive;

        // shifted before the answer, so a handler cannot see a half-consumed stream and a pipelined
        // second request is already at the front when the first one's answer is queued.
        connection->received_size -= consumed;

        if (connection->received_size > 0) memmove(connection->received, connection->received + consumed, connection->received_size);

        (*budget)--;
        _NYA_HTTP->request_count++;

        if (!_nya_http_answer(connection, keep_alive)) return false;

        if (!keep_alive) {
            connection->closing = true;
            return true;
        }
    }

    return true;
}

b8 _nya_http_answer(_NYA_HttpConnection* connection, b8 keep_alive) {
    // everything the exchange allocates dies with it. Cleared at the start rather than the end so a
    // response body that points into it is still valid while it is being written.
    nya_arena_free_all(_NYA_HTTP->scratch);

    NYA_HttpResponse response = { 0 };
    nya_http_response_create(&response, _NYA_HTTP->response_body, sizeof(_NYA_HTTP->response_body));
    defer nya_http_response_destroy(&response);

    _nya_http_request_id(response.request_id);

    NYA_HttpExchange exchange = {
        .request     = &_NYA_HTTP->request,
        .response    = &response,
        .arena       = _NYA_HTTP->scratch,
        .secret      = _NYA_HTTP->secret_size > 0 ? _NYA_HTTP->secret : nullptr,
        .secret_size = _NYA_HTTP->secret_size,
        .now_s       = nya_clock_get_timestamp_s(),
        .started_ns  = nya_clock_get_monotonic_ns(),
        .address     = connection->address,
    };

    // every line the request causes carries its id, so a report quoting X-Request-Id finds all of them.
    char tag[NYA_LOG_TAG_MAX_LENGTH] = { 0 };
    (void)snprintf(tag, sizeof(tag), "req=%s", response.request_id);
    nya_log_tag_set(tag);
    defer nya_log_tag_clear();

    NYA_HttpStatus status =
        nya_http_router_dispatch(&exchange, _NYA_HTTP->routers, _NYA_HTTP->router_count, _NYA_HTTP->layers, _NYA_HTTP->layer_count);

    b8 head_only = _NYA_HTTP->request.method == NYA_HTTP_METHOD_HEAD;

    return _nya_http_write(connection, &response, status, keep_alive, head_only);
}

void _nya_http_refuse(_NYA_HttpConnection* connection, NYA_HttpStatus status, NYA_ConstCString detail, u32 retry_after_s) {
    nya_assert(detail != nullptr);

    nya_arena_free_all(_NYA_HTTP->scratch);

    NYA_HttpResponse response = { 0 };
    nya_http_response_create(&response, _NYA_HTTP->response_body, sizeof(_NYA_HTTP->response_body));
    defer nya_http_response_destroy(&response);

    _nya_http_request_id(response.request_id);

    NYA_Error written = nya_http_response_printf(
        &response,
        NYA_HTTP_MEDIA_JSON,
        "{\"status\":%d,\"error\":\"%s\",\"detail\":\"%s\"}",
        (s32)status,
        nya_http_status_text(status),
        detail
    );

    if (!written.ok) nya_http_response_reset(&response);

    // a client told to slow down is told when to come back.
    if (retry_after_s > 0) {
        char seconds[16] = { 0 };
        (void)snprintf(seconds, sizeof(seconds), "%u", retry_after_s);
        if (!nya_http_response_header(&response, "Retry-After", seconds).ok) nya_http_response_reset(&response);
    }

    // never keep-alive: a stream the parser gave up on cannot be resynchronised, and guessing where
    // the next request starts is the request smuggling bug this refuses in the first place. A peer
    // over its budget is closed too, since anything it pipelined behind the refused request is unread.
    (void)_nya_http_write(connection, &response, status, false, false);
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

void _nya_http_request_id(OUT char* out) {
    u8 bits[8] = { 0 };

    // an id only has to be unique, not secret, so a CSPRNG that fails falls back to the request count
    // rather than refusing the request.
    if (!nya_os_random_bytes(bits, sizeof(bits))) {
        for (u32 index = 0; index < sizeof(bits); index++) bits[index] = (u8)(_NYA_HTTP->request_count >> (index * 8));
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

    NET_DestroyStreamSocket(connection->socket);

    *connection = (_NYA_HttpConnection){ 0 };

    nya_assert(_NYA_HTTP->connection_count > 0, "a connection was closed that was never counted");
    _NYA_HTTP->connection_count--;
}

void _nya_http_on_frame(NYA_Event* event) {
    nya_unused(event);

    nya_system_http_tick();
}
