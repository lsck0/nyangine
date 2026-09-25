#include "nyangine-core/nyangine.h"

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/* ── the opcodes, as Discord numbers them ── */

#define _NYA_DISCORD_OP_DISPATCH        0
#define _NYA_DISCORD_OP_HEARTBEAT       1
#define _NYA_DISCORD_OP_IDENTIFY        2
#define _NYA_DISCORD_OP_RESUME          6
#define _NYA_DISCORD_OP_RECONNECT       7
#define _NYA_DISCORD_OP_INVALID_SESSION 9
#define _NYA_DISCORD_OP_HELLO           10
#define _NYA_DISCORD_OP_HEARTBEAT_ACK   11

/**
 * The code this end closes with when it has decided the connection is over.
 *
 * 4000 and not 1000: Discord treats a 1000 as "I am done with this session" and throws the session away,
 * where anything else leaves it resumable. Closing a zombie with 1000 would turn every missed heartbeat
 * into a fresh IDENTIFY, which is the expensive one.
 * */
#define _NYA_DISCORD_CLOSE_RESUMABLE 4000

/** The shortest and longest wait after an INVALID SESSION, which Discord documents as one to five seconds. */
#define _NYA_DISCORD_INVALID_SESSION_MIN_MS 1000
#define _NYA_DISCORD_INVALID_SESSION_MAX_MS 5000

/** Where IDENTIFY and RESUME are assembled. The token, a session id and the fixed JSON around them. */
#define _NYA_DISCORD_LOGIN_BYTES (NYA_DISCORD_GATEWAY_MAX_TOKEN + NYA_DISCORD_GATEWAY_MAX_SESSION + 512)

/**
 * What IDENTIFY reports as the host, which Discord shows on the bot's connection.
 *
 * A fixed string per target rather than the real distribution and kernel: those would be this machine's
 * fingerprint, sent to a third party on every login, for a field nothing acts on.
 * */
#if OS_WINDOWS
#define _NYA_DISCORD_OS_NAME "windows"
#elif OS_LINUX
#define _NYA_DISCORD_OS_NAME "linux"
#else
#define _NYA_DISCORD_OS_NAME "unknown"
#endif

typedef struct _NYA_DiscordGatewaySocket _NYA_DiscordGatewaySocket;

/** What the default transport holds: one websocket at a time, and what to open the next one with. */
struct _NYA_DiscordGatewaySocket {
    NYA_Arena*     arena;
    NYA_WebSocket* socket;
    u64            max_message_bytes;
    b8             insecure_skip_tls_verify;
};

struct NYA_DiscordGateway {
    NYA_Arena* allocator;

    /** Only the default transport's, so its reconnects free back into an arena nothing else uses. */
    NYA_Arena* sockets;

    /** Where a decoded payload lives. Emptied at the top of every poll, so a long session grows nothing. */
    NYA_Arena* payloads;

    NYA_DiscordGatewayState     state;
    NYA_DiscordGatewayTransport transport;

    /** Null when the caller brought its own transport. */
    _NYA_DiscordGatewaySocket* owned_socket;

    char token[NYA_DISCORD_GATEWAY_MAX_TOKEN];
    u32  intents;
    u32  shard_id;
    u32  shard_count;
    u32  max_reconnect_attempts;

    /** Where a fresh IDENTIFY dials, and where a RESUME does. The second is empty until a READY names it. */
    char url[NYA_DISCORD_GATEWAY_MAX_URL];
    char resume_url[NYA_DISCORD_GATEWAY_MAX_URL];

    char session[NYA_DISCORD_GATEWAY_MAX_SESSION];
    s64  sequence;

    u64 heartbeat_interval_ms;
    u64 heartbeat_at_ms;

    /** A heartbeat went out and its ACK has not. Two of these in a row is a dead connection. */
    b8 awaiting_ack;

    /** HELLO arrived and the login has not gone out yet. */
    b8 pending_login;

    /** The login to send is a RESUME rather than an IDENTIFY. */
    b8 resume_wanted;

    /** Discord's one IDENTIFY per five seconds, held here rather than trusted to the backoff. */
    u64 identify_allowed_at_ms;

    /** Reconnects since the last successful login, which is what the backoff is measured in. */
    u32 attempt;
    u64 reconnect_at_ms;

    char event_name[NYA_DISCORD_GATEWAY_MAX_EVENT_NAME];
    char reason[NYA_WEBSOCKET_MAX_CLOSE_REASON_BYTES + 1];

    /** Where a login is built, and wiped the moment the transport has taken it. Never the caller's arena. */
    char login[_NYA_DISCORD_LOGIN_BYTES];
};

/* ── the default transport ── */

NYA_INTERNAL NYA_Error _nya_discord_socket_open(void* user, NYA_ConstCString url) __attr_no_discard;
NYA_INTERNAL void      _nya_discord_socket_close(void* user);
NYA_INTERNAL b8        _nya_discord_socket_poll(void* user, OUT NYA_WebSocketEvent* out_event);
NYA_INTERNAL NYA_Error _nya_discord_socket_send(void* user, const char* text, u64 size) __attr_no_discard;
NYA_INTERNAL u64       _nya_discord_socket_now_ms(void* user) __attr_no_discard;
NYA_INTERNAL f32       _nya_discord_socket_jitter(void* user) __attr_no_discard;

/* ── the payloads ── */

/** Whether `token` is something that can go into a JSON string with no escaping. A real token always is. */
NYA_INTERNAL b8 _nya_discord_token_is_plain(NYA_ConstCString token, u64 length) __attr_no_discard;

/** Builds and queues IDENTIFY or RESUME, then wipes the buffer it was built in. */
NYA_INTERNAL void _nya_discord_gateway_login(NYA_DiscordGateway* gateway, u64 now_ms);

/** Queues a heartbeat carrying the last sequence, and arms the next one. */
NYA_INTERNAL NYA_Error _nya_discord_gateway_heartbeat(NYA_DiscordGateway* gateway, u64 now_ms) __attr_no_discard;

/** Opens a connection to whichever url the next login needs. */
NYA_INTERNAL NYA_Error _nya_discord_gateway_dial(NYA_DiscordGateway* gateway) __attr_no_discard;

/**
 * Ends the connection, decides whether it comes back, and fills the event that says so. Always produces
 * one.
 * */
NYA_INTERNAL void _nya_discord_gateway_drop(NYA_DiscordGateway* gateway, u16 code, NYA_ConstCString reason, u64 now_ms,
                                            OUT NYA_DiscordGatewayEvent* out_event);

/** Handles one text payload. True when it produced an event for the caller. */
NYA_INTERNAL b8 _nya_discord_gateway_payload(NYA_DiscordGateway* gateway, const u8* text, u64 size, u64 now_ms,
                                             OUT NYA_DiscordGatewayEvent* out_event);

/** Handles a DISPATCH, which is READY, RESUMED, or something for the caller. */
NYA_INTERNAL b8 _nya_discord_gateway_dispatch(NYA_DiscordGateway* gateway, const NYA_Object* payload, OUT NYA_DiscordGatewayEvent* out_event);

/** `object[key]` as a string, or null when it is missing or is not one. */
NYA_INTERNAL NYA_ConstCString _nya_discord_string_at(const NYA_Object* object, NYA_CString key) __attr_no_discard;

/** `object[key]` as an integer, or `fallback` when it is missing or is not one. */
NYA_INTERNAL s64 _nya_discord_integer_at(const NYA_Object* object, NYA_CString key, s64 fallback) __attr_no_discard;

/** Copies `text` into a fixed buffer, truncating. For names and reasons, where a long one is not an error. */
NYA_INTERNAL void _nya_discord_copy(OUT char* destination, u64 capacity, NYA_ConstCString text);

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_DiscordGatewayCloseAction nya_discord_gateway_close_action(u16 code) {
    switch (code) {
        // The token, shard or intents: none become true by asking again, and Discord disables a token that keeps asking, so each ends this client.
        case 4004:  // authentication failed
        case 4010:  // invalid shard
        case 4011:  // sharding required
        case 4012:  // invalid api version
        case 4013:  // invalid intents
        case 4014:  // disallowed intents
            return NYA_DISCORD_GATEWAY_CLOSE_FATAL;

        // The session is gone; resuming would be refused, so the next login is an IDENTIFY.
        case NYA_WEBSOCKET_CLOSE_NORMAL:      // this end said it was done, or Discord did
        case NYA_WEBSOCKET_CLOSE_GOING_AWAY:
        case 4003:  // not authenticated
        case 4007:  // invalid sequence
        case 4009:  // session timed out
            return NYA_DISCORD_GATEWAY_CLOSE_REIDENTIFY;

        // A broken connection rather than an ended session: the events since the last sequence still wait, so reconnect and RESUME.
        case NYA_WEBSOCKET_CLOSE_ABNORMAL:
        case 4000:  // unknown error
        case 4001:  // unknown opcode
        case 4002:  // decode error
        case 4005:  // already authenticated
        case 4008:  // rate limited
            return NYA_DISCORD_GATEWAY_CLOSE_RESUME;

        // Discord adds codes; an unknown one is treated as a lost session, since a wrong RESUME costs only a round trip where a wrong IDENTIFY loses the events between.
        default: return NYA_DISCORD_GATEWAY_CLOSE_REIDENTIFY;
    }
}

u64 nya_discord_gateway_backoff_ms(u32 attempt) {
    u64 delay = NYA_DISCORD_GATEWAY_BACKOFF_MIN_MS;

    // Shifted, not multiplied in a loop, and capped by the shift count first so the doubling cannot run off a u64 after a very long failure.
    u32 doublings = attempt > 16 ? 16 : attempt;
    delay <<= doublings;

    return delay > NYA_DISCORD_GATEWAY_BACKOFF_MAX_MS ? (u64)NYA_DISCORD_GATEWAY_BACKOFF_MAX_MS : delay;
}

NYA_ConstCString nya_discord_gateway_state_name(NYA_DiscordGatewayState state) {
    switch (state) {
        case NYA_DISCORD_GATEWAY_STATE_IDLE:        return "idle";
        case NYA_DISCORD_GATEWAY_STATE_CONNECTING:  return "connecting";
        case NYA_DISCORD_GATEWAY_STATE_IDENTIFYING: return "identifying";
        case NYA_DISCORD_GATEWAY_STATE_READY:       return "ready";
        case NYA_DISCORD_GATEWAY_STATE_FATAL:       return "fatal";
        case NYA_DISCORD_GATEWAY_STATE_COUNT:
        default:                                    return "unknown";
    }
}

// ───────────────────────────────────── LIFETIME ─────────────────────────────────────

NYA_Error nya_discord_gateway_create(NYA_Arena* arena, NYA_DiscordGatewayOptions options, OUT NYA_DiscordGateway** out_gateway) {
    nya_assert(arena != nullptr);
    nya_assert(out_gateway != nullptr);

    if (options.token == nullptr || options.token[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a bot gateway needs a token");

    u64 token_length = strlen(options.token);

    // The token never reaches an error message, so these say what was wrong without quoting it.
    if (token_length >= NYA_DISCORD_GATEWAY_MAX_TOKEN) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the token is longer than %d bytes", NYA_DISCORD_GATEWAY_MAX_TOKEN - 1);
    }

    if (!_nya_discord_token_is_plain(options.token, token_length)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the token carries a byte a real one never does");
    }

    if (options.shard_count > 0 && options.shard_id >= options.shard_count) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "shard %u of %u does not exist", options.shard_id, options.shard_count);
    }

    NYA_ConstCString url = options.url != nullptr && options.url[0] != '\0' ? options.url : NYA_DISCORD_GATEWAY_URL;
    if (strlen(url) >= NYA_DISCORD_GATEWAY_MAX_URL) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the gateway url is longer than %d bytes", NYA_DISCORD_GATEWAY_MAX_URL - 1);

    u64 ceiling = options.max_message_bytes > 0 ? options.max_message_bytes : (u64)NYA_DISCORD_GATEWAY_MAX_MESSAGE_BYTES;

    // Below this the connection would open and then fail on the first payload, a worse place to find out.
    if (ceiling < 4096) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a payload ceiling of %llu is too small for a READY", (unsigned long long)ceiling);

    NYA_DiscordGateway* gateway = nya_arena_alloc(arena, sizeof(NYA_DiscordGateway));
    *gateway = (NYA_DiscordGateway){
        .allocator              = arena,
        .payloads               = nya_arena_create(.name = "discord_gateway_payloads"),
        .state                  = NYA_DISCORD_GATEWAY_STATE_IDLE,
        .transport              = options.transport,
        .intents                = options.intents,
        .shard_id               = options.shard_id,
        .shard_count            = options.shard_count,
        .max_reconnect_attempts = options.max_reconnect_attempts,
        .sequence               = -1,
    };

    _nya_discord_copy(gateway->token, sizeof(gateway->token), options.token);
    _nya_discord_copy(gateway->url, sizeof(gateway->url), url);

    if (gateway->transport.open == nullptr) {
        // Its own arena: the websocket frees its buffers at destroy, and a reconnect every minute stays flat only if nothing else allocated between.
        gateway->sockets = nya_arena_create(.name = "discord_gateway_sockets");

        _NYA_DiscordGatewaySocket* owned = nya_arena_alloc(gateway->sockets, sizeof(_NYA_DiscordGatewaySocket));
        *owned = (_NYA_DiscordGatewaySocket){
            .arena                    = gateway->sockets,
            .max_message_bytes        = ceiling,
            .insecure_skip_tls_verify = options.insecure_skip_tls_verify,
        };

        gateway->owned_socket = owned;
        gateway->transport    = (NYA_DiscordGatewayTransport){
               .user   = owned,
               .open   = _nya_discord_socket_open,
               .close  = _nya_discord_socket_close,
               .poll   = _nya_discord_socket_poll,
               .send   = _nya_discord_socket_send,
               .now_ms = _nya_discord_socket_now_ms,
               .jitter = _nya_discord_socket_jitter,
        };
    }

    nya_assert(gateway->transport.open != nullptr, "a transport brings all six or none");
    nya_assert(gateway->transport.close != nullptr);
    nya_assert(gateway->transport.poll != nullptr);
    nya_assert(gateway->transport.send != nullptr);
    nya_assert(gateway->transport.now_ms != nullptr);
    nya_assert(gateway->transport.jitter != nullptr);

    NYA_Error dialled = _nya_discord_gateway_dial(gateway);
    if (!dialled.ok) {
        // Not fatal: an unreachable gateway is ordinary just after boot, and the caller finds out through a DISCONNECTED event with a retry time.
        gateway->attempt         = 1;
        gateway->reconnect_at_ms = gateway->transport.now_ms(gateway->transport.user) + nya_discord_gateway_backoff_ms(0);
    }

    *out_gateway = gateway;

    return NYA_OK;
}

void nya_discord_gateway_destroy(NYA_DiscordGateway* gateway) {
    if (gateway == nullptr) return;

    gateway->transport.close(gateway->transport.user);

    // The two places a token sits in this struct; neither survives the call. See the header for what is still out there afterwards.
    nya_crypto_wipe(gateway->token, sizeof(gateway->token));
    nya_crypto_wipe(gateway->login, sizeof(gateway->login));

    nya_arena_destroy(gateway->payloads);
    if (gateway->sockets != nullptr) nya_arena_destroy(gateway->sockets);

    nya_arena_free(gateway->allocator, gateway, sizeof(NYA_DiscordGateway));
}

// ───────────────────────────────────── OPERATIONS ─────────────────────────────────────

NYA_DiscordGatewayState nya_discord_gateway_state(const NYA_DiscordGateway* gateway) {
    nya_assert(gateway != nullptr);

    return gateway->state;
}

s64 nya_discord_gateway_sequence(const NYA_DiscordGateway* gateway) {
    nya_assert(gateway != nullptr);

    return gateway->sequence;
}

NYA_Error nya_discord_gateway_send(NYA_DiscordGateway* gateway, NYA_Arena* arena, const NYA_Object* payload) {
    nya_assert(gateway != nullptr);
    nya_assert(arena != nullptr);
    nya_assert(payload != nullptr);

    if (gateway->state != NYA_DISCORD_GATEWAY_STATE_READY) {
        return nya_error(NYA_ERROR_IO, "the gateway is %s, not ready", nya_discord_gateway_state_name(gateway->state));
    }

    NYA_String* text = nya_serde_json_serialize(arena, payload, NYA_SERDE_NONE);

    return gateway->transport.send(gateway->transport.user, nya_string_to_cstring(arena, text), text->length);
}

b8 nya_discord_gateway_poll(NYA_DiscordGateway* gateway, OUT NYA_DiscordGatewayEvent* out_event) {
    nya_assert(gateway != nullptr);
    nya_assert(out_event != nullptr);

    *out_event = (NYA_DiscordGatewayEvent){ .name = "", .reason = "" };

    if (gateway->state == NYA_DISCORD_GATEWAY_STATE_FATAL) return false;

    // Last poll's payload dies here, which is what the "valid until the next poll" in the header means.
    nya_arena_free_all(gateway->payloads);

    u64 now_ms = gateway->transport.now_ms(gateway->transport.user);

    if (gateway->state == NYA_DISCORD_GATEWAY_STATE_IDLE) {
        if (now_ms < gateway->reconnect_at_ms) return false;

        NYA_Error dialled = _nya_discord_gateway_dial(gateway);
        if (!dialled.ok) {
            _nya_discord_gateway_drop(gateway, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the gateway could not be dialled", now_ms, out_event);
            return true;
        }

        return false;
    }

    for (u32 step = 0; step < NYA_DISCORD_GATEWAY_MAX_STEPS_PER_POLL; step++) {
        // Before the heartbeat, so a HELLO whose jittered first beat landed this millisecond still logs in first; does nothing while the IDENTIFY rate limit holds.
        if (gateway->pending_login) _nya_discord_gateway_login(gateway, now_ms);

        // The heartbeat is on a clock, not an event, so it is checked before anything is read: a quiet gateway is exactly where no event arrives to hang it off.
        if (gateway->heartbeat_interval_ms > 0 && now_ms >= gateway->heartbeat_at_ms) {
            if (gateway->awaiting_ack) {
                // The socket is open and the far end is not there; waiting longer only loses more events.
                _nya_discord_gateway_drop(gateway, _NYA_DISCORD_CLOSE_RESUMABLE, "the gateway stopped acknowledging heartbeats", now_ms, out_event);
                return true;
            }

            NYA_Error beat = _nya_discord_gateway_heartbeat(gateway, now_ms);
            if (!beat.ok) {
                _nya_discord_gateway_drop(gateway, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the heartbeat could not be sent", now_ms, out_event);
                return true;
            }
        }

        NYA_WebSocketEvent frame = { 0 };
        if (!gateway->transport.poll(gateway->transport.user, &frame)) return false;

        switch (frame.kind) {
            case NYA_WEBSOCKET_EVENT_TEXT:
                if (_nya_discord_gateway_payload(gateway, frame.data, frame.size, now_ms, out_event)) return true;
                break;

            case NYA_WEBSOCKET_EVENT_CLOSED:
                _nya_discord_gateway_drop(gateway, (u16)frame.code, frame.reason, now_ms, out_event);
                return true;

            // OPEN says the socket is up, not logged in (HELLO decides that); a binary payload is `etf` to a json client, and a pong is answered by the protocol.
            case NYA_WEBSOCKET_EVENT_OPEN:
            case NYA_WEBSOCKET_EVENT_BINARY:
            case NYA_WEBSOCKET_EVENT_PONG:
            case NYA_WEBSOCKET_EVENT_NONE:
            case NYA_WEBSOCKET_EVENT_KIND_COUNT:
            default:                          break;
        }
    }

    return false;
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

b8 _nya_discord_token_is_plain(NYA_ConstCString token, u64 length) {
    for (u64 i = 0; i < length; i++) {
        u8 character = (u8)token[i];

        // Printable ASCII, no quote or backslash: a real token is base64url and dots, so anything needing escaping into the IDENTIFY is trying to be a payload.
        if (character < 0x21 || character > 0x7E) return false;
        if (character == '"' || character == '\\') return false;
    }

    return true;
}

void _nya_discord_copy(char* destination, u64 capacity, NYA_ConstCString text) {
    nya_assert(destination != nullptr);
    nya_assert(capacity > 0);

    destination[0] = '\0';
    if (text == nullptr) return;

    u64 length = strlen(text);
    if (length > capacity - 1) length = capacity - 1;

    nya_memcpy(destination, text, length);
    destination[length] = '\0';
}

NYA_ConstCString _nya_discord_string_at(const NYA_Object* object, NYA_CString key) {
    if (object == nullptr) return nullptr;

    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr || value->type != NYA_TYPE_STRING) return nullptr;

    return value->as_string;
}

s64 _nya_discord_integer_at(const NYA_Object* object, NYA_CString key, s64 fallback) {
    if (object == nullptr) return fallback;

    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr) return fallback;

    // The json reader gives S64 or F64, and Discord sends heartbeat_interval as either, so both count as numbers here.
    if (value->type == NYA_TYPE_S64) return value->as_s64;
    if (value->type == NYA_TYPE_F64) return (s64)value->as_f64;

    return fallback;
}

// ───────────────────────────────────── THE CONNECTION ─────────────────────────────────────

NYA_Error _nya_discord_gateway_dial(NYA_DiscordGateway* gateway) {
    b8 resuming = gateway->resume_wanted && gateway->resume_url[0] != '\0';

    NYA_ConstCString url = resuming ? gateway->resume_url : gateway->url;

    gateway->heartbeat_interval_ms = 0;
    gateway->awaiting_ack          = false;
    gateway->pending_login         = false;

    NYA_TRY(gateway->transport.open(gateway->transport.user, url));

    gateway->state = NYA_DISCORD_GATEWAY_STATE_CONNECTING;

    return NYA_OK;
}

void _nya_discord_gateway_drop(NYA_DiscordGateway* gateway, u16 code, NYA_ConstCString reason, u64 now_ms, OUT NYA_DiscordGatewayEvent* out_event) {
    // Copied before the close, since a websocket's reason points into the socket that is about to go.
    _nya_discord_copy(gateway->reason, sizeof(gateway->reason), reason != nullptr ? reason : "");

    gateway->transport.close(gateway->transport.user);

    gateway->heartbeat_interval_ms = 0;
    gateway->awaiting_ack          = false;
    gateway->pending_login         = false;

    NYA_DiscordGatewayCloseAction action = nya_discord_gateway_close_action(code);

    if (action == NYA_DISCORD_GATEWAY_CLOSE_REIDENTIFY) {
        gateway->session[0]    = '\0';
        gateway->resume_url[0] = '\0';
        gateway->sequence      = -1;
    }

    gateway->resume_wanted = action == NYA_DISCORD_GATEWAY_CLOSE_RESUME && gateway->session[0] != '\0' && gateway->sequence >= 0;

    gateway->attempt += 1;

    b8 give_up = action == NYA_DISCORD_GATEWAY_CLOSE_FATAL ||
                 (gateway->max_reconnect_attempts > 0 && gateway->attempt > gateway->max_reconnect_attempts);

    if (give_up) {
        gateway->state = NYA_DISCORD_GATEWAY_STATE_FATAL;

        *out_event = (NYA_DiscordGatewayEvent){
            .kind   = NYA_DISCORD_GATEWAY_EVENT_FATAL,
            .name   = "",
            .code   = code,
            .reason = gateway->reason[0] != '\0' ? gateway->reason : "the gateway refused this client",
        };

        return;
    }

    // Half the delay plus a random half: full jitter could land the first retry immediately, which a just-dropped gateway does not need, so this keeps a floor and still spreads a fleet out.
    u64 delay    = nya_discord_gateway_backoff_ms(gateway->attempt - 1);
    u64 half     = delay / 2;
    u64 jittered = half + (u64)((f32)half * gateway->transport.jitter(gateway->transport.user));

    gateway->reconnect_at_ms = now_ms + jittered;
    gateway->state           = NYA_DISCORD_GATEWAY_STATE_IDLE;

    *out_event = (NYA_DiscordGatewayEvent){
        .kind        = NYA_DISCORD_GATEWAY_EVENT_DISCONNECTED,
        .name        = "",
        .code        = code,
        .reason      = gateway->reason[0] != '\0' ? gateway->reason : "the connection ended",
        .retry_in_ms = jittered,
    };
}

// ───────────────────────────────────── THE LOGIN AND THE HEARTBEAT ─────────────────────────────────────

void _nya_discord_gateway_login(NYA_DiscordGateway* gateway, u64 now_ms) {
    if (gateway->resume_wanted) {
        (void)snprintf(gateway->login, sizeof(gateway->login),
                       "{\"op\":%d,\"d\":{\"token\":\"%s\",\"session_id\":\"%s\",\"seq\":" FMTs64 "}}",
                       _NYA_DISCORD_OP_RESUME, gateway->token, gateway->session, gateway->sequence);
    } else {
        // Discord counts IDENTIFYs, not connections, and one too many is a 4008 that repeats into a disabled token; waiting here costs a few seconds once.
        if (now_ms < gateway->identify_allowed_at_ms) return;

        char shard[64] = { 0 };
        if (gateway->shard_count > 0) (void)snprintf(shard, sizeof(shard), ",\"shard\":[%u,%u]", gateway->shard_id, gateway->shard_count);

        (void)snprintf(gateway->login, sizeof(gateway->login),
                       "{\"op\":%d,\"d\":{\"token\":\"%s\",\"intents\":%u,"
                       "\"properties\":{\"os\":\"" _NYA_DISCORD_OS_NAME "\",\"browser\":\"nyangine\",\"device\":\"nyangine\"}%s}}",
                       _NYA_DISCORD_OP_IDENTIFY, gateway->token, gateway->intents, shard);

        gateway->identify_allowed_at_ms = now_ms + NYA_DISCORD_GATEWAY_IDENTIFY_INTERVAL_MS;
    }

    NYA_Error sent = gateway->transport.send(gateway->transport.user, gateway->login, strlen(gateway->login));

    // Wiped whether or not it went out: a login that failed to queue is still a token in a buffer, and the next attempt rebuilds it from the one copy meant to exist.
    nya_crypto_wipe(gateway->login, sizeof(gateway->login));

    if (!sent.ok) {
        nya_log_warn("The Discord gateway login could not be queued: %s", (NYA_ConstCString)sent.message);
        return;
    }

    gateway->pending_login = false;
    gateway->state         = NYA_DISCORD_GATEWAY_STATE_IDENTIFYING;
}

NYA_Error _nya_discord_gateway_heartbeat(NYA_DiscordGateway* gateway, u64 now_ms) {
    char beat[64] = { 0 };

    if (gateway->sequence >= 0) {
        (void)snprintf(beat, sizeof(beat), "{\"op\":%d,\"d\":" FMTs64 "}", _NYA_DISCORD_OP_HEARTBEAT, gateway->sequence);
    } else {
        (void)snprintf(beat, sizeof(beat), "{\"op\":%d,\"d\":null}", _NYA_DISCORD_OP_HEARTBEAT);
    }

    NYA_TRY(gateway->transport.send(gateway->transport.user, beat, strlen(beat)));

    gateway->awaiting_ack   = true;
    gateway->heartbeat_at_ms = now_ms + gateway->heartbeat_interval_ms;

    return NYA_OK;
}

// ───────────────────────────────────── THE PAYLOADS ─────────────────────────────────────

b8 _nya_discord_gateway_payload(NYA_DiscordGateway* gateway, const u8* text, u64 size, u64 now_ms, OUT NYA_DiscordGatewayEvent* out_event) {
    NYA_Object* payload = nullptr;
    NYA_Error   parsed  = nya_serde_json_deserialize(gateway->payloads, text, size, NYA_SERDE_NONE, &payload);

    if (!parsed.ok || payload == nullptr) {
        // Ignored, not closed over: Discord does not send malformed json, so this is a proxy or truncation, and dropping a session over one frame loses more.
        nya_log_debug("A Discord gateway payload did not parse: %s", (NYA_ConstCString)parsed.message);
        return false;
    }

    // The sequence is on the envelope, not the dispatch, and is what a RESUME replays from, so it is taken before deciding what the payload was.
    s64 sequence = _nya_discord_integer_at(payload, "s", -1);
    if (sequence >= 0) gateway->sequence = sequence;

    s64 opcode = _nya_discord_integer_at(payload, "op", -1);

    NYA_Value*  data_value = nya_object_get(payload, "d");
    NYA_Object* data       = data_value != nullptr && data_value->type == NYA_TYPE_OBJECT ? &data_value->as_object : nullptr;

    switch (opcode) {
        case _NYA_DISCORD_OP_HELLO: {
            s64 interval = _nya_discord_integer_at(data, "heartbeat_interval", 0);

            if (interval <= 0) {
                _nya_discord_gateway_drop(gateway, _NYA_DISCORD_CLOSE_RESUMABLE, "the gateway sent a HELLO with no heartbeat interval", now_ms, out_event);
                return true;
            }

            gateway->heartbeat_interval_ms = (u64)interval;

            // The first beat goes at a random fraction of the interval (the docs ask for it): without it every bot reconnected by one gateway restart beats on the same millisecond forever.
            f32 fraction             = gateway->transport.jitter(gateway->transport.user);
            gateway->heartbeat_at_ms = now_ms + (u64)((f32)gateway->heartbeat_interval_ms * fraction);
            gateway->awaiting_ack    = false;

            gateway->resume_wanted = gateway->session[0] != '\0' && gateway->sequence >= 0;
            gateway->pending_login = true;

            return false;
        }

        case _NYA_DISCORD_OP_HEARTBEAT_ACK: gateway->awaiting_ack = false; return false;

        // The server asking for one out of band, which it does when it is about to go away.
        case _NYA_DISCORD_OP_HEARTBEAT: {
            NYA_Error beat = _nya_discord_gateway_heartbeat(gateway, now_ms);
            if (!beat.ok) {
                _nya_discord_gateway_drop(gateway, NYA_WEBSOCKET_CLOSE_ABNORMAL, "the requested heartbeat could not be sent", now_ms, out_event);
                return true;
            }

            return false;
        }

        case _NYA_DISCORD_OP_RECONNECT:
            _nya_discord_gateway_drop(gateway, _NYA_DISCORD_CLOSE_RESUMABLE, "the gateway asked for a reconnect", now_ms, out_event);
            return true;

        case _NYA_DISCORD_OP_INVALID_SESSION: {
            // `d` is a bare boolean here, not an object: true means the session can still be resumed and only this attempt was wrong.
            b8 resumable = data_value != nullptr && data_value->type == NYA_TYPE_B8 && data_value->as_b8;

            if (!resumable) {
                gateway->session[0]    = '\0';
                gateway->resume_url[0] = '\0';
                gateway->sequence      = -1;
            }

            gateway->transport.close(gateway->transport.user);

            gateway->heartbeat_interval_ms = 0;
            gateway->awaiting_ack          = false;
            gateway->pending_login         = false;
            gateway->resume_wanted         = resumable && gateway->session[0] != '\0';
            gateway->state                 = NYA_DISCORD_GATEWAY_STATE_IDLE;

            // One to five seconds (Discord's), not the backoff: an invalid session is an answer, not a failure, so the attempt counter is left alone.
            u64 spread   = _NYA_DISCORD_INVALID_SESSION_MAX_MS - _NYA_DISCORD_INVALID_SESSION_MIN_MS;
            u64 delay    = _NYA_DISCORD_INVALID_SESSION_MIN_MS + (u64)((f32)spread * gateway->transport.jitter(gateway->transport.user));
            gateway->reconnect_at_ms = now_ms + delay;

            *out_event = (NYA_DiscordGatewayEvent){
                .kind        = NYA_DISCORD_GATEWAY_EVENT_DISCONNECTED,
                .name        = "",
                .code        = _NYA_DISCORD_OP_INVALID_SESSION,
                .reason      = resumable ? "the gateway refused this resume" : "the gateway invalidated the session",
                .retry_in_ms = delay,
            };

            return true;
        }

        case _NYA_DISCORD_OP_DISPATCH: return _nya_discord_gateway_dispatch(gateway, payload, out_event);

        default:
            // Discord adds opcodes and a client is expected to live through them.
            nya_log_debug("The Discord gateway sent opcode " FMTs64 ", which this client does not handle", opcode);
            return false;
    }
}

b8 _nya_discord_gateway_dispatch(NYA_DiscordGateway* gateway, const NYA_Object* payload, OUT NYA_DiscordGatewayEvent* out_event) {
    NYA_ConstCString name = _nya_discord_string_at(payload, "t");
    if (name == nullptr) return false;

    NYA_Value*  data_value = nya_object_get(payload, "d");
    NYA_Object* data       = data_value != nullptr && data_value->type == NYA_TYPE_OBJECT ? &data_value->as_object : nullptr;

    _nya_discord_copy(gateway->event_name, sizeof(gateway->event_name), name);

    if (nya_string_equals(name, "READY")) {
        _nya_discord_copy(gateway->session, sizeof(gateway->session), _nya_discord_string_at(data, "session_id"));

        NYA_ConstCString resume_url = _nya_discord_string_at(data, "resume_gateway_url");
        if (resume_url != nullptr) {
            // Discord sends the host alone; the version and the encoding are the client's to ask for, and a
            // resume to a url without them is answered in a format this client cannot read.
            b8 has_query = nya_string_contains(resume_url, "?");

            char full[NYA_DISCORD_GATEWAY_MAX_URL] = { 0 };
            (void)snprintf(full, sizeof(full), "%s%s", resume_url, has_query ? "" : "/?v=10&encoding=json");

            _nya_discord_copy(gateway->resume_url, sizeof(gateway->resume_url), full);
        }

        gateway->state   = NYA_DISCORD_GATEWAY_STATE_READY;
        gateway->attempt = 0;

        NYA_Value*       user_value = data != nullptr ? nya_object_get(data, "user") : nullptr;
        NYA_ConstCString user_name  = user_value != nullptr && user_value->type == NYA_TYPE_OBJECT
                                        ? _nya_discord_string_at(&user_value->as_object, "username")
                                        : nullptr;

        _nya_discord_copy(gateway->event_name, sizeof(gateway->event_name), user_name != nullptr ? user_name : "");

        *out_event = (NYA_DiscordGatewayEvent){
            .kind   = NYA_DISCORD_GATEWAY_EVENT_READY,
            .name   = gateway->event_name,
            .data   = data,
            .reason = "",
        };

        return true;
    }

    if (nya_string_equals(name, "RESUMED")) {
        gateway->state   = NYA_DISCORD_GATEWAY_STATE_READY;
        gateway->attempt = 0;

        *out_event = (NYA_DiscordGatewayEvent){
            .kind   = NYA_DISCORD_GATEWAY_EVENT_RESUMED,
            .name   = gateway->event_name,
            .data   = data,
            .reason = "",
        };

        return true;
    }

    *out_event = (NYA_DiscordGatewayEvent){
        .kind   = NYA_DISCORD_GATEWAY_EVENT_DISPATCH,
        .name   = gateway->event_name,
        .data   = data,
        .reason = "",
    };

    return true;
}

// ───────────────────────────────────── THE DEFAULT TRANSPORT ─────────────────────────────────────

NYA_Error _nya_discord_socket_open(void* user, NYA_ConstCString url) {
    _NYA_DiscordGatewaySocket* self = (_NYA_DiscordGatewaySocket*)user;

    _nya_discord_socket_close(user);

    return nya_websocket_create(self->arena,
                                (NYA_WebSocketOptions){
                                    .url                      = url,
                                    .max_message_bytes        = self->max_message_bytes,
                                    .insecure_skip_tls_verify = self->insecure_skip_tls_verify,
                                },
                                &self->socket);
}

void _nya_discord_socket_close(void* user) {
    _NYA_DiscordGatewaySocket* self = (_NYA_DiscordGatewaySocket*)user;

    nya_websocket_destroy(self->socket);
    self->socket = nullptr;
}

b8 _nya_discord_socket_poll(void* user, OUT NYA_WebSocketEvent* out_event) {
    _NYA_DiscordGatewaySocket* self = (_NYA_DiscordGatewaySocket*)user;

    if (self->socket == nullptr) return false;

    return nya_websocket_poll(self->socket, out_event);
}

NYA_Error _nya_discord_socket_send(void* user, const char* text, u64 size) {
    _NYA_DiscordGatewaySocket* self = (_NYA_DiscordGatewaySocket*)user;

    // The size is the transport's contract; the websocket client takes a terminated string, which every caller here builds.
    (void)size;

    if (self->socket == nullptr) return nya_error(NYA_ERROR_IO, "there is no open gateway socket");

    return nya_websocket_send_text(self->socket, text);
}

u64 _nya_discord_socket_now_ms(void* user) {
    (void)user;

    // Monotonic, since every deadline here is a duration: a wall clock stepping back over a DST change would stop the heartbeat for an hour.
    return nya_clock_get_monotonic_ms();
}

f32 _nya_discord_socket_jitter(void* user) {
    (void)user;

    u32 bits = 0;
    if (!nya_os_random_bytes((u8*)&bits, sizeof(bits))) {
        // The jitter only spreads a fleet out, so half an interval is a fine answer when the system random source is not answering.
        return 0.5F;
    }

    return (f32)((f64)bits / ((f64)UINT32_MAX + 1.0));
}
