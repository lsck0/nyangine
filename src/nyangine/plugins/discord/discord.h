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
