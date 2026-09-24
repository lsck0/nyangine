/**
 * @file discord_gateway.h
 *
 * The Discord bot gateway: the long lived `wss` connection a bot user holds open, over which Discord
 * sends everything that happens in the servers the bot is in.
 *
 * This is not `plugins/discord`. That one is the GameSDK — rich presence and join secrets for a game
 * that happens to be launched from Discord. This one is a program that *is* a Discord client: it logs
 * in as a bot user with a token, declares which events it wants, and receives them. The two share a
 * vendor and nothing else, which is why they are separate directories.
 *
 * ```c
 * NYA_DiscordGateway* gateway = nullptr;
 * NYA_EXPECT(nya_discord_gateway_create(arena, (NYA_DiscordGatewayOptions){
 *     .token   = token,
 *     .intents = NYA_DISCORD_INTENT_GUILDS | NYA_DISCORD_INTENT_GUILD_MESSAGES | NYA_DISCORD_INTENT_MESSAGE_CONTENT,
 * }, &gateway));
 * defer nya_discord_gateway_destroy(gateway);
 *
 * // once a frame
 * NYA_DiscordGatewayEvent event = { 0 };
 * while (nya_discord_gateway_poll(gateway, &event)) {
 *     switch (event.kind) {
 *         case NYA_DISCORD_GATEWAY_EVENT_READY:    nya_log_info("logged in as %s", event.name); break;
 *         case NYA_DISCORD_GATEWAY_EVENT_DISPATCH: handle(event.name, event.data);              break;
 *         case NYA_DISCORD_GATEWAY_EVENT_FATAL:    stop("%s", event.reason);                    break;
 *         default: break;
 *     }
 * }
 * ```
 *
 * ── the protocol, in the order it happens ──
 *
 * Connect; the server sends HELLO with a heartbeat interval. Heartbeat every interval, the first one
 * after a random fraction of it — that jitter is not decoration, it is what stops every bot in the world
 * beating on the same millisecond after Discord restarts a gateway node. Send IDENTIFY with the token
 * and the intents mask; the server answers READY with a session id and the url to resume on. From then
 * on every payload may carry a sequence number, which is remembered because it is what a RESUME replays
 * from. Every heartbeat is answered with HEARTBEAT_ACK; a heartbeat that goes unanswered before the next
 * one is due means the connection is a zombie — the socket is open and nothing is coming through it — so
 * it is dropped and resumed rather than waited on.
 *
 * On a close that Discord says is resumable, reconnect to the resume url and send RESUME with the
 * session and the sequence, and the events missed in between arrive. On one that is not, the session is
 * gone: connect and IDENTIFY afresh.
 *
 * ── the close codes that must not be retried ──
 *
 * 4004, 4010, 4011, 4012, 4013 and 4014 mean the token is wrong, the shard is wrong, or the intents were
 * never granted. None of them get better by trying again, and a bot that reconnects in a loop against a
 * rejected token has its token disabled by Discord — so those end in
 * NYA_DISCORD_GATEWAY_EVENT_FATAL and the client never dials again. Everything else is retried on an
 * exponential backoff. nya_discord_gateway_close_action is the whole table, public so a program that
 * keeps its own socket asks the same question the same way.
 *
 * ── the token, and what a memory read gets ──
 *
 * The token is copied into the client at create and wiped with nya_crypto_wipe at destroy. It is never
 * logged, never put in an NYA_Error message, and never reaches an event handed to the caller: an
 * IDENTIFY and a RESUME are built in a buffer inside the client and that buffer is wiped as soon as the
 * bytes are queued, rather than serialized through the caller's arena where a copy would sit until the
 * arena died.
 *
 * That is the whole of it, and it is worth being plain about what it does not do. An attacker who can
 * read this process's memory while the bot is running gets the token, in cleartext:
 *
 * - The client's own copy is there for as long as the client is, because every reconnect has to send it
 *   again.
 * - libcurl keeps its own copy of the `Authorization` header for the REST side, inside its handle, and
 *   nothing here can reach it.
 * - The kernel socket buffers hold the bytes of the last IDENTIFY until they are sent, and the
 *   websocket's send queue holds them until it flushes.
 * - A core dump written while the bot is connected contains all of the above.
 *
 * The wipe buys exactly two things: the token is not in memory after nya_discord_gateway_destroy, and it
 * is not in the long lived arena the rest of the program allocates from. Anything more than that wants
 * the token in another process, and that is not what this is. Read it from the environment or a file
 * outside the repository, never from a literal, and if it leaks, rotate it — a Discord token cannot be
 * revoked by anything but regenerating it.
 *
 * ── the transport is a seam ──
 *
 * The gateway does not name a socket. It drives NYA_DiscordGatewayTransport, which is the websocket
 * client by default and is the reason the state machine can be tested against canned frames with no
 * network at all — see tests/nyangine/plugins/test_discord_gateway.c. The clock and the jitter are part
 * of that seam for the same reason: a test makes a heartbeat come due without waiting a minute for it.
 *
 * Nothing here blocks. The poll advances the connection a little, hands out at most one event, and
 * returns, so a game hosting a bot keeps rendering.
 *
 * Thread safety: none. One thread owns a gateway for its whole life.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_types.h"
#include "nyangine/http/http_websocket.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/** Where a bot dials when it has no resume url yet. Version 10, JSON; `etf` is not implemented. */
#define NYA_DISCORD_GATEWAY_URL "wss://gateway.discord.gg/?v=10&encoding=json"

/**
 * Token bytes held, the terminator included.
 *
 * A bot token today is three dot separated base64url parts and lands near seventy bytes. This is
 * comfortably past that without being a place to put something that is not a token; a longer one is
 * refused at create rather than silently cut, since half a token authenticates nothing and the failure
 * would look like a wrong password.
 * */
#define NYA_DISCORD_GATEWAY_MAX_TOKEN 128

/** Session id bytes, terminator included. Discord's is a 32 character hex string. */
#define NYA_DISCORD_GATEWAY_MAX_SESSION 64

/** Resume url bytes, terminator included. Discord sends a hostname under its own domain. */
#define NYA_DISCORD_GATEWAY_MAX_URL 256

/** Dispatch name bytes, terminator included. The longest Discord defines is under forty. */
#define NYA_DISCORD_GATEWAY_MAX_EVENT_NAME 64

/**
 * The default ceiling on one gateway payload.
 *
 * READY is the big one: it lists every guild the bot is in, and a bot in a few thousand of them sends
 * hundreds of kilobytes before anything else happens. One megabyte covers that and is the buffer, paid
 * once at create. A bot that is in more guilds than that wants sharding, not a bigger buffer.
 * */
#define NYA_DISCORD_GATEWAY_MAX_MESSAGE_BYTES 1048576

/** The first reconnect waits about this long, and each further one doubles it. */
#define NYA_DISCORD_GATEWAY_BACKOFF_MIN_MS 1000

/** Where the doubling stops. A minute between attempts is polite to a gateway that is having a bad day. */
#define NYA_DISCORD_GATEWAY_BACKOFF_MAX_MS 60000

/**
 * The shortest gap between two IDENTIFYs, which Discord's own limit is five seconds.
 *
 * Exceeding it is a 4008 and repeated offences invalidate the token, so the client waits this out in
 * IDENTIFYING rather than trusting the backoff to have been long enough.
 * */
#define NYA_DISCORD_GATEWAY_IDENTIFY_INTERVAL_MS 5000

/**
 * How many payloads one poll may handle before returning with nothing.
 *
 * HELLO, HEARTBEAT_ACK and a server side HEARTBEAT are answered internally and produce no event, so a
 * poll has to keep going to find a dispatch. This bounds that: past it the poll returns and the caller's
 * next one continues, which is what stops a peer that sends nothing but ACKs from owning the frame.
 * */
#define NYA_DISCORD_GATEWAY_MAX_STEPS_PER_POLL 32

// ───────────────────────────────────── TYPES ─────────────────────────────────────

typedef enum NYA_DiscordGatewayState      NYA_DiscordGatewayState;
typedef enum NYA_DiscordGatewayEventKind  NYA_DiscordGatewayEventKind;
typedef enum NYA_DiscordGatewayCloseAction NYA_DiscordGatewayCloseAction;
typedef struct NYA_DiscordGatewayTransport NYA_DiscordGatewayTransport;
typedef struct NYA_DiscordGatewayEvent    NYA_DiscordGatewayEvent;
typedef struct NYA_DiscordGatewayOptions  NYA_DiscordGatewayOptions;
typedef struct NYA_DiscordGateway         NYA_DiscordGateway;

/**
 * Which events Discord sends, as the bitmask IDENTIFY carries.
 *
 * Asking for nothing is legal and gets a connection that only heartbeats, which is what a bot that only
 * answers interactions over HTTP wants. The three marked privileged have to be enabled on the
 * application's page first, and asking for one that is not enabled is close code 4014 — fatal, not
 * retried. Values are Discord's, not this engine's, and go on the wire as written.
 * */
typedef enum {
    NYA_DISCORD_INTENT_GUILDS                        = 1U << 0,

    /** Privileged. */
    NYA_DISCORD_INTENT_GUILD_MEMBERS                 = 1U << 1,

    NYA_DISCORD_INTENT_GUILD_MODERATION              = 1U << 2,
    NYA_DISCORD_INTENT_GUILD_EXPRESSIONS             = 1U << 3,
    NYA_DISCORD_INTENT_GUILD_INTEGRATIONS            = 1U << 4,
    NYA_DISCORD_INTENT_GUILD_WEBHOOKS                = 1U << 5,
    NYA_DISCORD_INTENT_GUILD_INVITES                 = 1U << 6,
    NYA_DISCORD_INTENT_GUILD_VOICE_STATES            = 1U << 7,

    /** Privileged. */
    NYA_DISCORD_INTENT_GUILD_PRESENCES               = 1U << 8,

    NYA_DISCORD_INTENT_GUILD_MESSAGES                = 1U << 9,
    NYA_DISCORD_INTENT_GUILD_MESSAGE_REACTIONS       = 1U << 10,
    NYA_DISCORD_INTENT_GUILD_MESSAGE_TYPING          = 1U << 11,
    NYA_DISCORD_INTENT_DIRECT_MESSAGES               = 1U << 12,
    NYA_DISCORD_INTENT_DIRECT_MESSAGE_REACTIONS      = 1U << 13,
    NYA_DISCORD_INTENT_DIRECT_MESSAGE_TYPING         = 1U << 14,

    /** Privileged. Without it every message arrives with an empty `content`. */
    NYA_DISCORD_INTENT_MESSAGE_CONTENT               = 1U << 15,

    NYA_DISCORD_INTENT_GUILD_SCHEDULED_EVENTS        = 1U << 16,
    NYA_DISCORD_INTENT_AUTO_MODERATION_CONFIGURATION = 1U << 20,
    NYA_DISCORD_INTENT_AUTO_MODERATION_EXECUTION     = 1U << 21,
    NYA_DISCORD_INTENT_GUILD_MESSAGE_POLLS           = 1U << 24,
    NYA_DISCORD_INTENT_DIRECT_MESSAGE_POLLS          = 1U << 25,
} NYA_DiscordIntent;

enum NYA_DiscordGatewayState {
    /** Created and not dialled yet, or waiting out a backoff between attempts. */
    NYA_DISCORD_GATEWAY_STATE_IDLE = 0,

    /** The socket is being opened and HELLO has not arrived. */
    NYA_DISCORD_GATEWAY_STATE_CONNECTING,

    /** HELLO arrived; IDENTIFY or RESUME is waiting on its rate limit, or on the server's answer. */
    NYA_DISCORD_GATEWAY_STATE_IDENTIFYING,

    /** Logged in. Dispatches arrive and heartbeats are answered. */
    NYA_DISCORD_GATEWAY_STATE_READY,

    /**
     * Discord refused the token, the shard or the intents. Terminal: nothing is retried, because
     * retrying is what gets a token disabled.
     * */
    NYA_DISCORD_GATEWAY_STATE_FATAL,

    NYA_DISCORD_GATEWAY_STATE_COUNT,
};

enum NYA_DiscordGatewayEventKind {
    NYA_DISCORD_GATEWAY_EVENT_NONE = 0,

    /** Logged in with a new session. `name` is the bot user's name, `data` the whole READY payload. */
    NYA_DISCORD_GATEWAY_EVENT_READY,

    /** The old session was picked up again and whatever was missed has been replayed. */
    NYA_DISCORD_GATEWAY_EVENT_RESUMED,

    /** Anything else Discord dispatched: `name` is "MESSAGE_CREATE" and so on, `data` its payload. */
    NYA_DISCORD_GATEWAY_EVENT_DISPATCH,

    /** The connection ended and will be retried. `code` and `reason` say why, `sequence` when. */
    NYA_DISCORD_GATEWAY_EVENT_DISCONNECTED,

    /** The connection ended and will not be retried. The state is FATAL from here on. */
    NYA_DISCORD_GATEWAY_EVENT_FATAL,

    NYA_DISCORD_GATEWAY_EVENT_KIND_COUNT,
};

/** What a close code obliges the client to do next. */
enum NYA_DiscordGatewayCloseAction {
    /** Reconnect and RESUME: the session survives and the missed events are replayed. */
    NYA_DISCORD_GATEWAY_CLOSE_RESUME = 0,

    /** Reconnect and IDENTIFY: the session is gone, and resuming it would only be refused again. */
    NYA_DISCORD_GATEWAY_CLOSE_REIDENTIFY,

    /** Do not reconnect. The token, the shard or the intents are wrong and retrying has a cost. */
    NYA_DISCORD_GATEWAY_CLOSE_FATAL,

    NYA_DISCORD_GATEWAY_CLOSE_ACTION_COUNT,
};

/**
 * Where the gateway gets its bytes, its time and its randomness.
 *
 * Left zeroed in the options, the client fills it with the websocket client from `plugins/curl`, the
 * monotonic clock and the OS CSPRNG, which is what a program wants. A test fills it with a queue of
 * canned frames and a clock it moves by hand, which is the only way to prove a heartbeat fires on time
 * without waiting for one.
 * */
struct NYA_DiscordGatewayTransport {
    void* user;

    /** Starts connecting to `url`. Returns as soon as it is under way, like nya_websocket_create. */
    NYA_Error (*open)(void* user, NYA_ConstCString url);

    /** Tears down whatever `open` made. Called for every ending, including the ones the peer started. */
    void (*close)(void* user);

    /** One event, or false when there is nothing. The contract nya_websocket_poll has. */
    b8 (*poll)(void* user, OUT NYA_WebSocketEvent* out_event);

    /** Queues one text frame. */
    NYA_Error (*send)(void* user, const char* text, u64 size);

    /** Monotonic milliseconds. Every deadline in the client is measured against this and nothing else. */
    u64 (*now_ms)(void* user);

    /** A fraction in [0, 1), for the first heartbeat's jitter and the backoff's. */
    f32 (*jitter)(void* user);
};

struct NYA_DiscordGatewayEvent {
    NYA_DiscordGatewayEventKind kind;

    /**
     * READY: the bot user's name. DISPATCH: the event name. Empty otherwise, never null.
     * */
    NYA_ConstCString name;

    /**
     * READY, RESUMED and DISPATCH: the payload's `d` field, parsed. Null when there was none. Valid
     * until the next poll, which is where the payload arena is reset.
     * */
    const NYA_Object* data;

    /** DISCONNECTED and FATAL: the close code, as Discord's table numbers them. */
    u16 code;

    /** DISCONNECTED and FATAL: why, in words, and never carrying the token. Never null. */
    NYA_ConstCString reason;

    /** DISCONNECTED: how long until the next attempt. Zero on every other kind. */
    u64 retry_in_ms;
};

struct NYA_DiscordGatewayOptions {
    /**
     * Required. The bot token, without the "Bot " prefix that the REST side adds.
     *
     * Copied at create and not referenced afterwards, so the caller's copy can be wiped immediately.
     * Refused when it is empty, longer than NYA_DISCORD_GATEWAY_MAX_TOKEN, or carries a byte that would
     * have to be escaped into the IDENTIFY payload — a token never does, and something that does is not
     * one.
     * */
    NYA_ConstCString token;

    /** The NYA_DiscordIntent bits this bot wants. Zero is legal and asks for nothing. */
    u32 intents;

    /** Where to dial. Null means NYA_DISCORD_GATEWAY_URL. */
    NYA_ConstCString url;

    /**
     * Which shard this connection is, of how many. Both zero is one unsharded connection.
     *
     * `shard_id` past `shard_count` is refused at create; getting it wrong on the wire is close code
     * 4010, which is fatal.
     * */
    u32 shard_id;
    u32 shard_count;

    /** Ceiling on one payload. Zero means NYA_DISCORD_GATEWAY_MAX_MESSAGE_BYTES. */
    u64 max_message_bytes;

    /**
     * Reconnect attempts before the client gives up and goes FATAL. Zero means forever, which is what a
     * bot that is meant to stay up wants.
     * */
    u32 max_reconnect_attempts;

    /** Left zeroed, the websocket client and the real clock. See NYA_DiscordGatewayTransport. */
    NYA_DiscordGatewayTransport transport;

    /** Accept any TLS certificate. Only for a test against a local gateway. */
    b8 insecure_skip_tls_verify;
};

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

// ───────────────────────────────────── LIFETIME ─────────────────────────────────────

/**
 * Validates the options, takes the client's buffers from `arena`, and starts connecting.
 *
 * NYA_ERROR_INVALID_ARGUMENT for a missing or malformed token, a shard that does not fit its count, or a
 * payload ceiling too small to hold a HELLO. A gateway that cannot be reached is not an error here; it
 * arrives later as NYA_DISCORD_GATEWAY_EVENT_DISCONNECTED.
 * */
NYA_API NYA_Error nya_discord_gateway_create(NYA_Arena* arena, NYA_DiscordGatewayOptions options, OUT NYA_DiscordGateway** out_gateway)
    __attr_no_discard;

/**
 * Closes the connection at whatever stage it reached, wipes the token, and frees the client. Null is a
 * no-op.
 *
 * Does not wait for a closing handshake: Discord treats a dropped connection as resumable, so there is
 * nothing to say goodbye for.
 * */
NYA_API void nya_discord_gateway_destroy(NYA_DiscordGateway* gateway);

// ───────────────────────────────────── OPERATIONS ─────────────────────────────────────

/**
 * Advances the connection and hands out one event, or returns false when there is nothing to report.
 *
 * This is where all of it happens: connecting, the heartbeat, IDENTIFY, RESUME, the backoff and the
 * decoding. It never blocks and does a bounded amount of work, so calling it until it returns false is
 * the intended use and cannot be stretched out by the peer.
 * */
NYA_API b8 nya_discord_gateway_poll(NYA_DiscordGateway* gateway, OUT NYA_DiscordGatewayEvent* out_event);

NYA_API NYA_DiscordGatewayState nya_discord_gateway_state(const NYA_DiscordGateway* gateway) __attr_no_discard;

/**
 * The last sequence number Discord sent, or -1 when this session has not carried one yet.
 *
 * What a RESUME replays from, and worth logging: a session whose sequence stops moving is a session that
 * is receiving nothing.
 * */
NYA_API s64 nya_discord_gateway_sequence(const NYA_DiscordGateway* gateway) __attr_no_discard;

/**
 * Sends one payload of the caller's own, such as a presence update or a guild member request.
 *
 * Refused unless the connection is logged in, because Discord closes a socket that sends anything but
 * IDENTIFY or RESUME before READY. The object is serialized into `arena`, so do not put a secret in it;
 * the token is the client's business and is never needed here.
 * */
NYA_API NYA_Error nya_discord_gateway_send(NYA_DiscordGateway* gateway, NYA_Arena* arena, const NYA_Object* payload) __attr_no_discard;

// ───────────────────────────────────── THE TABLES ─────────────────────────────────────

/**
 * What a close code obliges a client to do, the whole table in one function.
 *
 * Public because it is the piece that is dangerous to get wrong twice: a program keeping its own socket,
 * or a test, should read this table rather than write a second one that disagrees about 4004.
 * */
NYA_API NYA_DiscordGatewayCloseAction nya_discord_gateway_close_action(u16 code) __attr_no_discard;

/**
 * How long to wait before reconnect attempt `attempt`, counted from zero.
 *
 * NYA_DISCORD_GATEWAY_BACKOFF_MIN_MS doubled per attempt and held at
 * NYA_DISCORD_GATEWAY_BACKOFF_MAX_MS. Deterministic: the client jitters the result through the
 * transport's randomness, so that two bots restarted together do not come back in lockstep, and so that
 * this function stays something a test can state an answer for.
 * */
NYA_API u64 nya_discord_gateway_backoff_ms(u32 attempt) __attr_no_discard;

/** "ready", "identifying", ... for a log line. Never null. */
NYA_API NYA_ConstCString nya_discord_gateway_state_name(NYA_DiscordGatewayState state) __attr_no_discard;
