/**
 * @file twitch_eventsub.h
 *
 * A Twitch bot's ear: the EventSub socket everything that happens on a channel arrives over.
 *
 * Twitch splits a bot in two more sharply than the others do. Nothing is pushed until it has been
 * subscribed to, and a subscription is made over HTTP against a session this socket hands out, so the
 * two halves are not optional for each other: this file opens the socket and reports what comes down
 * it, and twitch_helix.h is what subscribes and what talks back.
 *
 * ```c
 * NYA_TwitchEventSub* events = nullptr;
 * NYA_EXPECT(nya_twitch_eventsub_create(arena, (NYA_TwitchEventSubOptions){ 0 }, &events));
 * defer nya_twitch_eventsub_destroy(events);
 *
 * // once a frame
 * NYA_TwitchEventSubMessage message = { 0 };
 * while (nya_twitch_eventsub_poll(events, &message)) {
 *     switch (message.kind) {
 *         // the session exists now, so this is where subscriptions are made; see twitch_helix.h
 *         case NYA_TWITCH_EVENTSUB_WELCOME:      subscribe(nya_twitch_eventsub_session(events)); break;
 *         case NYA_TWITCH_EVENTSUB_NOTIFICATION: handle(message.subscription_type, message.event);  break;
 *         case NYA_TWITCH_EVENTSUB_FATAL:        stop("%s", message.reason);                        break;
 *         default: break;
 *     }
 * }
 * ```
 *
 * ── the session, and why a welcome is not a connection ──
 *
 * Twitch answers a fresh socket with `session_welcome`, carrying a session id and how many seconds it
 * will wait between keepalives. The id is what a subscription names as its transport, so a bot that
 * subscribed against the previous session is a bot subscribed to nothing: every welcome means
 * subscribing again, including the one after a reconnect. That is why the welcome is an event a caller
 * sees rather than something handled in here — only the caller knows what it wanted to hear about.
 *
 * ── the keepalive, which is the only liveness signal there is ──
 *
 * Twitch sends `session_keepalive` whenever a quiet period passes, and says at welcome how long that
 * period is. So silence past that period plus a margin means the socket is dead even though TCP still
 * believes in it — the one failure a chat bot hits constantly and the one nothing but a timer detects.
 * Past NYA_TWITCH_EVENTSUB_GRACE_MS beyond the promised interval the socket is dropped and reopened.
 *
 * ── reconnects Twitch asks for ──
 *
 * `session_reconnect` carries a new url and means "move, you have about thirty seconds". The old
 * socket keeps delivering until the new one has been welcomed, which is Twitch's own instruction and
 * the reason a reconnect loses nothing. This client opens the new one, and closes the old the moment
 * the new one is welcomed.
 *
 * ── replays, and the id every message carries ──
 *
 * Every message has an id and a timestamp, and Twitch documents that a message may be delivered more
 * than once. A duplicate is dropped here rather than handed over twice, against a ring of the last
 * NYA_TWITCH_EVENTSUB_SEEN_MAX ids, and a message older than ten minutes is dropped whatever its id
 * says: that is Twitch's own replay window, and a notification from before it is a capture rather
 * than an event.
 *
 * Thread safety: none. One thread owns a client for its whole life.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_types.h"
#include "nyangine/http/http_websocket.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where a socket is opened when a caller names no url. */
#define NYA_TWITCH_EVENTSUB_URL "wss://eventsub.wss.twitch.tv/ws"

/** A session id, a message id or a subscription type, terminator included. */
#define NYA_TWITCH_EVENTSUB_MAX_ID 128

/** A reconnect url, terminator included. Twitch's carries the session in a query string. */
#define NYA_TWITCH_EVENTSUB_MAX_URL 512

/**
 * How long past the keepalive interval Twitch promised the socket is given before it counts as dead.
 *
 * Twitch's own keepalives are not exactly on the interval and a frame is not a precise clock, so a
 * margin of half the smallest interval it offers (ten seconds) keeps a busy machine from dropping a
 * socket that is merely late.
 * */
#define NYA_TWITCH_EVENTSUB_GRACE_MS 5000

/** The keepalive interval assumed until a welcome says otherwise. Twitch's own default. */
#define NYA_TWITCH_EVENTSUB_KEEPALIVE_S 10

/**
 * Message ids remembered, for the duplicates Twitch says it may send.
 *
 * A ring rather than a set with a time bound: a duplicate arrives close behind its original in every
 * case Twitch documents, so what matters is how many messages may pass between the two, not how long.
 * Sixty-four is a busy channel's few seconds.
 * */
#define NYA_TWITCH_EVENTSUB_SEEN_MAX 64

/** How old a message may be before it is dropped as a replay, whatever its id says. Twitch's own window. */
#define NYA_TWITCH_EVENTSUB_REPLAY_S 600

/** The first reconnect delay after a socket ends by itself, doubled per attempt up to the cap below. */
#define NYA_TWITCH_EVENTSUB_BACKOFF_MS 1000

/** The longest a reconnect waits. Past this a bot that is down stays down no longer than a minute. */
#define NYA_TWITCH_EVENTSUB_BACKOFF_MAX_MS 60000

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_TwitchEventSubKind          NYA_TwitchEventSubKind;
typedef enum NYA_TwitchEventSubState         NYA_TwitchEventSubState;
typedef struct NYA_TwitchEventSubTransport   NYA_TwitchEventSubTransport;
typedef struct NYA_TwitchEventSubMessage     NYA_TwitchEventSubMessage;
typedef struct NYA_TwitchEventSubOptions     NYA_TwitchEventSubOptions;
typedef struct NYA_TwitchEventSub            NYA_TwitchEventSub;

/** What one poll turned up. */
enum NYA_TwitchEventSubKind {
    /** Nothing. The zero, so a message nobody filled reads as no message. */
    NYA_TWITCH_EVENTSUB_NONE = 0,

    /**
     * A session exists. `session` is its id, and this is where subscriptions are made — including
     * after a reconnect, which is a new session and therefore no subscriptions at all.
     * */
    NYA_TWITCH_EVENTSUB_WELCOME,

    /** Something a subscription asked for happened. `subscription_type` says what, `event` is its payload. */
    NYA_TWITCH_EVENTSUB_NOTIFICATION,

    /** A subscription Twitch dropped, with `subscription_type` naming it. Usually a revoked token or a banned bot. */
    NYA_TWITCH_EVENTSUB_REVOKED,

    /** The socket ended and another will be opened. `retry_in_ms` says when. */
    NYA_TWITCH_EVENTSUB_DISCONNECTED,

    /** Nothing here will work: the url is wrong, or Twitch refused the socket outright. No retry follows. */
    NYA_TWITCH_EVENTSUB_FATAL,

    NYA_TWITCH_EVENTSUB_KIND_COUNT,
};

/** Where the client is. Worth logging: a bot that never leaves CONNECTING is a bot with no network. */
enum NYA_TwitchEventSubState {
    /** Nothing open and nothing waiting. What a client is before its first poll. */
    NYA_TWITCH_EVENTSUB_STATE_IDLE = 0,

    /** A socket is opening. */
    NYA_TWITCH_EVENTSUB_STATE_CONNECTING,

    /** Open, and waiting for the welcome that makes it a session. */
    NYA_TWITCH_EVENTSUB_STATE_OPENED,

    /** Welcomed. The only state in which a session id means anything. */
    NYA_TWITCH_EVENTSUB_STATE_READY,

    /** Twitch asked for a move: two sockets are open, and the old one is closed once the new is welcomed. */
    NYA_TWITCH_EVENTSUB_STATE_RECONNECTING,

    /** Waiting out a backoff before opening another socket. */
    NYA_TWITCH_EVENTSUB_STATE_WAITING,

    /** Stopped for good. See NYA_TWITCH_EVENTSUB_FATAL. */
    NYA_TWITCH_EVENTSUB_STATE_STOPPED,

    NYA_TWITCH_EVENTSUB_STATE_COUNT,
};

/**
 * How the client reaches the socket and the clock.
 *
 * Two of everything, because a reconnect has two sockets open at once: `slot` is 0 for the live one
 * and 1 for the one being moved to. A test fills this with a fake and drives a whole reconnect with no
 * network.
 * */
struct NYA_TwitchEventSubTransport {
    void* user;

    /** Starts connecting `slot` to `url`. Returns as soon as it is under way, like nya_websocket_create. */
    NYA_Error (*open)(void* user, u32 slot, NYA_ConstCString url);

    /** Tears down whatever `open` made in `slot`. Called for every ending, including the ones Twitch started. */
    void (*close)(void* user, u32 slot);

    /** One event from `slot`, or false when there is nothing. The contract nya_websocket_poll has. */
    b8 (*poll)(void* user, u32 slot, OUT NYA_WebSocketEvent* out_event);

    /** Monotonic milliseconds. Every deadline in the client is measured against this and nothing else. */
    u64 (*now_ms)(void* user);

    /** Seconds since the epoch, for the replay window. Twitch timestamps are wall clock, so this must be too. */
    u64 (*now_s)(void* user);
};

struct NYA_TwitchEventSubMessage {
    NYA_TwitchEventSubKind kind;

    /** WELCOME: the session id a subscription names. Empty on every other kind, never null. */
    NYA_ConstCString session;

    /** NOTIFICATION and REVOKED: what was subscribed to, such as "channel.chat.message". Never null. */
    NYA_ConstCString subscription_type;

    /**
     * NOTIFICATION: the event itself, parsed. Null when there was none. Valid until the next poll,
     * which is where the message arena is reset.
     * */
    const NYA_Object* event;

    /** DISCONNECTED and FATAL: why, in words. Never null, and never carrying a token. */
    NYA_ConstCString reason;

    /** DISCONNECTED: how long until the next attempt. Zero on every other kind. */
    u64 retry_in_ms;
};

struct NYA_TwitchEventSubOptions {
    /** Where the first socket is opened. Null means NYA_TWITCH_EVENTSUB_URL. */
    NYA_ConstCString url;

    /**
     * The largest message this will take, in bytes. Zero means the websocket module's own default.
     *
     * A chat message with every badge and emote is a few kilobytes; a `channel.follow` is a few
     * hundred bytes. The bound is what stops a peer making this process allocate.
     * */
    u64 max_message_bytes;

    /** For a local Twitch CLI mock, which serves ws:// with no certificate. Never true against Twitch. */
    b8 insecure_skip_tls_verify;

    /**
     * How the socket and the clocks are reached.
     *
     * Left zeroed, real websockets and the real clocks. A test fills it in and drives welcomes,
     * keepalives and reconnects without a network.
     * */
    NYA_TwitchEventSubTransport transport;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Makes a client. Nothing is opened until the first poll, so a caller decides when the network is touched. */
NYA_API NYA_Error nya_twitch_eventsub_create(NYA_Arena* arena, NYA_TwitchEventSubOptions options, OUT NYA_TwitchEventSub** out_events)
    __attr_no_discard;

/** Closes whatever is open and frees the client. Null is a no-op. */
NYA_API void nya_twitch_eventsub_destroy(NYA_TwitchEventSub* events);

/**
 * Runs the client and hands over the next thing that happened, or false when nothing did.
 *
 * This is the only function that opens sockets, answers keepalives, notices silence and reconnects, so
 * a program that stops calling it stops being a bot. Call it until it answers false.
 * */
NYA_API b8 nya_twitch_eventsub_poll(NYA_TwitchEventSub* events, OUT NYA_TwitchEventSubMessage* out_message);

/** The live session id, or an empty string when there is none. What a subscription names as its transport. */
NYA_API NYA_ConstCString nya_twitch_eventsub_session(const NYA_TwitchEventSub* events) __attr_no_discard;

NYA_API NYA_TwitchEventSubState nya_twitch_eventsub_state(const NYA_TwitchEventSub* events) __attr_no_discard;

/** "ready", "waiting", and so on: the state as a word, for a log line or an overlay. */
NYA_API NYA_ConstCString nya_twitch_eventsub_state_name(NYA_TwitchEventSubState state) __attr_no_discard;
