/**
 * @file discord.h
 *
 * ```c
 * NYA_EXPECT(nya_discord_init(1234567890123456789ULL));
 *
 * // Once, or whenever what the player is doing changes. Not every frame.
 * NYA_EXPECT(nya_discord_activity_set((NYA_DiscordActivity){
 *     .details      = "Competitive | In a Match",
 *     .state        = "In a Group",
 *     .start_time_s = (s64)nya_clock_get_timestamp_s(),
 *     .large_image  = "numbani_map",
 *     .large_text   = "Numbani",
 *     .party_size   = 3,
 *     .party_max    = 6,
 *     .party_id     = "party-id",
 * }));
 *
 * // Every frame, and cheap. Drives the connect/retry state machine and drains replies.
 * nya_discord_pump();
 *
 * // Whatever the client sent this frame: an invite the player accepted, or a friend asking to join.
 * NYA_DiscordEvent event;
 * while (nya_discord_poll(&event)) {
 *     if (event.kind == NYA_DISCORD_EVENT_JOIN) join(event.secret);
 *     if (event.kind == NYA_DISCORD_EVENT_JOIN_REQUEST) ask_the_player_about(event.user_name, event.user_id);
 * }
 *
 * NYA_EXPECT(nya_discord_join_reply(user_id, true));
 *
 * nya_discord_deinit();
 * ```
 *
 * Why it looks like this
 *
 * - The socket is written to from the pump and nowhere else, so nothing a caller does can block, fail
 *   over a missing client, or half-write a frame. Presence is held and sent when the rate limit allows.
 * - Inbound events are queued and drained rather than delivered through a callback, because a join
 *   request opens a menu and a callback would do that from inside a socket read. A bounded queue also
 *   makes a client that floods requests a dropped event rather than a growing allocation.
 * - Every string the client sends is copied into a fixed buffer through a parser that rejects what it
 *   does not understand. Nothing downstream sees the raw bytes; see NYA_DiscordEvent.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_DiscordActivity NYA_DiscordActivity;
typedef struct NYA_DiscordEvent    NYA_DiscordEvent;
typedef enum NYA_DiscordStatus     NYA_DiscordStatus;
typedef enum NYA_DiscordEventKind  NYA_DiscordEventKind;

/** How long a string field may be. Discord truncates at 128 bytes; this is the buffer that carries it. */
#define NYA_DISCORD_MAX_TEXT 160

/** Buttons Discord shows on a presence card. Two is its hard limit, not a choice made here. */
#define NYA_DISCORD_MAX_BUTTONS 2

/**
 * How long a join or spectate secret may be, buffer included. Discord's own limit is 128 bytes.
 * */
#define NYA_DISCORD_MAX_SECRET 160

/**
 * How long a Discord user id may be, buffer included. A snowflake is a 64 bit number in decimal, so
 * twenty digits is the most there can ever be.
 * */
#define NYA_DISCORD_MAX_USER_ID 24

/**
 * How many inbound events are held before the oldest is dropped.
 *
 * Eight, because the queue is drained every frame and the only way to fill it is a friend list spamming
 * join requests. Dropping the oldest keeps the newest request answerable, which is the one the player is
 * looking at; the sender sees no reply and Discord expires the request on its own.
 * */
#define NYA_DISCORD_MAX_EVENTS 8

enum NYA_DiscordStatus {
    /** nya_discord_init has not been called, or deinit has. */
    NYA_DISCORD_STATUS_OFF = 0,

    /** No client found yet. Retried on a backoff; the ordinary state when Discord is not running. */
    NYA_DISCORD_STATUS_DISCONNECTED,

    /** Socket open, handshake sent, waiting for the READY that names the user. */
    NYA_DISCORD_STATUS_CONNECTING,

    /** Handshake complete. Presence set here is visible. */
    NYA_DISCORD_STATUS_CONNECTED,

    NYA_DISCORD_STATUS_COUNT,
};

/** A button on the presence card. Both fields are required or the button is dropped. */
typedef struct {
    NYA_ConstCString label;

    /** Must be http or https. Discord rejects the whole activity otherwise, not just the button. */
    NYA_ConstCString url;
} NYA_DiscordButton;

/**
 * What the player is doing, as Discord will show it.
 * */
struct NYA_DiscordActivity {
    /** The upper line. Usually what mode or level the player is in. */
    NYA_ConstCString details;

    /** The lower line. Usually the party or the current objective. */
    NYA_ConstCString state;

    /**
     * Unix seconds. Set `start_time_s` and Discord counts up from it; set `end_time_s` and it counts
     * down to it.
     * */
    s64 start_time_s;
    s64 end_time_s;

    /** Asset keys uploaded in the developer portal, or an `mp:` / external image URL. */
    NYA_ConstCString large_image;
    NYA_ConstCString large_text;
    NYA_ConstCString small_image;
    NYA_ConstCString small_text;

    /**
     * The party this player is in, which is what makes "3 of 6" appear and what an invite joins.
     * */
    NYA_ConstCString party_id;
    u32              party_size;
    u32              party_max;

    /**
     * Opaque strings a friend's client hands back when they accept an invite.
     * */
    NYA_ConstCString join_secret;
    NYA_ConstCString spectate_secret;

    /** Up to NYA_DISCORD_MAX_BUTTONS. Mutually exclusive with the secrets above, per Discord. */
    NYA_DiscordButton buttons[NYA_DISCORD_MAX_BUTTONS];
};

enum NYA_DiscordEventKind {
    NYA_DISCORD_EVENT_NONE = 0,

    /**
     * The player accepted an invite, or clicked join on a friend's card. `secret` is the `join_secret`
     * that friend's game published and nothing else has touched.
     * */
    NYA_DISCORD_EVENT_JOIN,

    /**
     * Somebody asked to join this player's game. Answer with nya_discord_join_reply; Discord expires the
     * request on its own after about thirty seconds if nobody does.
     * */
    NYA_DISCORD_EVENT_JOIN_REQUEST,

    NYA_DISCORD_EVENT_KIND_COUNT,
};

/**
 * One thing the Discord client reported, drained by nya_discord_poll.
 *
 * Every field is a fixed buffer holding a copy, not a pointer into the frame: the event outlives the read
 * that produced it, and the bytes came from another process.
 * */
struct NYA_DiscordEvent {
    NYA_DiscordEventKind kind;

    /** Decimal digits only, and only for a JOIN_REQUEST. What nya_discord_join_reply answers. */
    char user_id[NYA_DISCORD_MAX_USER_ID];

    /** The asking player's Discord name, for a JOIN_REQUEST. Empty when the client sent none. */
    char user_name[NYA_DISCORD_MAX_TEXT];

    /** What to join, for a JOIN. Printable ASCII, and whatever the inviting game put in `join_secret`. */
    char secret[NYA_DISCORD_MAX_SECRET];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts trying to reach the local Discord client for `application_id`.
 * */
NYA_API NYA_Error nya_discord_init(u64 application_id) __attr_no_discard;

/** Closes the socket and forgets the pending activity. Safe to call when nothing was ever connected. */
NYA_API void nya_discord_deinit(void);

/**
 * Drives the connection and drains whatever the client sent. Call once per frame.
 * */
NYA_API void nya_discord_pump(void);

NYA_API NYA_DiscordStatus nya_discord_status(void) __attr_no_discard;

/** Whether presence set right now would be visible. Shorthand for the status being CONNECTED. */
NYA_API b8 nya_discord_connected(void) __attr_no_discard;

/**
 * The Discord user this client is signed in as, or null until the handshake completes.
 *
 * For showing "signed in as X" in a settings screen. Owned by the module and valid until deinit.
 * */
NYA_API NYA_ConstCString nya_discord_user_name(void) __attr_no_discard;

/**
 * Sets what the player is doing.
 * */
NYA_API NYA_Error nya_discord_activity_set(NYA_DiscordActivity activity) __attr_no_discard;

/** Clears the presence card. What returning to a launcher, or quitting to the desktop, wants. */
NYA_API NYA_Error nya_discord_activity_clear(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * INBOUND
 * ─────────────────────────────────────────────────────────
 */

/**
 * Drains one queued event. False when there are none left, which is every frame in an ordinary session.
 *
 * Call it after nya_discord_pump and until it returns false, so a queue that filled empties in one frame.
 * Nothing arrives at all while the module is off or disconnected, which is what makes a player with no
 * Discord running indistinguishable from one whose friends are quiet.
 * */
NYA_API b8 nya_discord_poll(OUT NYA_DiscordEvent* out_event);

/**
 * Answers a NYA_DISCORD_EVENT_JOIN_REQUEST: accepting sends the asker an invite carrying the current
 * activity's `join_secret`, declining closes the request on their client.
 *
 * `user_id` is the one from the event. An id that is not decimal digits is refused rather than sent, and
 * so is an answer while nothing is connected, since the request it would answer cannot still be open.
 * */
NYA_API NYA_Error nya_discord_join_reply(NYA_ConstCString user_id, b8 accept) __attr_no_discard;
