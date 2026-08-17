/**
 * @file discord.h
 *
 * Discord Rich Presence — what the player's profile says they are doing — over Discord's local IPC
 * socket.
 *
 * ```c
 * NYA_EXPECT(nya_discord_init(1234567890123456789ULL));
 *
 * // Once, or whenever what the player is doing changes. Not every frame.
 * NYA_EXPECT(nya_discord_activity_set((NYA_DiscordActivity){
 *     .details      = "Competitive | In a Match",
 *     .state        = "In a Group",
 *     .start_time_s = nya_clock_unix_seconds(),
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
 * nya_discord_deinit();
 * ```
 *
 * A plugin: nothing here is compiled unless -DNYA_PLUGIN_DISCORD is set. See plugins.h.
 *
 * ## Why this talks to a socket rather than linking Discord's SDK
 *
 * Because the SDK cannot be vendored. The Discord Social SDK is distributed only through the
 * developer portal behind a login — there is no public URL to fetch and no repository to point a
 * submodule at — and its terms forbid redistributing it: you may ship it compiled into your
 * application, and you may not "modify, create derivative works, copy, reproduce, redistribute,
 * rent, lease, sell, or syndicate access to" it. So a `vendor/discord` alongside `vendor/steam` is
 * not something this repository is allowed to contain, and a build rule that downloads it cannot
 * authenticate.
 *
 * What this module speaks instead is the protocol the SDK itself speaks when it sets presence
 * without authenticating: a local socket to the running Discord client, length-prefixed JSON. So
 * nothing about presence is given up — state, details, timestamps, both image slots, party size,
 * join and spectate secrets are all here, because they are all fields of the one `SET_ACTIVITY`
 * message the SDK also sends.
 *
 * ## What is given up
 *
 * Everything the Social SDK does that is *not* presence, all of which needs the authenticated
 * client: the friends list, cross-platform messaging, voice, lobbies, account linking, provisional
 * accounts. Also presence on consoles and iOS, which have no local Discord client to talk to, and
 * presence while Discord is closed — the authenticated path pushes that server side, this one
 * cannot.
 *
 * If any of those become real requirements, the migration is behind this API rather than through
 * it: NYA_DiscordActivity is a superset of what `SET_ACTIVITY` carries and maps field for field onto
 * the SDK's `Discord_Activity`, so swapping the transport is a new discord_sdk.c, not a new
 * interface. That is the reason the activity struct is flat data with no socket in it.
 *
 * ## It is always optional
 *
 * Discord not running, not installed, or closing mid-session is the ordinary case, not a failure.
 * Every function here is safe to call in that state and does nothing; nya_discord_init succeeds even
 * when there is nothing to connect to, and nya_discord_pump keeps trying on a backoff. A game must
 * never gate anything on this working.
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
typedef enum NYA_DiscordStatus     NYA_DiscordStatus;

/** How long a string field may be. Discord truncates at 128 bytes; this is the buffer that carries it. */
#define NYA_DISCORD_MAX_TEXT 160

/** Buttons Discord shows on a presence card. Two is its hard limit, not a choice made here. */
#define NYA_DISCORD_MAX_BUTTONS 2

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
 *
 * Plain data, copied on the way in — nothing here has to outlive the call, so a formatted string on
 * the stack is fine. Every field is optional; an all-zero activity is a valid "playing this game
 * and nothing more specific".
 *
 * Laid out to match Discord's `SET_ACTIVITY` payload field for field, which is also the shape of the
 * Social SDK's `Discord_Activity`. That is deliberate: see the note on migration in this file's
 * header.
 * */
struct NYA_DiscordActivity {
    /** The upper line. Usually what mode or level the player is in. */
    NYA_ConstCString details;

    /** The lower line. Usually the party or the current objective. */
    NYA_ConstCString state;

    /**
     * Unix seconds. Set `start_time_s` and Discord counts up from it; set `end_time_s` and it counts
     * down to it.
     *
     * Setting both is legal and Discord shows the countdown. Setting neither shows no timer, which
     * is right for a menu.
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
     *
     * `party_id` is any stable string identifying the group; both sizes must be non-zero for the
     * count to show at all, and `party_max` below `party_size` is refused by Discord.
     * */
    NYA_ConstCString party_id;
    u32              party_size;
    u32              party_max;

    /**
     * Opaque strings a friend's client hands back when they accept an invite.
     *
     * `join_secret` is what a game reads to connect the friend to this session. It is a secret in
     * the real sense — anyone holding it can join — so it must not be a lobby id in the clear.
     *
     * Sending one without a `party_id` does nothing: Discord shows no join button for a player who
     * is not in a party.
     * */
    NYA_ConstCString join_secret;
    NYA_ConstCString spectate_secret;

    /** Up to NYA_DISCORD_MAX_BUTTONS. Mutually exclusive with the secrets above, per Discord. */
    NYA_DiscordButton buttons[NYA_DISCORD_MAX_BUTTONS];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts trying to reach the local Discord client for `application_id`.
 *
 * The id is the application's, from the developer portal, and is what decides which game's name and
 * which uploaded assets the presence card shows.
 *
 * Returns success even when Discord is not running: connecting is nya_discord_pump's job and being
 * unable to is the ordinary state. An error here means something a game got wrong — a zero
 * application id, or a second init without a deinit.
 * */
NYA_API NYA_Error nya_discord_init(u64 application_id) __attr_no_discard;

/** Closes the socket and forgets the pending activity. Safe to call when nothing was ever connected. */
NYA_API void nya_discord_deinit(void);

/**
 * Drives the connection and drains whatever the client sent. Call once per frame.
 *
 * Everything in this module is non-blocking, and this is where that is paid for: connecting,
 * retrying on a backoff, sending the handshake, reading replies and re-sending the activity after a
 * reconnect all happen here. Never calling it means never connecting.
 *
 * Costs a failed `connect()` every few seconds at worst, and a read that returns nothing otherwise.
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
 *
 * Copied immediately and sent when there is a connection, so this may be called before Discord is
 * running: the activity is remembered and sent on connect, and re-sent after a reconnect. That is
 * what makes presence survive the player starting Discord after the game.
 *
 * Cheap to call repeatedly — an activity identical to the one already sent is dropped rather than
 * written to the socket, so a game may call this every frame from wherever the state actually lives
 * rather than tracking changes itself. Discord rate limits presence updates to one per fifteen
 * seconds per client and this respects that, which is the other half of why calling it often is
 * safe.
 * */
NYA_API NYA_Error nya_discord_activity_set(NYA_DiscordActivity activity) __attr_no_discard;

/** Clears the presence card. What returning to a launcher, or quitting to the desktop, wants. */
NYA_API NYA_Error nya_discord_activity_clear(void) __attr_no_discard;
