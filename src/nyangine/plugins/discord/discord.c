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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE PROTOCOL
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Every message is a header and a body written as one buffer:
 *
 *     struct { u32 opcode; u32 length; } header;   // little endian, both
 *     char json[length];                           // not null terminated
 *
 * Written in one call rather than two, because the client treats a header not immediately followed
 * by its body as a broken stream and closes the connection. That is the single easiest thing to get
 * wrong here, and it fails intermittently rather than always — a short write splits the two.
 */

#define _NYA_DISCORD_OPCODE_HANDSHAKE 0
#define _NYA_DISCORD_OPCODE_FRAME     1
#define _NYA_DISCORD_OPCODE_CLOSE     2
#define _NYA_DISCORD_OPCODE_PING      3
#define _NYA_DISCORD_OPCODE_PONG      4

/** What the reference implementation caps a frame at, header included. Anything larger is a broken peer. */
#define _NYA_DISCORD_MAX_FRAME (64 * 1024)

/** Discord names its sockets discord-ipc-0 through discord-ipc-9. Every one is tried, in order. */
#define _NYA_DISCORD_MAX_SOCKETS 10

/**
 * Discord accepts one presence update per fifteen seconds per client and silently drops the rest.
 *
 * Enforced here rather than left to the caller, because the failure mode is invisible: an update
 * sent too soon does not error, it just never appears, and the presence card then shows whatever
 * happened to land inside the window. Holding the newest and sending it when the window opens is
 * the behaviour a caller would have to write anyway.
 * */
#define _NYA_DISCORD_UPDATE_INTERVAL_MS 15000

/** How long after a failed connect before trying again, and the ceiling it doubles up to. */
#define _NYA_DISCORD_RETRY_MIN_MS 2000
#define _NYA_DISCORD_RETRY_MAX_MS 60000

/** How long to wait for the handshake reply before treating the connection as dead. */
#define _NYA_DISCORD_HANDSHAKE_TIMEOUT_MS 5000

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

    /**
     * What was most recently asked for, and what was most recently sent.
     *
     * Two copies rather than one plus a dirty flag: comparing them is how a repeated call with an
     * unchanged activity is dropped, and it is also what lets a reconnect re-send whatever is
     * current without the caller knowing a reconnect happened.
     * */
    NYA_DiscordActivity pending;
    NYA_DiscordActivity sent;
    b8                  has_pending;
    b8                  has_sent;

    /** Monotonic milliseconds. Zero means "no update has been sent, so the window is open". */
    u64 last_update_ms;

    /** Monotonic milliseconds at which another connect may be attempted, and the current backoff. */
    u64 next_retry_ms;
    u64 retry_delay_ms;

    /**
     * When to give up waiting for the handshake reply.
     *
     * A client can accept the socket and then never answer — it is starting up, or it is wedged. With
     * no deadline the module sits in CONNECTING forever: the pump only retries from DISCONNECTED, so
     * nothing would ever try again and presence would be dead for the rest of the session.
     * */
    u64 handshake_deadline_ms;

    /** Filled from the READY payload. Empty until the handshake completes. */
    char user_name[NYA_DISCORD_MAX_TEXT];

    /**
     * Partial frame carried between pumps.
     *
     * The socket is non-blocking, so a read can stop in the middle of a header or a body. Without
     * somewhere to keep the fragment the next pump would read the remainder and interpret its first
     * four bytes as an opcode.
     * */
    u8  read_buffer[_NYA_DISCORD_MAX_FRAME];
    u32 read_length;
} _NYA_DiscordSystem;

/**
 * The one system, as a file scope static.
 *
 * Like nya_arena_global and the i18n system: a plugin cannot hang state off NYA_App, because core is
 * above plugins and a headless tool compiles the plugin without an app at all. Safe across a hot
 * reload for the same reason — this is compiled into the executable, not into the game's library.
 * */
NYA_INTERNAL _NYA_DiscordSystem _NYA_DISCORD = { 0 };

/** Opens the first Discord socket that answers. False when none does, which is the ordinary case. */
NYA_INTERNAL b8 _nya_discord_connect(void);

/**
 * Drops the connection and arms the retry backoff.
 *
 * Everything that notices the connection is gone goes through here, so the backoff cannot be armed
 * on one path and forgotten on another — which is exactly what happened when only a failed *connect*
 * armed it. See the note in the implementation.
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

/**
 * nya_string_equals, but null is a value rather than an assertion.
 *
 * Every field of NYA_DiscordActivity is optional and therefore routinely null, so the comparison
 * this module needs is "are these the same text", where two absent strings are the same and an
 * absent one differs from any present one. nya_string_equals asserts on either argument being null.
 * */
NYA_INTERNAL b8 _nya_discord_text_equals(NYA_ConstCString a, NYA_ConstCString b) __attr_no_discard;

/** Sends `pending` if the rate limit window is open. Called from the pump and after a connect. */
NYA_INTERNAL void _nya_discord_flush_activity(void);

/** The process id, which SET_ACTIVITY requires and Discord uses to notice the game exiting. */
NYA_INTERNAL s64 _nya_discord_process_id(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

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

    // Deliberately not connecting here. Discord not running is the ordinary state and init would
    // then either block, fail, or lie — and a game must not have to care which. The pump connects.
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

        // A retry every frame would be a failed connect() sixty times a second for as long as the
        // player does not have Discord open, which is most players most of the time.
        if (!_nya_discord_connect()) {
            _nya_discord_arm_retry();
            return;
        }

        /*
         * The backoff is *not* reset here.
         *
         * Opening the socket is not success — Discord accepts it before deciding whether it likes
         * the application id. Resetting on connect meant a client that hung up immediately reset the
         * delay to its minimum every time, so the backoff never actually grew and the retry loop
         * stayed hot. It is reset when the handshake is answered, in _nya_discord_handle_frame,
         * which is the first moment anything has actually worked.
         */
        _NYA_DISCORD.status                = NYA_DISCORD_STATUS_CONNECTING;
        _NYA_DISCORD.handshake_deadline_ms = now_ms + _NYA_DISCORD_HANDSHAKE_TIMEOUT_MS;

        NYA_Arena* scratch = nya_arena_create(.name = "discord_handshake");
        defer      nya_arena_destroy(scratch);

        // The client id is a *string* in the handshake even though it is a number everywhere else.
        // Sending it unquoted is accepted by the socket and then silently never answered.
        NYA_String* handshake = nya_string_sprintf(scratch, "{\"v\":1,\"client_id\":\"%llu\"}", (unsigned long long)_NYA_DISCORD.application_id);

        if (!_nya_discord_write(_NYA_DISCORD_OPCODE_HANDSHAKE, nya_string_to_cstring(scratch, handshake), (u32)handshake->length)) {
            _nya_discord_disconnect();
            return;
        }
    }

    _nya_discord_read();

    // Accepted and then never answered. Without this the module sits in CONNECTING forever, because
    // only DISCONNECTED retries.
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

    // Held rather than sent. The pump owns the socket and the rate limit window, so this is only
    // ever "what the game wants shown" — which is also why it works before Discord is running.
    _NYA_DISCORD.pending     = activity;
    _NYA_DISCORD.has_pending = true;

    return NYA_OK;
}

NYA_Error nya_discord_activity_clear(void) {
    if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_OFF) return nya_error(NYA_ERROR_NOT_OK, "the Discord plugin is not initialized");

    _NYA_DISCORD.pending     = (NYA_DiscordActivity){ 0 };
    _NYA_DISCORD.has_pending = false;

    // Not routed through `pending`, because "no activity" and "an activity with every field empty"
    // are different messages and only one of them takes the card down.
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

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_discord_flush_activity(void) {
    if (!_NYA_DISCORD.has_pending) return;

    // Unchanged since the last send, so there is nothing to say. This is what makes calling
    // nya_discord_activity_set every frame free rather than a socket write every frame.
    if (_NYA_DISCORD.has_sent && _nya_discord_activity_equals(&_NYA_DISCORD.pending, &_NYA_DISCORD.sent)) return;

    u64 now_ms = nya_clock_get_monotonic_ms();

    // The window is closed. Held, not dropped: the newest activity is still in `pending` and goes
    // out on the pump after the window opens, so the card ends up correct rather than stale.
    /*
     * Compared with the subtraction the right way round, so it cannot wrap.
     *
     * `last_update_ms` can be the *later* of the two reads — it is set by code that samples the clock
     * itself, after this function's caller already did. Unsigned, that wraps to something near U64_MAX,
     * which this comparison would read as "ages have passed" and the rate limit would let everything
     * through. Testing `now_ms > last` first makes the subtraction safe by construction.
     */
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
    // Absent and empty are treated as the same thing, because the payload builder omits both — so
    // two activities differing only in that produce byte-identical frames and must not be re-sent.
    b8 a_empty = a == nullptr || a[0] == '\0';
    b8 b_empty = b == nullptr || b[0] == '\0';

    if (a_empty || b_empty) return a_empty && b_empty;

    return nya_string_equals(a, b);
}

b8 _nya_discord_activity_equals(const NYA_DiscordActivity* a, const NYA_DiscordActivity* b) {
    nya_assert(a != nullptr);
    nya_assert(b != nullptr);

    /*
     * Field by field, comparing strings by content.
     *
     * Not nya_memcmp over the structs, which would compare *pointers* — a caller formatting its
     * details line into a stack buffer produces a different pointer every frame with identical
     * bytes behind it, so a pointer comparison would report a change every frame and send a socket
     * write every frame, which is precisely what this exists to avoid.
     */
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

    /*
     * The nonce is required and must differ between commands on one connection.
     *
     * Discord echoes it back on the reply, which is how a caller matches replies to requests.
     * Nothing here waits on a reply, so a counter is enough — and it only has to be unique *within a
     * connection*, which is why it does not need to survive a restart and does not need an RNG.
     */
    static u64 nonce = 0;
    nonce++;

    nya_string_extend_sprintf(out, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%llu\"", (unsigned long long)nonce);

    // The pid is not decoration: Discord watches it and takes the card down when the process exits,
    // which is what stops a crashed game showing as still playing.
    nya_string_extend_sprintf(out, ",\"args\":{\"pid\":%lld", (long long)_nya_discord_process_id());

    // Null activity is how presence is cleared. An empty object would be an activity with no fields,
    // which leaves the card up showing the game's name.
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

        // Both or neither. Discord shows no count for a party with one number, and rejects a maximum
        // below the current size — so a half-filled pair is silently worse than none.
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

    /*
     * Buttons, and only when there are no secrets.
     *
     * Discord refuses an activity carrying both and the refusal is silent — the card simply does not
     * update. Dropping the buttons rather than the secrets, because a join secret is functional and
     * a button is decorative.
     */
    b8 has_secrets = activity->join_secret != nullptr || activity->spectate_secret != nullptr;
    b8 has_buttons = false;

    for (u32 i = 0; i < NYA_DISCORD_MAX_BUTTONS && !has_secrets; i++) {
        NYA_ConstCString label = activity->buttons[i].label;
        NYA_ConstCString url   = activity->buttons[i].url;

        // Both or nothing: a button with one of the two is rejected along with the whole activity.
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

    // Whatever the last field was left one behind it.
    nya_string_strip_suffix(out, ",");
    nya_string_extend(out, "}}}");

    return out;
}

void _nya_discord_append_string_field(NYA_String* out, NYA_ConstCString key, NYA_ConstCString value) {
    // Omitted rather than sent empty. Discord treats an empty string as a value and reserves the
    // line for it, so a card with an empty state has a blank row where nothing should be.
    if (value == nullptr || value[0] == '\0') return;

    nya_string_extend_sprintf(out, "\"%s\":", key);
    _nya_discord_append_escaped(out, value);
    nya_string_extend(out, ",");
}

void _nya_discord_append_escaped(NYA_String* out, NYA_ConstCString value) {
    nya_string_extend(out, "\"");

    /*
     * Escaped by hand rather than through serde.
     *
     * This is the only JSON this module writes and it is a fixed shape, so building an NYA_Object to
     * serialize would cost an allocation per field to produce a string this loop produces directly.
     * What must not be skipped is the escaping itself: a player's party name reaches this, and an
     * unescaped quote in it produces a malformed frame that the client answers by closing the
     * connection.
     *
     * Truncated at NYA_DISCORD_MAX_TEXT, which is above Discord's own 128 byte limit, so a longer
     * string is already going to be cut — better here, where the frame stays valid, than by a peer
     * that rejects the whole activity.
     */
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

        // Everything below space has to be escaped as \u00XX; JSON has no literal control characters.
        // Bytes above 0x7F are passed through, which is correct for the UTF-8 this is given.
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
        // Echoed verbatim, which is what the protocol asks for. A client that does not answer a ping
        // is dropped after a few seconds.
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

    // A frame this module cannot parse is not a reason to drop the connection: it is far more likely
    // to be a command reply in a shape that has grown a field than a broken stream.
    if (!error.ok || root == nullptr) return;

    NYA_Value* event = nya_object_get(root, "evt");
    if (event == nullptr || event->type != NYA_TYPE_STRING) return;
    if (!nya_string_equals(event->as_string, "READY")) return;

    _NYA_DISCORD.status = NYA_DISCORD_STATUS_CONNECTED;

    // The first moment anything has actually worked, and therefore the only honest place to reset
    // the backoff. See the note where it deliberately is not reset on connect.
    _NYA_DISCORD.retry_delay_ms = _NYA_DISCORD_RETRY_MIN_MS;

    /*
     * The handshake is where a reconnect re-sends the presence.
     *
     * Clearing `has_sent` is what does it: the flush then sees pending differing from nothing and
     * writes it. And clearing `last_update_ms` reopens the rate limit window, because the limit is
     * per connection and the new client has seen no updates from us at all — without that, a
     * reconnect inside fifteen seconds of the last update would come up with no presence.
     */
    _NYA_DISCORD.has_sent       = false;
    _NYA_DISCORD.last_update_ms = 0;

    NYA_Value* data = nya_object_get(root, "data");
    if (data == nullptr || data->type != NYA_TYPE_OBJECT) return;

    NYA_Value* user = nya_object_get(&data->as_object, "user");
    if (user == nullptr || user->type != NYA_TYPE_OBJECT) return;

    // `username` rather than `global_name`: the latter is null for accounts that never set a display
    // name, and this is only ever shown as "signed in as".
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

/*
 * ─────────────────────────────────────────────────────────
 * TRANSPORT
 * ─────────────────────────────────────────────────────────
 */

#if OS_WINDOWS

b8 _nya_discord_connect(void) {
    for (u32 i = 0; i < _NYA_DISCORD_MAX_SOCKETS; i++) {
        char path[64];
        (void)snprintf(path, sizeof(path), "\\\\?\\pipe\\discord-ipc-%u", i);

        HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;

        /*
         * Non-blocking, and the connection is refused if it cannot be made so.
         *
         * Not decoration: nya_discord_pump runs inside the frame, and a blocking handle turns a read
         * with nothing behind it into a stall of unbounded length — a frame hitch caused by Discord
         * being busy. Better to have no presence than a game that pauses for it.
         */
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
    /*
     * The socket lives under the runtime directory, and which one that is depends on the platform.
     *
     * XDG_RUNTIME_DIR is the correct answer on a modern Linux desktop; the TMPDIR family is the
     * fallback and what macOS uses. /tmp last, because it is right often enough to be worth trying
     * and wrong to rely on.
     */
    const char* prefixes[] = { getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), getenv("TEMP"), "/tmp" };

    /*
     * Discord installed from a Snap or a Flatpak puts its socket inside that sandbox's own
     * directory rather than at the top of the runtime directory.
     *
     * Both are common enough on Linux that omitting them means "rich presence does not work" for a
     * large share of players, with nothing to see: the connect simply never succeeds.
     */
    const char* subdirectories[] = { "", "snap.discord/", "app/com.discordapp.Discord/", "app/dev.vencord.Vesktop/" };

    for (u32 p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
        if (prefixes[p] == nullptr || prefixes[p][0] == '\0') continue;

        for (u32 s = 0; s < sizeof(subdirectories) / sizeof(subdirectories[0]); s++) {
            for (u32 i = 0; i < _NYA_DISCORD_MAX_SOCKETS; i++) {
                struct sockaddr_un address = { .sun_family = AF_UNIX };

                s32 length = snprintf(address.sun_path, sizeof(address.sun_path), "%s/%sdiscord-ipc-%u", prefixes[p], subdirectories[s], i);

                // Truncated, so this is not the path anything is listening on. sun_path is 108 bytes
                // and a deep XDG_RUNTIME_DIR plus a Flatpak subdirectory genuinely reaches it.
                if (length < 0 || (u64)length >= sizeof(address.sun_path)) continue;

                s32 handle = socket(AF_UNIX, SOCK_STREAM, 0);
                if (handle < 0) return false;

                if (connect(handle, (struct sockaddr*)&address, sizeof(address)) != 0) {
                    (void)close(handle);
                    continue;
                }

                /*
                 * Non-blocking only after connecting, so the connect itself is a simple success or
                 * failure rather than an EINPROGRESS to poll. A local socket connects immediately.
                 *
                 * Both calls are checked and the socket is dropped if either fails. This used to
                 * ignore the result, which meant a socket that stayed blocking was still reported as
                 * connected — and then nya_discord_pump, which runs inside the frame, would block in
                 * recv for as long as Discord took to say anything. A frame hitch is a worse outcome
                 * than no presence at all.
                 */
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
        /*
         * MSG_NOSIGNAL, because the peer going away mid-write raises SIGPIPE otherwise — and the
         * default disposition of SIGPIPE is to kill the process. Discord being closed by the player
         * would take the game with it.
         */
        ssize_t chunk = send(_NYA_DISCORD.handle, data + written, length - written, MSG_NOSIGNAL);

        if (chunk < 0) {
            // The socket is non-blocking and the buffer is momentarily full. Rare for a frame this
            // small, and a spin is correct: the alternative is a half written frame on the wire,
            // which the client answers by closing the connection.
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

    // Zero from a stream socket is end of file, not "nothing right now" — that is what the EAGAIN
    // above is. Reporting it as an error is what makes a closed Discord trigger a reconnect.
    if (read_count == 0) return -1;

    return (s64)read_count;
}

#endif

void _nya_discord_disconnect(void) {
    _nya_discord_close_handle();

    _NYA_DISCORD.read_length  = 0;
    _NYA_DISCORD.user_name[0] = '\0';

    // OFF means deinit was called; a dropped connection goes back to retrying instead.
    if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_OFF) return;

    _NYA_DISCORD.status = NYA_DISCORD_STATUS_DISCONNECTED;

    /*
     * Arming the backoff *here* rather than only where a connect fails.
     *
     * Only the failed-connect path used to arm it, and a connection that is accepted and then closed
     * takes neither of those branches — so `next_retry_ms` stayed where it was and the next pump
     * reconnected immediately. Discord answers an unrecognised application id by accepting the
     * socket and replying `{"code":4000,"message":"Invalid Client ID"}` with a CLOSE, which is
     * precisely that shape: a game shipped with the wrong id opened a socket, wrote a handshake and
     * was hung up on sixty times a second for as long as it ran.
     *
     * A client shutting down normally is the same shape and the same fix.
     */
    _nya_discord_arm_retry();
}

void _nya_discord_arm_retry(void) {
    _NYA_DISCORD.next_retry_ms = nya_clock_get_monotonic_ms() + _NYA_DISCORD.retry_delay_ms;

    // Doubling to a minute. The ceiling exists because backing off forever means a player who starts
    // Discord an hour in never gets presence.
    _NYA_DISCORD.retry_delay_ms = nya_min(_NYA_DISCORD.retry_delay_ms * 2, (u64)_NYA_DISCORD_RETRY_MAX_MS);
}

b8 _nya_discord_write(u32 opcode, NYA_ConstCString payload, u32 payload_length) {
    if (payload_length + 8 > _NYA_DISCORD_MAX_FRAME) {
        nya_log_warn("Discord: refusing to send a %u byte frame; the limit is %d.", payload_length, _NYA_DISCORD_MAX_FRAME);
        return false;
    }

    // One buffer, one write. See the note at the top of this file: a header not immediately followed
    // by its body is a broken stream as far as the client is concerned.
    u8 frame[_NYA_DISCORD_MAX_FRAME];

    u32 header[2] = { opcode, payload_length };

    // Little endian on the wire. Written byte by byte rather than by copying the u32s, so this is
    // correct on a big endian host too rather than only on the ones anyone tests on.
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
        // Into whatever is left of the buffer, behind any fragment a previous pump stopped mid frame.
        u32 space = _NYA_DISCORD_MAX_FRAME - _NYA_DISCORD.read_length;

        // Full with no complete frame in it means the peer is sending something this cannot be: the
        // length prefix said more than the protocol's own maximum. Dropping the connection is the
        // only move that does not loop forever.
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

        // Every complete frame currently buffered, before deciding whether to read again.
        for (;;) {
            if (_NYA_DISCORD.read_length < 8) break;

            const u8* buffer = _NYA_DISCORD.read_buffer;

            u32 opcode = (u32)buffer[0] | ((u32)buffer[1] << 8) | ((u32)buffer[2] << 16) | ((u32)buffer[3] << 24);
            u32 length = (u32)buffer[4] | ((u32)buffer[5] << 8) | ((u32)buffer[6] << 16) | ((u32)buffer[7] << 24);

            // Checked before it is used as a bound. A length past the buffer would otherwise be
            // waited on forever, since it can never arrive.
            if (length > _NYA_DISCORD_MAX_FRAME - 8) {
                nya_log_warn("Discord: a frame declared %u bytes, past the %d byte limit; dropping the connection.", length, _NYA_DISCORD_MAX_FRAME);
                _nya_discord_disconnect();
                return;
            }

            // The body has not all arrived. Left in the buffer for the next read to complete.
            if (_NYA_DISCORD.read_length < length + 8) break;

            _nya_discord_handle_frame(opcode, buffer + 8, length);

            // The handler may have closed the connection — a CLOSE frame, or a failed pong — which
            // resets read_length. Continuing to shuffle the buffer would then be reading freed state.
            if (_NYA_DISCORD.status == NYA_DISCORD_STATUS_DISCONNECTED || _NYA_DISCORD.status == NYA_DISCORD_STATUS_OFF) return;

            u32 consumed = length + 8;

            _NYA_DISCORD.read_length -= consumed;
            nya_memmove(_NYA_DISCORD.read_buffer, _NYA_DISCORD.read_buffer + consumed, _NYA_DISCORD.read_length);
        }

        // Nothing was readable, so there is nothing more to drain this frame.
        if (read_count == 0) return;
    }
}
