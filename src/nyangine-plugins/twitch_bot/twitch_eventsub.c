#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_clock.h"
#include "nyangine-std/base/base_clock_format.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-plugins/curl/websocket.h"
#include "nyangine-plugins/twitch_bot/twitch_eventsub.h"
#include "nyangine-std/serde/serde_json.h"

// ───────────────────────────────────── PRIVATE TYPES ─────────────────────────────────────

/** The two sockets a reconnect needs: the one being used, and the one being moved to. */
#define _NYA_TWITCH_SOCKET_LIVE 0
#define _NYA_TWITCH_SOCKET_NEXT 1
#define _NYA_TWITCH_SOCKET_COUNT 2

/** The real transport: a websocket per slot, and the engine's clocks. */
typedef struct {
    NYA_Arena*     arena;
    NYA_WebSocket* sockets[_NYA_TWITCH_SOCKET_COUNT];

    u64 max_message_bytes;
    b8  insecure_skip_tls_verify;
} _NYA_TwitchSockets;

struct NYA_TwitchEventSub {
    NYA_Arena* arena;

    /** Reset at the top of every poll: a message's parsed payload lives exactly until the next one. */
    NYA_Arena* messages;

    NYA_TwitchEventSubTransport transport;
    _NYA_TwitchSockets          sockets;

    NYA_TwitchEventSubState state;

    char url[NYA_TWITCH_EVENTSUB_MAX_URL];
    char reconnect_url[NYA_TWITCH_EVENTSUB_MAX_URL];
    char session[NYA_TWITCH_EVENTSUB_MAX_ID];

    /** Filled per message and pointed at by the one handed over, so nothing points into the arena. */
    char subscription_type[NYA_TWITCH_EVENTSUB_MAX_ID];
    char reason[256];

    /** How long Twitch said it would wait between keepalives, and when this last heard anything at all. */
    u64 keepalive_ms;
    u64 heard_at_ms;

    /** When the next socket may be opened, and how many endings led to it. */
    u64 retry_at_ms;
    u32 attempts;

    /** The ids this has already handed over, oldest overwritten first. See the header on replays. */
    char seen[NYA_TWITCH_EVENTSUB_SEEN_MAX][NYA_TWITCH_EVENTSUB_MAX_ID];
    u32  seen_next;
};

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/* The default transport: real websockets, the monotonic clock, and the wall clock. */
NYA_INTERNAL NYA_Error _nya_twitch_socket_open(void* user, u32 slot, NYA_ConstCString url) __attr_no_discard;
NYA_INTERNAL void      _nya_twitch_socket_close(void* user, u32 slot);
NYA_INTERNAL b8        _nya_twitch_socket_poll(void* user, u32 slot, OUT NYA_WebSocketEvent* out_event);
NYA_INTERNAL u64       _nya_twitch_now_ms(void* user) __attr_no_discard;
NYA_INTERNAL u64       _nya_twitch_now_s(void* user) __attr_no_discard;

/** Copies `text` into `destination`, truncating rather than running over. */
NYA_INTERNAL void _nya_twitch_copy(char* destination, u64 capacity, NYA_ConstCString text);

/** A string field of a JSON object, or null where it is absent or another type. */
NYA_INTERNAL NYA_ConstCString _nya_twitch_string_at(const NYA_Object* object, NYA_CString key) __attr_no_discard;

/** A number field of a JSON object, or `fallback`. */
NYA_INTERNAL s64 _nya_twitch_integer_at(const NYA_Object* object, NYA_CString key, s64 fallback) __attr_no_discard;

/** An object field of a JSON object, or null. */
NYA_INTERNAL const NYA_Object* _nya_twitch_object_at(const NYA_Object* object, NYA_CString key) __attr_no_discard;

/** Opens the live socket on `url` and puts the client in CONNECTING, or stops it when that cannot be done. */
NYA_INTERNAL void _nya_twitch_connect(NYA_TwitchEventSub* events, NYA_ConstCString url, OUT NYA_TwitchEventSubMessage* out_message, OUT b8* out_reported);

/** Ends the live socket, arms the backoff and fills `out_message` with the disconnection. */
NYA_INTERNAL void _nya_twitch_disconnect(NYA_TwitchEventSub* events, NYA_ConstCString reason, OUT NYA_TwitchEventSubMessage* out_message);

/** Whether this message id has already been handed over, remembering it when it has not. */
NYA_INTERNAL b8 _nya_twitch_seen(NYA_TwitchEventSub* events, NYA_ConstCString id) __attr_no_discard;

/** Reads one text frame from a slot into a message. False when it is not one a caller should see. */
NYA_INTERNAL b8 _nya_twitch_message(NYA_TwitchEventSub* events, u32 slot, const u8* text, u64 size, OUT NYA_TwitchEventSubMessage* out_message);

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_Error nya_twitch_eventsub_create(NYA_Arena* arena, NYA_TwitchEventSubOptions options, NYA_TwitchEventSub** out_events) {
    nya_assert(arena != nullptr && out_events != nullptr);

    *out_events = nullptr;

    NYA_ConstCString url = options.url != nullptr ? options.url : NYA_TWITCH_EVENTSUB_URL;
    if (url[0] == '\0' || strlen(url) >= NYA_TWITCH_EVENTSUB_MAX_URL) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is not a url to open");

    NYA_TwitchEventSub* events = nya_arena_alloc(arena, sizeof(NYA_TwitchEventSub));
    nya_memset(events, 0, sizeof(NYA_TwitchEventSub));

    events->arena    = arena;
    events->messages = nya_arena_create(.name = "twitch_eventsub_messages");

    _nya_twitch_copy(events->url, sizeof(events->url), url);

    events->keepalive_ms = (u64)NYA_TWITCH_EVENTSUB_KEEPALIVE_S * 1000;

    events->sockets = (_NYA_TwitchSockets){
        .arena                    = arena,
        .max_message_bytes        = options.max_message_bytes,
        .insecure_skip_tls_verify = options.insecure_skip_tls_verify,
    };

    // A caller's transport is taken whole or not at all: half of one is a real socket read against a fake clock, which nothing means.
    b8 supplied = options.transport.open != nullptr || options.transport.close != nullptr || options.transport.poll != nullptr;

    if (supplied) {
        if (options.transport.open == nullptr || options.transport.close == nullptr || options.transport.poll == nullptr ||
            options.transport.now_ms == nullptr || options.transport.now_s == nullptr) {
            nya_arena_destroy(events->messages);
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a transport needs open, close, poll, now_ms and now_s");
        }

        events->transport = options.transport;
    } else {
        events->transport = (NYA_TwitchEventSubTransport){
            .user   = &events->sockets,
            .open   = _nya_twitch_socket_open,
            .close  = _nya_twitch_socket_close,
            .poll   = _nya_twitch_socket_poll,
            .now_ms = _nya_twitch_now_ms,
            .now_s  = _nya_twitch_now_s,
        };
    }

    *out_events = events;

    return NYA_OK;
}

void nya_twitch_eventsub_destroy(NYA_TwitchEventSub* events) {
    if (events == nullptr) return;

    for (u32 slot = 0; slot < _NYA_TWITCH_SOCKET_COUNT; slot++) events->transport.close(events->transport.user, slot);

    nya_arena_destroy(events->messages);
}

b8 nya_twitch_eventsub_poll(NYA_TwitchEventSub* events, NYA_TwitchEventSubMessage* out_message) {
    nya_assert(events != nullptr && out_message != nullptr);

    *out_message = (NYA_TwitchEventSubMessage){ .session = "", .subscription_type = "", .reason = "" };

    if (events->state == NYA_TWITCH_EVENTSUB_STATE_STOPPED) return false;

    u64 now_ms = events->transport.now_ms(events->transport.user);

    // The payload handed over last time; nobody may read it after this point, which is what the header promises.
    nya_arena_free_all(events->messages);

    if (events->state == NYA_TWITCH_EVENTSUB_STATE_IDLE || events->state == NYA_TWITCH_EVENTSUB_STATE_WAITING) {
        if (events->state == NYA_TWITCH_EVENTSUB_STATE_WAITING && now_ms < events->retry_at_ms) return false;

        b8 reported = false;
        _nya_twitch_connect(events, events->url, out_message, &reported);

        return reported;
    }

    // The socket being moved to, first: it carries the welcome that ends the move, and until then the live one still delivers.
    if (events->state == NYA_TWITCH_EVENTSUB_STATE_RECONNECTING) {
        NYA_WebSocketEvent event = { 0 };

        while (events->transport.poll(events->transport.user, _NYA_TWITCH_SOCKET_NEXT, &event)) {
            if (event.kind == NYA_WEBSOCKET_EVENT_TEXT && _nya_twitch_message(events, _NYA_TWITCH_SOCKET_NEXT, event.data, event.size, out_message)) {
                return true;
            }

            if (event.kind == NYA_WEBSOCKET_EVENT_CLOSED) {
                // The move failed but the old socket is still good, so this is a reconnect that did not happen, not a disconnection; Twitch will ask again.
                nya_log_warn("The twitch reconnect socket closed before it was welcomed; staying on the old one.");

                events->transport.close(events->transport.user, _NYA_TWITCH_SOCKET_NEXT);
                events->state = NYA_TWITCH_EVENTSUB_STATE_READY;
                break;
            }
        }
    }

    NYA_WebSocketEvent event = { 0 };

    while (events->transport.poll(events->transport.user, _NYA_TWITCH_SOCKET_LIVE, &event)) {
        switch (event.kind) {
            case NYA_WEBSOCKET_EVENT_OPEN: {
                events->state       = NYA_TWITCH_EVENTSUB_STATE_OPENED;
                events->heard_at_ms = now_ms;
                break;
            }

            case NYA_WEBSOCKET_EVENT_TEXT: {
                events->heard_at_ms = now_ms;

                if (_nya_twitch_message(events, _NYA_TWITCH_SOCKET_LIVE, event.data, event.size, out_message)) return true;
                break;
            }

            case NYA_WEBSOCKET_EVENT_CLOSED: {
                _nya_twitch_disconnect(events, event.reason != nullptr ? event.reason : "the socket closed", out_message);
                return true;
            }

            case NYA_WEBSOCKET_EVENT_BINARY:
            case NYA_WEBSOCKET_EVENT_PONG:
            case NYA_WEBSOCKET_EVENT_NONE:
            default: break;
        }
    }

    // Silence past the promised keepalive is how this socket dies in practice (up but delivering nothing); dropped rather than waited on, since it is indistinguishable from broken.
    if (events->heard_at_ms != 0 && now_ms - events->heard_at_ms > events->keepalive_ms + NYA_TWITCH_EVENTSUB_GRACE_MS) {
        _nya_twitch_disconnect(events, "twitch stopped sending keepalives", out_message);
        return true;
    }

    return false;
}

NYA_ConstCString nya_twitch_eventsub_session(const NYA_TwitchEventSub* events) {
    nya_assert(events != nullptr);

    return events->state == NYA_TWITCH_EVENTSUB_STATE_READY || events->state == NYA_TWITCH_EVENTSUB_STATE_RECONNECTING ? events->session : "";
}

NYA_TwitchEventSubState nya_twitch_eventsub_state(const NYA_TwitchEventSub* events) {
    nya_assert(events != nullptr);

    return events->state;
}

NYA_ConstCString nya_twitch_eventsub_state_name(NYA_TwitchEventSubState state) {
    switch (state) {
        case NYA_TWITCH_EVENTSUB_STATE_IDLE:         return "idle";
        case NYA_TWITCH_EVENTSUB_STATE_CONNECTING:   return "connecting";
        case NYA_TWITCH_EVENTSUB_STATE_OPENED:       return "opened";
        case NYA_TWITCH_EVENTSUB_STATE_READY:        return "ready";
        case NYA_TWITCH_EVENTSUB_STATE_RECONNECTING: return "reconnecting";
        case NYA_TWITCH_EVENTSUB_STATE_WAITING:      return "waiting";
        case NYA_TWITCH_EVENTSUB_STATE_STOPPED:      return "stopped";

        case NYA_TWITCH_EVENTSUB_STATE_COUNT:
        default:                                     return "unknown";
    }
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

NYA_Error _nya_twitch_socket_open(void* user, u32 slot, NYA_ConstCString url) {
    _NYA_TwitchSockets* sockets = (_NYA_TwitchSockets*)user;

    nya_assert(slot < _NYA_TWITCH_SOCKET_COUNT);

    if (sockets->sockets[slot] != nullptr) {
        nya_websocket_destroy(sockets->sockets[slot]);
        sockets->sockets[slot] = nullptr;
    }

    return nya_websocket_create(sockets->arena,
                                (NYA_WebSocketOptions){
                                    .url                      = url,
                                    .max_message_bytes        = sockets->max_message_bytes,
                                    .insecure_skip_tls_verify = sockets->insecure_skip_tls_verify,
                                },
                                &sockets->sockets[slot]);
}

void _nya_twitch_socket_close(void* user, u32 slot) {
    _NYA_TwitchSockets* sockets = (_NYA_TwitchSockets*)user;

    nya_assert(slot < _NYA_TWITCH_SOCKET_COUNT);

    if (sockets->sockets[slot] == nullptr) return;

    nya_websocket_destroy(sockets->sockets[slot]);
    sockets->sockets[slot] = nullptr;
}

b8 _nya_twitch_socket_poll(void* user, u32 slot, NYA_WebSocketEvent* out_event) {
    _NYA_TwitchSockets* sockets = (_NYA_TwitchSockets*)user;

    nya_assert(slot < _NYA_TWITCH_SOCKET_COUNT);

    if (sockets->sockets[slot] == nullptr) return false;

    return nya_websocket_poll(sockets->sockets[slot], out_event);
}

u64 _nya_twitch_now_ms(void* user) {
    (void)user;

    return nya_clock_get_monotonic_ms();
}

u64 _nya_twitch_now_s(void* user) {
    (void)user;

    return nya_clock_get_timestamp_s();
}

void _nya_twitch_copy(char* destination, u64 capacity, NYA_ConstCString text) {
    nya_assert(destination != nullptr && capacity > 0);

    destination[0] = '\0';

    if (text == nullptr) return;

    u64 length = strlen(text);
    if (length > capacity - 1) length = capacity - 1;

    nya_memcpy(destination, text, length);
    destination[length] = '\0';
}

NYA_ConstCString _nya_twitch_string_at(const NYA_Object* object, NYA_CString key) {
    if (object == nullptr) return nullptr;

    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr || value->type != NYA_TYPE_STRING) return nullptr;

    return value->as_string;
}

s64 _nya_twitch_integer_at(const NYA_Object* object, NYA_CString key, s64 fallback) {
    if (object == nullptr) return fallback;

    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr) return fallback;

    if (value->type == NYA_TYPE_S64) return value->as_s64;
    if (value->type == NYA_TYPE_F64) return (s64)value->as_f64;

    return fallback;
}

const NYA_Object* _nya_twitch_object_at(const NYA_Object* object, NYA_CString key) {
    if (object == nullptr) return nullptr;

    NYA_Value* value = nya_object_get(object, key);
    if (value == nullptr || value->type != NYA_TYPE_OBJECT) return nullptr;

    return &value->as_object;
}

void _nya_twitch_connect(NYA_TwitchEventSub* events, NYA_ConstCString url, NYA_TwitchEventSubMessage* out_message, b8* out_reported) {
    *out_reported = false;

    NYA_Error opened = events->transport.open(events->transport.user, _NYA_TWITCH_SOCKET_LIVE, url);

    if (!opened.ok) {
        // A url that will not open will not open next time either: an unresolvable host or a non-websocket scheme is a program's mistake, not a network's.
        _nya_twitch_copy(events->reason, sizeof(events->reason), (NYA_ConstCString)opened.message);

        events->state = NYA_TWITCH_EVENTSUB_STATE_STOPPED;

        *out_message  = (NYA_TwitchEventSubMessage){ .kind = NYA_TWITCH_EVENTSUB_FATAL, .session = "", .subscription_type = "", .reason = events->reason };
        *out_reported = true;

        return;
    }

    events->state       = NYA_TWITCH_EVENTSUB_STATE_CONNECTING;
    events->heard_at_ms = events->transport.now_ms(events->transport.user);
    events->session[0]  = '\0';
}

void _nya_twitch_disconnect(NYA_TwitchEventSub* events, NYA_ConstCString reason, NYA_TwitchEventSubMessage* out_message) {
    events->transport.close(events->transport.user, _NYA_TWITCH_SOCKET_LIVE);
    events->transport.close(events->transport.user, _NYA_TWITCH_SOCKET_NEXT);

    events->session[0]  = '\0';
    events->heard_at_ms = 0;

    u64 now_ms = events->transport.now_ms(events->transport.user);

    // Doubled per ending and capped, so an outage is not a bot hammering it and a single dropped socket is back inside a second.
    u64 wait_ms = (u64)NYA_TWITCH_EVENTSUB_BACKOFF_MS << nya_min(events->attempts, 6U);
    if (wait_ms > NYA_TWITCH_EVENTSUB_BACKOFF_MAX_MS) wait_ms = NYA_TWITCH_EVENTSUB_BACKOFF_MAX_MS;

    events->attempts   += 1;
    events->retry_at_ms = now_ms + wait_ms;
    events->state       = NYA_TWITCH_EVENTSUB_STATE_WAITING;

    _nya_twitch_copy(events->reason, sizeof(events->reason), reason);

    *out_message = (NYA_TwitchEventSubMessage){
        .kind              = NYA_TWITCH_EVENTSUB_DISCONNECTED,
        .session           = "",
        .subscription_type = "",
        .reason            = events->reason,
        .retry_in_ms       = wait_ms,
    };
}

b8 _nya_twitch_seen(NYA_TwitchEventSub* events, NYA_ConstCString id) {
    if (id == nullptr || id[0] == '\0') return false;

    for (u32 i = 0; i < NYA_TWITCH_EVENTSUB_SEEN_MAX; i++) {
        if (nya_string_equals((NYA_ConstCString)events->seen[i], id)) return true;
    }

    _nya_twitch_copy(events->seen[events->seen_next], NYA_TWITCH_EVENTSUB_MAX_ID, id);

    events->seen_next = (events->seen_next + 1) % NYA_TWITCH_EVENTSUB_SEEN_MAX;

    return false;
}

b8 _nya_twitch_message(NYA_TwitchEventSub* events, u32 slot, const u8* text, u64 size, NYA_TwitchEventSubMessage* out_message) {
    NYA_Object* payload = nullptr;

    NYA_Error parsed = nya_serde_json_deserialize(events->messages, text, size, NYA_SERDE_NONE, &payload);

    if (!parsed.ok || payload == nullptr) {
        nya_log_warn("A twitch message was not json and was dropped: %s", (NYA_ConstCString)parsed.message);
        return false;
    }

    const NYA_Object* metadata = _nya_twitch_object_at(payload, "metadata");
    const NYA_Object* body     = _nya_twitch_object_at(payload, "payload");

    NYA_ConstCString type = _nya_twitch_string_at(metadata, "message_type");
    if (type == nullptr) return false;

    // The replay rules, before anything acts: an id may arrive twice, and nothing older than ten minutes counts; the timestamp catches what the ring cannot, a capture replayed after its ids rolled out.
    NYA_ConstCString stamp = _nya_twitch_string_at(metadata, "message_timestamp");

    if (stamp != nullptr) {
        NYA_Instant instant  = { 0 };
        u64         position = 0;

        if (nya_instant_from_rfc3339((const u8*)stamp, strlen(stamp), &instant, &position) == NYA_TIME_PARSE_OK && instant.ns >= 0) {
            u64 sent_s = (u64)(instant.ns / NYA_NS_PER_SECOND);
            u64 now_s  = events->transport.now_s(events->transport.user);

            if (now_s > sent_s && now_s - sent_s > NYA_TWITCH_EVENTSUB_REPLAY_S) {
                nya_log_warn("A twitch message older than the replay window was dropped.");
                return false;
            }
        }
    }

    if (_nya_twitch_seen(events, _nya_twitch_string_at(metadata, "message_id"))) return false;

    if (nya_string_equals(type, "session_welcome")) {
        const NYA_Object* session = _nya_twitch_object_at(body, "session");

        _nya_twitch_copy(events->session, sizeof(events->session), _nya_twitch_string_at(session, "id"));

        s64 keepalive_s = _nya_twitch_integer_at(session, "keepalive_timeout_seconds", NYA_TWITCH_EVENTSUB_KEEPALIVE_S);
        if (keepalive_s <= 0) keepalive_s = NYA_TWITCH_EVENTSUB_KEEPALIVE_S;

        events->keepalive_ms = (u64)keepalive_s * 1000;
        events->attempts     = 0;

        // The move is over: the new socket has a session, so the old one has nothing left to deliver.
        if (slot == _NYA_TWITCH_SOCKET_NEXT) {
            events->transport.close(events->transport.user, _NYA_TWITCH_SOCKET_LIVE);
            events->sockets.sockets[_NYA_TWITCH_SOCKET_LIVE] = events->sockets.sockets[_NYA_TWITCH_SOCKET_NEXT];
            events->sockets.sockets[_NYA_TWITCH_SOCKET_NEXT] = nullptr;
        }

        events->state = NYA_TWITCH_EVENTSUB_STATE_READY;

        *out_message = (NYA_TwitchEventSubMessage){
            .kind              = NYA_TWITCH_EVENTSUB_WELCOME,
            .session           = events->session,
            .subscription_type = "",
            .reason            = "",
        };

        return true;
    }

    // A heartbeat with a body, not handed over: a caller ignoring these would write the same empty case in every bot.
    if (nya_string_equals(type, "session_keepalive")) return false;

    if (nya_string_equals(type, "session_reconnect")) {
        const NYA_Object* session = _nya_twitch_object_at(body, "session");
        NYA_ConstCString  url     = _nya_twitch_string_at(session, "reconnect_url");

        if (url == nullptr || url[0] == '\0') {
            nya_log_warn("Twitch asked for a reconnect without a url; staying where we are.");
            return false;
        }

        _nya_twitch_copy(events->reconnect_url, sizeof(events->reconnect_url), url);

        NYA_Error opened = events->transport.open(events->transport.user, _NYA_TWITCH_SOCKET_NEXT, events->reconnect_url);

        if (!opened.ok) {
            // The old socket has ~30 seconds left and still works, so this is not fatal: it ends on its own and the ordinary backoff opens a fresh one.
            nya_log_warn("The twitch reconnect url would not open (%s); the old socket has a little longer.",
                         (NYA_ConstCString)opened.message);
            return false;
        }

        events->state = NYA_TWITCH_EVENTSUB_STATE_RECONNECTING;

        return false;
    }

    if (nya_string_equals(type, "notification") || nya_string_equals(type, "revocation")) {
        NYA_ConstCString subscription_type = _nya_twitch_string_at(metadata, "subscription_type");

        // Twitch puts it in a notification's metadata and a revocation's payload, so both are read rather than one assumed.
        if (subscription_type == nullptr) subscription_type = _nya_twitch_string_at(_nya_twitch_object_at(body, "subscription"), "type");

        _nya_twitch_copy(events->subscription_type, sizeof(events->subscription_type), subscription_type);

        b8 revoked = nya_string_equals(type, "revocation");

        *out_message = (NYA_TwitchEventSubMessage){
            .kind              = revoked ? NYA_TWITCH_EVENTSUB_REVOKED : NYA_TWITCH_EVENTSUB_NOTIFICATION,
            .session           = "",
            .subscription_type = events->subscription_type,
            .event             = revoked ? nullptr : _nya_twitch_object_at(body, "event"),
            .reason            = "",
        };

        return true;
    }

    // A message type this build has never heard of, logged at debug and stepped over: Twitch adds them, and refusing one would stop at the first unrecognized thing.
    nya_log_debug("A twitch message of type '%s' was ignored.", type);

    return false;
}
