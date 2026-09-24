#include "nyangine/nyangine.h"

#if OS_WINDOWS
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

// ───────────────────────────────────── THE PROTOCOL ─────────────────────────────────────

// Every message is an opcode and a length (u32 each, little endian) then the body, written as one buffer.

#define _NYA_DISCORD_OPCODE_HANDSHAKE 0
#define _NYA_DISCORD_OPCODE_FRAME     1
#define _NYA_DISCORD_OPCODE_CLOSE     2
#define _NYA_DISCORD_OPCODE_PING      3
#define _NYA_DISCORD_OPCODE_PONG      4

/** What the reference implementation caps a frame at, header included. Anything larger is a broken peer. */
#define _NYA_DISCORD_MAX_FRAME (64 * 1024)

/** Discord names its sockets discord-ipc-0 through discord-ipc-9. Every one is tried, in order. */
#define _NYA_DISCORD_MAX_SOCKETS 10

/** Discord accepts one presence update per fifteen seconds per client and silently drops the rest. */
#define _NYA_DISCORD_UPDATE_INTERVAL_MS 15000

/** How long after a failed connect before trying again, and the ceiling it doubles up to. */
#define _NYA_DISCORD_RETRY_MIN_MS 2000
#define _NYA_DISCORD_RETRY_MAX_MS 60000

/** How long to wait for the handshake reply before treating the connection as dead. */
#define _NYA_DISCORD_HANDSHAKE_TIMEOUT_MS 5000

// The two events worth subscribing to: the only things the client says on its own, and without a SUBSCRIBE it says neither.
#define _NYA_DISCORD_EVENT_ACTIVITY_JOIN         "ACTIVITY_JOIN"
#define _NYA_DISCORD_EVENT_ACTIVITY_JOIN_REQUEST "ACTIVITY_JOIN_REQUEST"

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

typedef struct {
    NYA_Arena* allocator;

    u64               application_id;
    NYA_DiscordStatus status;

    /** The socket, or the pipe on Windows. -1 / INVALID_HANDLE_VALUE when there is none. */
#if OS_WINDOWS
    HANDLE handle;
#else
    s32 handle;
#endif

    /** What was most recently asked for, and what was most recently sent. */
    NYA_DiscordActivity pending;
    NYA_DiscordActivity sent;
    b8                  has_pending;
    b8                  has_sent;

    /** Monotonic milliseconds. Zero means nothing sent yet, so the window is open. */
    u64 last_update_ms;

    /** Monotonic milliseconds at which another connect may be attempted, and the current backoff. */
    u64 next_retry_ms;
    u64 retry_delay_ms;

    /**
     * When to give up waiting for the handshake reply.
     * */
    u64 handshake_deadline_ms;

    /** Filled from the READY payload. Empty until the handshake completes. */
    char user_name[NYA_DISCORD_MAX_TEXT];

    /**
     * Inbound events, oldest first, drained by nya_discord_poll. A plain array rather than a ring: it
     * holds eight entries and is emptied every frame, so the shift on drain moves nothing worth a
     * second index.
     * */
    NYA_DiscordEvent events[NYA_DISCORD_MAX_EVENTS];
    u32              event_count;

    /** How many events were dropped because the queue was full. Logged once, not per drop. */
    u64 events_dropped;

    /** Partial frame carried between pumps. */
    u8  read_buffer[_NYA_DISCORD_MAX_FRAME];
    u32 read_length;
} _NYA_DiscordSystem;

/**
 * The one system, as a file scope static.
 * */
NYA_INTERNAL _NYA_DiscordSystem _NYA_DISCORD = { 0 };

/** Opens the first Discord socket that answers. False when none does, the ordinary case. */
NYA_INTERNAL b8 _nya_discord_connect(void);

/**
 * Drops the connection and arms the retry backoff.
 * */
NYA_INTERNAL void _nya_discord_disconnect(void);

/** Closes the handle and nothing else. The per platform half of _nya_discord_disconnect. */
NYA_INTERNAL void _nya_discord_close_handle(void);

/** Pushes the next connect attempt out, doubling the delay up to _NYA_DISCORD_RETRY_MAX_MS. */
NYA_INTERNAL void _nya_discord_arm_retry(void);

/** Writes one framed message. False on any error, which is taken as the connection being gone. */
NYA_INTERNAL b8 _nya_discord_write(u32 opcode, NYA_ConstCString payload, u32 payload_length);

/** Drains whatever is readable into the frame buffer and handles every complete frame in it. */
NYA_INTERNAL void _nya_discord_read(void);

/** Acts on one complete frame. */
NYA_INTERNAL void _nya_discord_handle_frame(u32 opcode, const u8* payload, u32 length);

/** Builds the SET_ACTIVITY payload for `activity`, or for a cleared presence when null. */
NYA_INTERNAL NYA_String* _nya_discord_activity_payload(NYA_Arena* arena, const NYA_DiscordActivity* activity) __attr_no_discard;

/** Appends `"key":"value",` with `value` escaped, or nothing at all when it is null or empty. */
NYA_INTERNAL void _nya_discord_append_string_field(NYA_String* out, NYA_ConstCString key, NYA_ConstCString value);

/** Appends a JSON string literal, quotes included, escaping what JSON requires. */
NYA_INTERNAL void _nya_discord_append_escaped(NYA_String* out, NYA_ConstCString value);

/** Whether two activities would produce the same payload. */
NYA_INTERNAL b8 _nya_discord_activity_equals(const NYA_DiscordActivity* a, const NYA_DiscordActivity* b) __attr_no_discard;

/** nya_string_equals, with null as a value. */
NYA_INTERNAL b8 _nya_discord_text_equals(NYA_ConstCString a, NYA_ConstCString b) __attr_no_discard;

/** Sends `pending` if the rate limit window is open. Called from the pump and after a connect. */
NYA_INTERNAL void _nya_discord_flush_activity(void);

/** The process id, which SET_ACTIVITY requires; Discord uses it to notice the game exit. */
NYA_INTERNAL s64 _nya_discord_process_id(void) __attr_no_discard;

/** Asks the client to start sending `event`. Without this the client reports nothing at all. */
NYA_INTERNAL void _nya_discord_subscribe(NYA_ConstCString event);

/** Sends one command whose only argument is a user id. SEND_ACTIVITY_JOIN_INVITE and CLOSE_ACTIVITY_REQUEST. */
NYA_INTERNAL NYA_Error _nya_discord_send_user_command(NYA_ConstCString command, NYA_ConstCString user_id) __attr_no_discard;

/** Appends an event, dropping the oldest when the queue is full. */
NYA_INTERNAL void _nya_discord_event_push(NYA_DiscordEvent event);

/** Turns one dispatched `evt` frame into a queued event. Does nothing for an event it does not know. */
NYA_INTERNAL void _nya_discord_handle_event(NYA_ConstCString event, const NYA_Object* data);

/**
 * Copies a string the client sent into a fixed buffer, or refuses it.
 * */
NYA_INTERNAL b8 _nya_discord_parse_text(const NYA_Value* value, OUT char* out, u64 capacity) __attr_no_discard;

/**
 * Copies a Discord user id, which is a snowflake written in decimal and nothing else.
 * */
NYA_INTERNAL b8 _nya_discord_parse_user_id(const NYA_Value* value, OUT char* out, u64 capacity) __attr_no_discard;

/** Whether `text` is one to twenty decimal digits. What a user id has to be before it is sent back. */
NYA_INTERNAL b8 _nya_discord_user_id_is_valid(NYA_ConstCString text) __attr_no_discard;

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

NYA_Error nya_discord_init(u64 application_id) {
    if (application_id == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Discord application id of zero");
    if (_NYA_DISCORD.status != NYA_DISCORD_STATUS_OFF) return nya_error(NYA_ERROR_NOT_OK, "the Discord plugin is already initialized");

    _NYA_DISCORD = (_NYA_DiscordSystem){
        .allocator      = nya_arena_create(.name = "discord_allocator"),
        .application_id = application_id,
        .status         = NYA_DISCORD_STATUS_DISCONNECTED,
        .retry_delay_ms = _NYA_DISCORD_RETRY_MIN_MS,
#if OS_WINDOWS
        .handle = INVALID_HANDLE_VALUE,
#else
        .handle = -1,
#endif
    };

    // not connecting here: Discord not running is ordinary, and init must not block or fail over it. the pump
    // connects.
    return NYA_OK;
}

void nya_discord_deinit(void) {
    if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_OFF) return;

    _nya_discord_disconnect();

    if (_NYA_DISCORD.allocator != nullptr) nya_arena_destroy(_NYA_DISCORD.allocator);

    _NYA_DISCORD = (_NYA_DiscordSystem){ 0 };
}

void nya_discord_pump(void) {
    if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_OFF) return;

    if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_DISCONNECTED) {
        u64 now_ms = nya_clock_get_monotonic_ms();
        if (now_ms < _NYA_DISCORD.next_retry_ms) return;

        // retrying every frame would fail connect() sixty times a second for everyone without Discord open.
        if (!_nya_discord_connect()) {
            // once, on the first attempt only: an optional dependency that is missing says so and then
            // stays quiet, rather than writing a line every two seconds for a player who has no Discord.
            if (_NYA_DISCORD.retry_delay_ms == _NYA_DISCORD_RETRY_MIN_MS && _NYA_DISCORD.next_retry_ms == 0) {
                nya_log_warn("No Discord client is running; rich presence and Discord invites are off. Start Discord before the game to enable them.");
            }

            _nya_discord_arm_retry();
            return;
        }

        /* The backoff is not reset on connect, only once the handshake succeeds. */
        _NYA_DISCORD.status                = NYA_DISCORD_STATUS_CONNECTING;
        _NYA_DISCORD.handshake_deadline_ms = now_ms + _NYA_DISCORD_HANDSHAKE_TIMEOUT_MS;

        NYA_Arena* scratch = nya_arena_create(.name = "discord_handshake");
        defer      nya_arena_destroy(scratch);

        // the client id is a string in the handshake; unquoted it is accepted and never answered.
        NYA_String* handshake = nya_string_sprintf(scratch, "{\"v\":1,\"client_id\":\"%llu\"}", (unsigned long long)_NYA_DISCORD.application_id);

        if (!_nya_discord_write(_NYA_DISCORD_OPCODE_HANDSHAKE, nya_string_to_cstring(scratch, handshake), (u32)handshake->length)) {
            _nya_discord_disconnect();
            return;
        }
    }

    _nya_discord_read();

    // accepted and never answered would leave CONNECTING forever, since only DISCONNECTED retries.
    if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_CONNECTING && nya_clock_get_monotonic_ms() > _NYA_DISCORD.handshake_deadline_ms) {
        nya_log_warn("Discord: no handshake reply within %d ms; dropping the connection.", _NYA_DISCORD_HANDSHAKE_TIMEOUT_MS);
        _nya_discord_disconnect();
        return;
    }

    if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_CONNECTED) _nya_discord_flush_activity();
}

NYA_DiscordStatus nya_discord_status(void) {
    return _NYA_DISCORD.status;
}

b8 nya_discord_connected(void) {
    return _NYA_DISCORD.status == NYA_DISCORD_STATUS_CONNECTED;
}

NYA_ConstCString nya_discord_user_name(void) {
    return _NYA_DISCORD.user_name[0] == '\0' ? nullptr : _NYA_DISCORD.user_name;
}

NYA_Error nya_discord_activity_set(NYA_DiscordActivity activity) {
    if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_OFF) return nya_error(NYA_ERROR_NOT_OK, "the Discord plugin is not initialized");

    // held, not sent: the pump owns the socket and the rate limit, and this works before Discord runs.
    _NYA_DISCORD.pending     = activity;
    _NYA_DISCORD.has_pending = true;

    return NYA_OK;
}

NYA_Error nya_discord_activity_clear(void) {
    if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_OFF) return nya_error(NYA_ERROR_NOT_OK, "the Discord plugin is not initialized");

    _NYA_DISCORD.pending     = (NYA_DiscordActivity){ 0 };
    _NYA_DISCORD.has_pending = false;

    // not through `pending`: "no activity" and "an empty activity" are different messages, and only one removes the
    // card.
    if (_NYA_DISCORD.status != NYA_DISCORD_STATUS_CONNECTED) return NYA_OK;

    NYA_Arena* scratch = nya_arena_create(.name = "discord_clear");
    defer      nya_arena_destroy(scratch);

    NYA_String* payload = _nya_discord_activity_payload(scratch, nullptr);

    if (!_nya_discord_write(_NYA_DISCORD_OPCODE_FRAME, nya_string_to_cstring(scratch, payload), (u32)payload->length)) {
        _nya_discord_disconnect();
        return nya_error(NYA_ERROR_NOT_OK, "the Discord connection closed while clearing presence");
    }

    _NYA_DISCORD.has_sent       = false;
    _NYA_DISCORD.last_update_ms = nya_clock_get_monotonic_ms();

    return NYA_OK;
}

// ───────────────────────────────────── INBOUND ─────────────────────────────────────

b8 nya_discord_poll(OUT NYA_DiscordEvent* out_event) {
    nya_assert(out_event != nullptr);

    *out_event = (NYA_DiscordEvent){ 0 };

    if (_NYA_DISCORD.event_count == 0) return false;

    nya_assert(_NYA_DISCORD.event_count <= NYA_DISCORD_MAX_EVENTS);

    *out_event = _NYA_DISCORD.events[0];

    _NYA_DISCORD.event_count--;
    nya_memmove(&_NYA_DISCORD.events[0], &_NYA_DISCORD.events[1], _NYA_DISCORD.event_count * sizeof(NYA_DiscordEvent));

    // the vacated slot, so a stale user id cannot be read out of the tail of the array.
    _NYA_DISCORD.events[_NYA_DISCORD.event_count] = (NYA_DiscordEvent){ 0 };

    nya_assert(out_event->kind > NYA_DISCORD_EVENT_NONE && out_event->kind < NYA_DISCORD_EVENT_KIND_COUNT);

    return true;
}

NYA_Error nya_discord_join_reply(NYA_ConstCString user_id, b8 accept) {
    if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_OFF) return nya_error(NYA_ERROR_NOT_OK, "the Discord plugin is not initialized");

    // refused rather than asserted: the id travels through a menu the player took a while to answer, and
    // a reload or a bad copy is an operating error.
    if (!_nya_discord_user_id_is_valid(user_id)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Discord user id that is not decimal digits");

    // the request cannot still be open across a reconnect, since Discord forgets it with the connection.
    if (_NYA_DISCORD.status != NYA_DISCORD_STATUS_CONNECTED) return nya_error(NYA_ERROR_NOT_OK, "no Discord connection to answer a join request on");

    return _nya_discord_send_user_command(accept ? "SEND_ACTIVITY_JOIN_INVITE" : "CLOSE_ACTIVITY_REQUEST", user_id);
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

void _nya_discord_event_push(NYA_DiscordEvent event) {
    nya_assert(event.kind > NYA_DISCORD_EVENT_NONE && event.kind < NYA_DISCORD_EVENT_KIND_COUNT);
    nya_assert(_NYA_DISCORD.event_count <= NYA_DISCORD_MAX_EVENTS);

    if (_NYA_DISCORD.event_count == NYA_DISCORD_MAX_EVENTS) {
        // the oldest goes, not the newest: the newest is the one whose request is still open.
        nya_memmove(&_NYA_DISCORD.events[0], &_NYA_DISCORD.events[1], (NYA_DISCORD_MAX_EVENTS - 1) * sizeof(NYA_DiscordEvent));
        _NYA_DISCORD.event_count = NYA_DISCORD_MAX_EVENTS - 1;

        // once per connection, not per drop: a client sending faster than the game drains would otherwise
        // write the log as fast as it sends.
        if (_NYA_DISCORD.events_dropped == 0) {
            nya_log_warn("Discord: more than %d unread events; the oldest are being dropped.", NYA_DISCORD_MAX_EVENTS);
        }

        _NYA_DISCORD.events_dropped++;
    }

    _NYA_DISCORD.events[_NYA_DISCORD.event_count] = event;
    _NYA_DISCORD.event_count++;
}

b8 _nya_discord_user_id_is_valid(NYA_ConstCString text) {
    if (text == nullptr) return false;

    u64 length = strnlen(text, NYA_DISCORD_MAX_USER_ID);

    // twenty digits is the most a 64 bit snowflake can be written in, and the buffer holds twenty-three.
    if (length == 0 || length >= NYA_DISCORD_MAX_USER_ID) return false;

    for (u64 i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return false;
    }

    return true;
}

b8 _nya_discord_parse_text(const NYA_Value* value, OUT char* out, u64 capacity) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 0);

    out[0] = '\0';

    if (value == nullptr || value->type != NYA_TYPE_STRING || value->as_string == nullptr) return false;

    u64 length = strnlen(value->as_string, capacity);

    // refused rather than truncated: this is a secret or a name that is compared and echoed back, and
    // half of either is a different value.
    if (length == 0 || length >= capacity) return false;

    // printable ASCII only. the string goes back out inside a JSON frame and, for a secret, into an
    // address parser; a control byte belongs in neither and nothing legitimate sends one.
    for (u64 i = 0; i < length; i++) {
        unsigned char character = (unsigned char)value->as_string[i];
        if (character < 0x20 || character > 0x7E) return false;
    }

    nya_memcpy(out, value->as_string, length);
    out[length] = '\0';

    return true;
}

b8 _nya_discord_parse_user_id(const NYA_Value* value, OUT char* out, u64 capacity) {
    nya_assert(out != nullptr);
    nya_assert(capacity >= NYA_DISCORD_MAX_USER_ID);

    out[0] = '\0';

    if (value == nullptr || value->type != NYA_TYPE_STRING) return false;
    if (!_nya_discord_user_id_is_valid(value->as_string)) return false;

    (void)snprintf(out, capacity, "%s", value->as_string);

    return true;
}

void _nya_discord_subscribe(NYA_ConstCString event) {
    nya_assert(event != nullptr);
    nya_assert(_NYA_DISCORD.status == NYA_DISCORD_STATUS_CONNECTED);

    NYA_Arena* scratch = nya_arena_create(.name = "discord_subscribe");
    defer      nya_arena_destroy(scratch);

    /* The nonce shares the counter with SET_ACTIVITY, since it only has to be unique per connection. */
    static u64 nonce = 0;
    nonce++;

    // SUBSCRIBE takes no args for these two, but the field is not optional: the client answers a frame
    // without it with an error and never sends the event.
    NYA_String* payload =
        nya_string_sprintf(scratch, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"%s\",\"nonce\":\"sub-%llu\",\"args\":{}}", event, (unsigned long long)nonce);

    if (!_nya_discord_write(_NYA_DISCORD_OPCODE_FRAME, nya_string_to_cstring(scratch, payload), (u32)payload->length)) _nya_discord_disconnect();
}

NYA_Error _nya_discord_send_user_command(NYA_ConstCString command, NYA_ConstCString user_id) {
    nya_assert(command != nullptr);
    nya_assert(_nya_discord_user_id_is_valid(user_id));

    NYA_Arena* scratch = nya_arena_create(.name = "discord_reply");
    defer      nya_arena_destroy(scratch);

    static u64 nonce = 0;
    nonce++;

    // the id is digits only, checked above, so it needs no escaping and cannot break out of the string.
    NYA_String* payload = nya_string_sprintf(scratch, "{\"cmd\":\"%s\",\"nonce\":\"reply-%llu\",\"args\":{\"user_id\":\"%s\"}}", command,
                                             (unsigned long long)nonce, user_id);

    if (!_nya_discord_write(_NYA_DISCORD_OPCODE_FRAME, nya_string_to_cstring(scratch, payload), (u32)payload->length)) {
        _nya_discord_disconnect();
        return nya_error(NYA_ERROR_NOT_OK, "the Discord connection closed while answering a join request");
    }

    return NYA_OK;
}

void _nya_discord_handle_event(NYA_ConstCString event, const NYA_Object* data) {
    nya_assert(event != nullptr);

    // Reject by default: an event this build does not know is ignored, not guessed at.
    if (nya_string_equals(event, _NYA_DISCORD_EVENT_ACTIVITY_JOIN)) {
        if (data == nullptr) return;

        NYA_DiscordEvent queued = { .kind = NYA_DISCORD_EVENT_JOIN };

        // a join with no secret is nothing to join, so it is dropped rather than queued empty.
        if (!_nya_discord_parse_text(nya_object_get(data, "secret"), queued.secret, sizeof(queued.secret))) {
            nya_log_warn("Discord: an ACTIVITY_JOIN arrived without a usable secret; ignoring it.");
            return;
        }

        _nya_discord_event_push(queued);
        return;
    }

    if (nya_string_equals(event, _NYA_DISCORD_EVENT_ACTIVITY_JOIN_REQUEST)) {
        if (data == nullptr) return;

        NYA_Value* user = nya_object_get(data, "user");
        if (user == nullptr || user->type != NYA_TYPE_OBJECT) return;

        NYA_DiscordEvent queued = { .kind = NYA_DISCORD_EVENT_JOIN_REQUEST };

        // without an id there is nothing to answer, so the request is unanswerable and dropped.
        if (!_nya_discord_parse_user_id(nya_object_get(&user->as_object, "id"), queued.user_id, sizeof(queued.user_id))) {
            nya_log_warn("Discord: an ACTIVITY_JOIN_REQUEST arrived without a usable user id; ignoring it.");
            return;
        }

        // the name is decoration: a request from somebody whose display name this build cannot render is
        // still a request, and the menu falls back to the id.
        if (!_nya_discord_parse_text(nya_object_get(&user->as_object, "global_name"), queued.user_name, sizeof(queued.user_name))) {
            (void)_nya_discord_parse_text(nya_object_get(&user->as_object, "username"), queued.user_name, sizeof(queued.user_name));
        }

        _nya_discord_event_push(queued);
        return;
    }
}

void _nya_discord_flush_activity(void) {
    if (!_NYA_DISCORD.has_pending) return;

    // unchanged since the last send, so calling nya_discord_activity_set every frame writes nothing.
    if (_NYA_DISCORD.has_sent && _nya_discord_activity_equals(&_NYA_DISCORD.pending, &_NYA_DISCORD.sent)) return;

    u64 now_ms = nya_clock_get_monotonic_ms();

    // the window is closed; the newest activity stays pending and goes out when it opens.
    /* Subtracted the right way round, so it cannot wrap. */
    if (_NYA_DISCORD.last_update_ms != 0) {
        u64 elapsed_ms = now_ms > _NYA_DISCORD.last_update_ms ? now_ms - _NYA_DISCORD.last_update_ms : 0;

        if (elapsed_ms < _NYA_DISCORD_UPDATE_INTERVAL_MS) return;
    }

    NYA_Arena* scratch = nya_arena_create(.name = "discord_activity");
    defer      nya_arena_destroy(scratch);

    NYA_String* payload = _nya_discord_activity_payload(scratch, &_NYA_DISCORD.pending);

    if (!_nya_discord_write(_NYA_DISCORD_OPCODE_FRAME, nya_string_to_cstring(scratch, payload), (u32)payload->length)) {
        _nya_discord_disconnect();
        return;
    }

    _NYA_DISCORD.sent           = _NYA_DISCORD.pending;
    _NYA_DISCORD.has_sent       = true;
    _NYA_DISCORD.last_update_ms = now_ms;
}

b8 _nya_discord_text_equals(NYA_ConstCString a, NYA_ConstCString b) {
    // absent and empty are the same, since the payload omits both.
    b8 a_empty = a == nullptr || a[0] == '\0';
    b8 b_empty = b == nullptr || b[0] == '\0';

    if (a_empty || b_empty) return a_empty && b_empty;

    return nya_string_equals(a, b);
}

b8 _nya_discord_activity_equals(const NYA_DiscordActivity* a, const NYA_DiscordActivity* b) {
    nya_assert(a != nullptr);
    nya_assert(b != nullptr);

    /* Field by field, strings by content. */
    if (!_nya_discord_text_equals(a->details, b->details)) return false;
    if (!_nya_discord_text_equals(a->state, b->state)) return false;
    if (a->start_time_s != b->start_time_s) return false;
    if (a->end_time_s != b->end_time_s) return false;
    if (!_nya_discord_text_equals(a->large_image, b->large_image)) return false;
    if (!_nya_discord_text_equals(a->large_text, b->large_text)) return false;
    if (!_nya_discord_text_equals(a->small_image, b->small_image)) return false;
    if (!_nya_discord_text_equals(a->small_text, b->small_text)) return false;
    if (!_nya_discord_text_equals(a->party_id, b->party_id)) return false;
    if (a->party_size != b->party_size) return false;
    if (a->party_max != b->party_max) return false;
    if (!_nya_discord_text_equals(a->join_secret, b->join_secret)) return false;
    if (!_nya_discord_text_equals(a->spectate_secret, b->spectate_secret)) return false;

    for (u32 i = 0; i < NYA_DISCORD_MAX_BUTTONS; i++) {
        if (!_nya_discord_text_equals(a->buttons[i].label, b->buttons[i].label)) return false;
        if (!_nya_discord_text_equals(a->buttons[i].url, b->buttons[i].url)) return false;
    }

    return true;
}

NYA_String* _nya_discord_activity_payload(NYA_Arena* arena, const NYA_DiscordActivity* activity) {
    NYA_String* out = nya_string_create(arena);

    /* The nonce is required and must differ between commands on one connection. */
    static u64 nonce = 0;
    nonce++;

    nya_string_extend_sprintf(out, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%llu\"", (unsigned long long)nonce);

    // Discord watches the pid and removes the card when the process exits, so a crashed game does not show as playing.
    nya_string_extend_sprintf(out, ",\"args\":{\"pid\":%lld", (long long)_nya_discord_process_id());

    // null activity clears presence; an empty object would leave the card up.
    if (activity == nullptr) {
        nya_string_extend(out, ",\"activity\":null}}");
        return out;
    }

    nya_string_extend(out, ",\"activity\":{");

    _nya_discord_append_string_field(out, "details", activity->details);
    _nya_discord_append_string_field(out, "state", activity->state);

    if (activity->start_time_s != 0 || activity->end_time_s != 0) {
        nya_string_extend(out, "\"timestamps\":{");

        if (activity->start_time_s != 0) nya_string_extend_sprintf(out, "\"start\":%lld,", (long long)activity->start_time_s);
        if (activity->end_time_s != 0) nya_string_extend_sprintf(out, "\"end\":%lld,", (long long)activity->end_time_s);

        nya_string_strip_suffix(out, ",");
        nya_string_extend(out, "},");
    }

    if (activity->large_image != nullptr || activity->small_image != nullptr) {
        nya_string_extend(out, "\"assets\":{");

        _nya_discord_append_string_field(out, "large_image", activity->large_image);
        _nya_discord_append_string_field(out, "large_text", activity->large_text);
        _nya_discord_append_string_field(out, "small_image", activity->small_image);
        _nya_discord_append_string_field(out, "small_text", activity->small_text);

        nya_string_strip_suffix(out, ",");
        nya_string_extend(out, "},");
    }

    if (activity->party_id != nullptr) {
        nya_string_extend(out, "\"party\":{");
        _nya_discord_append_string_field(out, "id", activity->party_id);

        // both or neither: Discord shows no count for one number and rejects a maximum below the size.
        if (activity->party_size > 0 && activity->party_max >= activity->party_size) {
            nya_string_extend_sprintf(out, "\"size\":[%u,%u],", activity->party_size, activity->party_max);
        }

        nya_string_strip_suffix(out, ",");
        nya_string_extend(out, "},");
    }

    if (activity->join_secret != nullptr || activity->spectate_secret != nullptr) {
        nya_string_extend(out, "\"secrets\":{");

        _nya_discord_append_string_field(out, "join", activity->join_secret);
        _nya_discord_append_string_field(out, "spectate", activity->spectate_secret);

        nya_string_strip_suffix(out, ",");
        nya_string_extend(out, "},");
    }

    /* Buttons, only when there are no secrets. */
    b8 has_secrets = activity->join_secret != nullptr || activity->spectate_secret != nullptr;
    b8 has_buttons = false;

    for (u32 i = 0; i < NYA_DISCORD_MAX_BUTTONS && !has_secrets; i++) {
        NYA_ConstCString label = activity->buttons[i].label;
        NYA_ConstCString url   = activity->buttons[i].url;

        // both or nothing: a button with one of the two rejects the whole activity.
        if (label == nullptr || label[0] == '\0' || url == nullptr || url[0] == '\0') continue;

        nya_string_extend(out, has_buttons ? "," : "\"buttons\":[");
        nya_string_extend(out, "{\"label\":");
        _nya_discord_append_escaped(out, label);
        nya_string_extend(out, ",\"url\":");
        _nya_discord_append_escaped(out, url);
        nya_string_extend(out, "}");

        has_buttons = true;
    }

    if (has_buttons) nya_string_extend(out, "],");

    // the trailing comma after the last field.
    nya_string_strip_suffix(out, ",");
    nya_string_extend(out, "}}}");

    return out;
}

void _nya_discord_append_string_field(NYA_String* out, NYA_ConstCString key, NYA_ConstCString value) {
    // omitted, not empty: Discord reserves a blank row for an empty string.
    if (value == nullptr || value[0] == '\0') return;

    nya_string_extend_sprintf(out, "\"%s\":", key);
    _nya_discord_append_escaped(out, value);
    nya_string_extend(out, ",");
}

void _nya_discord_append_escaped(NYA_String* out, NYA_ConstCString value) {
    nya_string_extend(out, "\"");

    /* Escaped by hand rather than through serde, which this plugin does not depend on. */
    u64 written = 0;

    for (const char* cursor = value; *cursor != '\0' && written < NYA_DISCORD_MAX_TEXT; cursor++, written++) {
        unsigned char character = (unsigned char)*cursor;

        switch (character) {
            case '"':  nya_string_extend(out, "\\\""); continue;
            case '\\': nya_string_extend(out, "\\\\"); continue;
            case '\n': nya_string_extend(out, "\\n"); continue;
            case '\r': nya_string_extend(out, "\\r"); continue;
            case '\t': nya_string_extend(out, "\\t"); continue;
            default:   break;
        }

        // control characters become \u00XX; bytes above 0x7F pass through as UTF-8.
        if (character < 0x20) {
            nya_string_extend_sprintf(out, "\\u%04x", (unsigned)character);
            continue;
        }

        nya_string_extend_sprintf(out, "%c", (char)character);
    }

    nya_string_extend(out, "\"");
}

void _nya_discord_handle_frame(u32 opcode, const u8* payload, u32 length) {
    if (opcode == _NYA_DISCORD_OPCODE_CLOSE) {
        _nya_discord_disconnect();
        return;
    }

    if (opcode == _NYA_DISCORD_OPCODE_PING) {
        // echoed verbatim, as the protocol asks; an unanswered ping drops the client within seconds.
        NYA_Arena* scratch = nya_arena_create(.name = "discord_pong");
        defer      nya_arena_destroy(scratch);

        NYA_String* echo = nya_string_create(scratch);
        for (u32 i = 0; i < length; i++) nya_string_push_back(echo, payload[i]);

        if (!_nya_discord_write(_NYA_DISCORD_OPCODE_PONG, nya_string_to_cstring(scratch, echo), (u32)echo->length)) _nya_discord_disconnect();
        return;
    }

    if (opcode != _NYA_DISCORD_OPCODE_FRAME) return;

    NYA_Arena* scratch = nya_arena_create(.name = "discord_frame");
    defer      nya_arena_destroy(scratch);

    NYA_Object* root  = nullptr;
    NYA_Error   error = nya_deserialize(scratch, payload, length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &root);

    // an unparsable frame is most likely a reply that grew a field, not a broken stream, so the connection stays.
    if (!error.ok || root == nullptr) return;

    NYA_Value* event = nya_object_get(root, "evt");
    if (event == nullptr || event->type != NYA_TYPE_STRING) return;

    NYA_Value*  data_value = nya_object_get(root, "data");
    NYA_Object* data       = data_value != nullptr && data_value->type == NYA_TYPE_OBJECT ? &data_value->as_object : nullptr;

    // the client's own errors, which arrive as a frame like any other. logged, never acted on: a refused
    // SUBSCRIBE means the feature is off for this session, not that the connection is broken.
    if (nya_string_equals(event->as_string, "ERROR")) {
        NYA_Value* message = data == nullptr ? nullptr : nya_object_get(data, "message");

        nya_log_warn("Discord refused a command: %s", message != nullptr && message->type == NYA_TYPE_STRING ? message->as_string : "(no reason given)");
        return;
    }

    if (!nya_string_equals(event->as_string, "READY")) {
        // everything after the handshake: a join, or a friend asking to join.
        _nya_discord_handle_event(event->as_string, data);
        return;
    }

    _NYA_DISCORD.status = NYA_DISCORD_STATUS_CONNECTED;

    // the first time anything worked, so the backoff resets here.
    _NYA_DISCORD.retry_delay_ms = _NYA_DISCORD_RETRY_MIN_MS;

    /* The handshake is where a reconnect re-sends the presence. */
    _NYA_DISCORD.has_sent       = false;
    _NYA_DISCORD.last_update_ms = 0;

    // and where it re-subscribes: the client forgets a subscription with the connection, so a Discord
    // restart mid-session would otherwise leave the game connected and permanently deaf.
    _NYA_DISCORD.events_dropped = 0;
    _nya_discord_subscribe(_NYA_DISCORD_EVENT_ACTIVITY_JOIN);

    // the second only if the first did not take the connection down with it.
    if (_NYA_DISCORD.status != NYA_DISCORD_STATUS_CONNECTED) return;
    _nya_discord_subscribe(_NYA_DISCORD_EVENT_ACTIVITY_JOIN_REQUEST);

    if (data == nullptr) return;

    NYA_Value* user = nya_object_get(data, "user");
    if (user == nullptr || user->type != NYA_TYPE_OBJECT) return;

    // `username`, not `global_name`, which is null for accounts without a display name.
    NYA_Value* name = nya_object_get(&user->as_object, "username");
    if (name == nullptr || name->type != NYA_TYPE_STRING) return;

    (void)snprintf(_NYA_DISCORD.user_name, sizeof(_NYA_DISCORD.user_name), "%s", name->as_string);
}

s64 _nya_discord_process_id(void) {
#if OS_WINDOWS
    return (s64)GetCurrentProcessId();
#else
    return (s64)getpid();
#endif
}

// ───────────────────────────────────── TRANSPORT ─────────────────────────────────────

#if OS_WINDOWS

b8 _nya_discord_connect(void) {
    for (u32 i = 0; i < _NYA_DISCORD_MAX_SOCKETS; i++) {
        char path[64];
        (void)snprintf(path, sizeof(path), "\\\\?\\pipe\\discord-ipc-%u", i);

        HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;

        /* Non-blocking, and refused if it cannot be made so. */
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;

        if (!SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) {
            nya_log_warn("Discord: could not put the pipe into non-blocking mode; not connecting.");
            (void)CloseHandle(handle);
            continue;
        }

        _NYA_DISCORD.handle = handle;
        return true;
    }

    return false;
}

void _nya_discord_close_handle(void) {
    if (_NYA_DISCORD.handle != INVALID_HANDLE_VALUE) (void)CloseHandle(_NYA_DISCORD.handle);

    _NYA_DISCORD.handle = INVALID_HANDLE_VALUE;
}

NYA_INTERNAL b8 _nya_discord_write_bytes(const u8* data, u32 length) {
    u32 written = 0;

    while (written < length) {
        DWORD chunk = 0;
        if (!WriteFile(_NYA_DISCORD.handle, data + written, length - written, &chunk, nullptr)) return false;
        if (chunk == 0) return false;

        written += chunk;
    }

    return true;
}

NYA_INTERNAL s64 _nya_discord_read_bytes(u8* out, u32 capacity) {
    DWORD available = 0;
    if (!PeekNamedPipe(_NYA_DISCORD.handle, nullptr, 0, nullptr, &available, nullptr)) return -1;
    if (available == 0) return 0;

    DWORD read = 0;
    if (!ReadFile(_NYA_DISCORD.handle, out, nya_min((u32)available, capacity), &read, nullptr)) return -1;

    return (s64)read;
}

#else

b8 _nya_discord_connect(void) {
    /* The socket lives under the platform's runtime directory. */
    const char* prefixes[] = { getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), getenv("TEMP"), "/tmp" };

    /* Discord from a Snap or Flatpak puts its socket inside the sandbox's own directory. */
    const char* subdirectories[] = { "", "snap.discord/", "app/com.discordapp.Discord/", "app/dev.vencord.Vesktop/" };

    for (u32 p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
        if (prefixes[p] == nullptr || prefixes[p][0] == '\0') continue;

        for (u32 s = 0; s < sizeof(subdirectories) / sizeof(subdirectories[0]); s++) {
            for (u32 i = 0; i < _NYA_DISCORD_MAX_SOCKETS; i++) {
                struct sockaddr_un address = { .sun_family = AF_UNIX };

                s32 length = snprintf(address.sun_path, sizeof(address.sun_path), "%s/%sdiscord-ipc-%u", prefixes[p], subdirectories[s], i);

                // truncated: sun_path is 108 bytes, which a deep XDG_RUNTIME_DIR plus a Flatpak path can exceed.
                if (length < 0 || (u64)length >= sizeof(address.sun_path)) continue;

                s32 handle = socket(AF_UNIX, SOCK_STREAM, 0);
                if (handle < 0) return false;

                if (connect(handle, (struct sockaddr*)&address, sizeof(address)) != 0) {
                    (void)close(handle);
                    continue;
                }

                /* Non-blocking only after connecting, so a local connect is a plain success or failure. */
                s32 flags = fcntl(handle, F_GETFL, 0);

                if (flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0) {
                    nya_log_warn("Discord: could not put the socket into non-blocking mode; not connecting.");
                    (void)close(handle);
                    continue;
                }

                _NYA_DISCORD.handle = handle;
                return true;
            }
        }
    }

    return false;
}

void _nya_discord_close_handle(void) {
    if (_NYA_DISCORD.handle >= 0) (void)close(_NYA_DISCORD.handle);

    _NYA_DISCORD.handle = -1;
}

NYA_INTERNAL b8 _nya_discord_write_bytes(const u8* data, u32 length) {
    u32 written = 0;

    while (written < length) {
        // MSG_NOSIGNAL: a peer closing mid-write raises SIGPIPE, which by default kills the process, so closing Discord would take the game with it.
        ssize_t chunk = send(_NYA_DISCORD.handle, data + written, length - written, MSG_NOSIGNAL);

        if (chunk < 0) {
            // a momentarily full buffer: spinning is correct, since a half-written frame closes the connection.
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }

        if (chunk == 0) return false;

        written += (u32)chunk;
    }

    return true;
}

NYA_INTERNAL s64 _nya_discord_read_bytes(u8* out, u32 capacity) {
    ssize_t read_count = recv(_NYA_DISCORD.handle, out, capacity, 0);

    if (read_count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }

    // zero from a stream socket is end of file (EAGAIN is "nothing now"), which triggers a reconnect.
    if (read_count == 0) return -1;

    return (s64)read_count;
}

#endif

void _nya_discord_disconnect(void) {
    _nya_discord_close_handle();

    _NYA_DISCORD.read_length  = 0;
    _NYA_DISCORD.user_name[0] = '\0';

    // Unanswered join requests go, accepted joins stay: a request is a dead conversation, but a join is a secret the player asked for.
    u32 kept = 0;

    for (u32 i = 0; i < _NYA_DISCORD.event_count; i++) {
        if (_NYA_DISCORD.events[i].kind == NYA_DISCORD_EVENT_JOIN_REQUEST) continue;

        _NYA_DISCORD.events[kept] = _NYA_DISCORD.events[i];
        kept++;
    }

    for (u32 i = kept; i < _NYA_DISCORD.event_count; i++) _NYA_DISCORD.events[i] = (NYA_DiscordEvent){ 0 };

    _NYA_DISCORD.event_count = kept;

    // OFF means deinit was called; a dropped connection retries.
    if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_OFF) return;

    _NYA_DISCORD.status = NYA_DISCORD_STATUS_DISCONNECTED;

    /* The backoff is armed here, on every disconnect, not only on a failed connect. */
    _nya_discord_arm_retry();
}

void _nya_discord_arm_retry(void) {
    _NYA_DISCORD.next_retry_ms = nya_clock_get_monotonic_ms() + _NYA_DISCORD.retry_delay_ms;

    // doubling up to a minute; without a ceiling, starting Discord an hour in would never show presence.
    _NYA_DISCORD.retry_delay_ms = nya_min(_NYA_DISCORD.retry_delay_ms * 2, (u64)_NYA_DISCORD_RETRY_MAX_MS);
}

b8 _nya_discord_write(u32 opcode, NYA_ConstCString payload, u32 payload_length) {
    if (payload_length + 8 > _NYA_DISCORD_MAX_FRAME) {
        nya_log_warn("Discord: refusing to send a %u byte frame; the limit is %d.", payload_length, _NYA_DISCORD_MAX_FRAME);
        return false;
    }

    // one buffer, one write: a header without its body is a broken stream to the client.
    u8 frame[_NYA_DISCORD_MAX_FRAME];

    u32 header[2] = { opcode, payload_length };

    // little endian, written byte by byte so big endian hosts are correct too.
    for (u32 i = 0; i < 2; i++) {
        frame[(i * 4) + 0] = (u8)(header[i] & 0xFF);
        frame[(i * 4) + 1] = (u8)((header[i] >> 8) & 0xFF);
        frame[(i * 4) + 2] = (u8)((header[i] >> 16) & 0xFF);
        frame[(i * 4) + 3] = (u8)((header[i] >> 24) & 0xFF);
    }

    nya_memcpy(frame + 8, payload, payload_length);

    return _nya_discord_write_bytes(frame, payload_length + 8);
}

void _nya_discord_read(void) {
    for (;;) {
        // after any fragment a previous pump left mid frame.
        u32 space = _NYA_DISCORD_MAX_FRAME - _NYA_DISCORD.read_length;

        // full with no complete frame means a length past the protocol maximum. dropping is the only way out of a loop.
        if (space == 0) {
            nya_log_warn("Discord: a frame larger than %d bytes arrived; dropping the connection.", _NYA_DISCORD_MAX_FRAME);
            _nya_discord_disconnect();
            return;
        }

        s64 read_count = _nya_discord_read_bytes(_NYA_DISCORD.read_buffer + _NYA_DISCORD.read_length, space);

        if (read_count < 0) {
            _nya_discord_disconnect();
            return;
        }

        if (read_count > 0) _NYA_DISCORD.read_length += (u32)read_count;

        // Every complete frame buffered, before reading again.
        for (;;) {
            if (_NYA_DISCORD.read_length < 8) break;

            const u8* buffer = _NYA_DISCORD.read_buffer;

            u32 opcode = (u32)buffer[0] | ((u32)buffer[1] << 8) | ((u32)buffer[2] << 16) | ((u32)buffer[3] << 24);
            u32 length = (u32)buffer[4] | ((u32)buffer[5] << 8) | ((u32)buffer[6] << 16) | ((u32)buffer[7] << 24);

            // checked first: a length past the buffer could never arrive.
            if (length > _NYA_DISCORD_MAX_FRAME - 8) {
                nya_log_warn("Discord: a frame declared %u bytes, past the %d byte limit; dropping the connection.", length, _NYA_DISCORD_MAX_FRAME);
                _nya_discord_disconnect();
                return;
            }

            // the body is incomplete; the next read finishes it.
            if (_NYA_DISCORD.read_length < length + 8) break;

            _nya_discord_handle_frame(opcode, buffer + 8, length);

            // the handler may have closed the connection (CLOSE, or a failed pong), which resets the buffer.
            if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_DISCONNECTED || _NYA_DISCORD.status == NYA_DISCORD_STATUS_OFF) return;

            u32 consumed = length + 8;

            _NYA_DISCORD.read_length -= consumed;
            nya_memmove(_NYA_DISCORD.read_buffer, _NYA_DISCORD.read_buffer + consumed, _NYA_DISCORD.read_length);
        }

        // nothing readable this frame.
        if (read_count == 0) return;
    }
}
