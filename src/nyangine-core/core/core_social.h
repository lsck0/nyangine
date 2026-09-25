/**
 * @file core_social.h
 *
 * Presence and invites, over whichever friends service is there: Discord, Steam, both or neither.
 *
 * Functions
 *
 *   nya_social_init, nya_social_deinit   brings the providers up and pumps them once a frame
 *   nya_social_available                 whether anything would be shown to anybody
 *   nya_social_presence_set / _clear     what the player is doing, and how to join them
 *   nya_social_join_reply                answers a NYA_EVENT_SOCIAL_JOIN_REQUEST
 *   nya_social_invite_open               opens the provider's own "pick a friend" dialog
 *   nya_social_user_name                 who the player is signed in as, for a settings screen
 *
 * ```c
 * NYA_EXPECT(nya_social_init(.discord_application_id = 1234567890123456789ULL, .large_image = "logo"));
 *
 * // Whenever what the player is doing changes. Cheap and idempotent, so once a frame is fine.
 * NYA_EXPECT(nya_social_presence_set((NYA_SocialPresence){
 *     .details     = "Sandbox",
 *     .state       = "Hosting",
 *     .party_size  = 2,
 *     .party_max   = 8,
 *     .join_secret = secret,           // see nya_net_config_to_join_secret
 * }));
 *
 * // And handle the two events, wherever the game handles events.
 * if (event->type == NYA_EVENT_SOCIAL_JOIN) join(event->as_social_event.secret);
 * if (event->type == NYA_EVENT_SOCIAL_JOIN_REQUEST) prompt(event->as_social_event.user_name);
 * ```
 *
 * Why it looks like this
 *
 * - One facade over two providers, because a game asking "who wants to join" does not care which
 *   service carried the question, and because every screen that shows presence would otherwise grow a
 *   branch per provider. The providers keep their own APIs for anything specific to them.
 * - Both halves degrade to nothing. No Discord running, no Steam client, or a build with neither, and
 *   every call here succeeds and does nothing. There is no error path a game has to handle for a friend
 *   service being absent, because that is not an error.
 * - It pumps itself from a NYA_EVENT_FRAME_STARTED hook rather than being a registered subsystem, so a
 *   game brings it up whenever it likes, including after the app is already running.
 * - Inbound arrives as engine events rather than a callback, so accepting a join lands in the same
 *   queue as a key press and a game can answer it at the simulation barrier like anything else.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"
#include "nyangine-core/core/core_types.h"
#include "nyangine-core/net/net_config.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How long a presence line may be, buffer included. Discord truncates at 128 bytes. */
#define NYA_SOCIAL_MAX_TEXT 160

/** How long a join secret carried through a provider may be. The launch config's own limit. */
#define NYA_SOCIAL_MAX_SECRET NYA_NET_MAX_JOIN_SECRET

/** How long a provider's user id may be. A Discord snowflake in decimal is the longest of them. */
#define NYA_SOCIAL_MAX_USER_ID 24

/**
 * How many events keep their own string storage at once.
 *
 * The strings an event carries live in this module until the next pump, and a frame that produced more
 * than this many would have the last ones overwrite the first. Eight, because a provider's own inbound
 * queue is that size and a frame cannot deliver more than one queue's worth.
 * */
#define NYA_SOCIAL_MAX_PENDING 8

/**
 * The Steam lobby key this module publishes a join secret under.
 *
 * A Steam invite carries a lobby, not a secret, so the lobby's own data table is where the address goes
 * and this is the agreed name for it. Named here rather than in a game, because both the writing side
 * (nya_social_presence_set) and the reading side (a friend's accepted invite) are in this file.
 * */
#define NYA_SOCIAL_LOBBY_KEY_JOIN "nya_join"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_SocialConfig   NYA_SocialConfig;
typedef struct NYA_SocialPresence NYA_SocialPresence;

struct NYA_SocialConfig {
    /**
     * The game's Discord application id, from the Discord developer portal. Zero leaves Discord alone,
     * which is what a game without one wants and what every test gets.
     * */
    u64 discord_application_id;

    /** The artwork key on the presence card, uploaded in that same portal. Optional. */
    NYA_ConstCString large_image;

    /** What hovering that artwork says. Optional. */
    NYA_ConstCString large_text;
};

/**
 * What the player is doing, as a friend would see it.
 *
 * Provider-neutral on purpose: Discord's two lines and Steam's localization token are different shapes
 * for the same three facts, so this carries the facts.
 * */
struct NYA_SocialPresence {
    /** The upper line. What mode or level the player is in. */
    NYA_ConstCString details;

    /** The lower line. The party, or what they are doing in it. */
    NYA_ConstCString state;

    /** Unix seconds the session started, so a friend's list can count up from it. Zero shows nothing. */
    s64 start_time_s;

    /** Groups friends into one party on a card. Optional; a party of one needs none. */
    NYA_ConstCString party_id;

    /** Both or neither: one number alone shows nothing. */
    u32 party_size;
    u32 party_max;

    /**
     * What a friend accepting an invite is handed back, or null when this session cannot be joined.
     *
     * Produce it with nya_net_config_to_join_secret and read it back with the matching parser, so the
     * only thing crossing a friend service is a launch config that was written and parsed in one place.
     * */
    NYA_ConstCString join_secret;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Brings up whichever providers this build and this machine have, and starts pumping them once a frame.
 *
 * Never fails over a provider being absent: no Discord client, no Steam client and no plugin at all are
 * all ordinary, logged once, and leave the feature off. Calling it twice is refused.
 * */
NYA_API NYA_Error nya_social_init_with_config(NYA_SocialConfig config) __attr_no_discard;

/** nya_social_init(.discord_application_id = ..., .large_image = ...). */
#define nya_social_init(...) nya_social_init_with_config((NYA_SocialConfig){ __VA_ARGS__ })

/** Clears the presence card and stops pumping. Safe when nothing was ever brought up. */
NYA_API void nya_social_deinit(void);

/** Whether any provider is connected, so a menu can hide an invite button that would do nothing. */
NYA_API b8 nya_social_available(void) __attr_no_discard;

/**
 * Sets what the player is doing, on every connected provider.
 *
 * Held rather than sent when nothing is connected yet, so a game may call it from its first frame and a
 * player who starts Discord an hour in still gets the right card.
 * */
NYA_API NYA_Error nya_social_presence_set(NYA_SocialPresence presence) __attr_no_discard;

/** Takes the card down. What returning to a launcher, or quitting, wants. */
NYA_API NYA_Error nya_social_presence_clear(void) __attr_no_discard;

/**
 * Answers a NYA_EVENT_SOCIAL_JOIN_REQUEST. `provider` and `user_id` are the ones the event carried.
 *
 * Accepting sends that person whatever the current presence's `join_secret` is, so a game that has
 * cleared it in the meantime is declining by another name; set the presence before accepting.
 * */
NYA_API NYA_Error nya_social_join_reply(NYA_SocialProvider provider, NYA_ConstCString user_id, b8 accept) __attr_no_discard;

/**
 * Opens the provider's own friend picker, which on Steam is the overlay's invite dialog.
 *
 * Creates a Steam lobby first when there is none, and opens the dialog once that lobby exists, so a
 * game can wire this straight to a button. Discord has no such dialog: a Discord invite is sent from
 * the chat window, and the presence card is what makes it possible.
 * */
NYA_API NYA_Error nya_social_invite_open(void) __attr_no_discard;

/** Who the player is signed in as, or an empty string. The first connected provider answers. */
NYA_API NYA_ConstCString nya_social_user_name(void) __attr_no_discard;

/**
 * Drains every provider and dispatches what they reported. Registered as a frame hook by init, so a
 * game does not call it; exposed because a test drives frames itself.
 * */
NYA_API void nya_social_pump(void);
