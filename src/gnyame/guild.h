#pragma once

#include "nyangine-core/nyangine.h"

/**
 * @file guild.h
 *
 * Who in this session may do what, over `permission`.
 *
 * The same table answers two callers that share nothing else: the game, where the host kicks a player
 * from the pause menu, and the web interface, where the same question arrives as an HTTP request. That
 * is the point of it being here rather than inside either one — a guild rank and a route's permission
 * are the same question with different callers, so they resolve through the same table, the same
 * hierarchy and the same audit trail.
 *
 * A subject id is the peer's index as the transport numbers it, and the resource is the room: zero for
 * the session itself, which is the only room this game has so far.
 * */

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

// @flags(GnyPermission)
/** What this game's members may do. The reserved three live at the top of the word; see permission.h. */
enum GnyPermission {
    GNY_PERMISSION_NONE = 0,

    /** Move and be replicated. Everyone has it, and losing it is what being muted in a lobby means. */
    GNY_PERMISSION_PLAY = 1 << 0,

    /** Drop another player from the session. */
    GNY_PERMISSION_KICK = 1 << 1,

    /** Change what the session is running: the world, the conditions, the rate. */
    GNY_PERMISSION_MANAGE_SESSION = 1 << 2,
};

/** The whole session, as a resource id. One room for now; a lobby per world would be more of these. */
#define GNY_GUILD_SESSION 0ULL

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Builds the table: `@everyone` plays, "officer" also kicks, "host" manages the session, and whoever is
 * running this process owns it when they are the server.
 *
 * Called from gny_net_start, because the answer depends on whether this process is the server.
 * */
void gny_guild_start(void);

/** Drops the table. The arena it lives on goes with the session. */
void gny_guild_stop(void);

/** The table itself, for a caller that wants to walk the roles. Null before the session starts. */
NYA_Permissions* gny_guild(void) __attr_no_discard;

/** The subject id a peer resolves as, which is the peer's index and nothing the peer can choose. */
u64 gny_guild_subject(NYA_NetPeerId peer) __attr_no_discard;

/** Who this process is playing as, for the checks a layer makes about the player at the keyboard. */
u64 gny_guild_local(void) __attr_no_discard;

/** Adds a peer as a member, which is what joining a session means here. */
void gny_guild_join(NYA_NetPeerId peer);

/** Whether the subject may do this in the session. The one call a UI or a route asks. */
b8 gny_guild_may(u64 subject, enum GnyPermission permission) __attr_no_discard;

/**
 * Kicks `target` on `actor`'s behalf, checking the permission and the hierarchy first.
 *
 * NYA_ERROR_PERMISSION_DENIED when the actor may not kick or does not outrank the target, which is the
 * same refusal the HTTP route answers 403 with. The check is here rather than at either call site so
 * that neither can forget it.
 * */
NYA_Error gny_guild_kick(u64 actor, u64 target) __attr_no_discard;

/**
 * Which player a token is: the subject claim read as the peer's subject id.
 *
 * The program's own, because only the program knows what its ids mean. A claim that is not one of them
 * answers NYA_PERMISSION_SYSTEM, which the HTTP extractor refuses rather than obeys.
 * */
u64 gny_guild_subject_of(const NYA_HttpIdentity* identity) __attr_no_discard;

/** The name of the highest role the subject holds, for a HUD line. "@everyone" when they hold nothing else. */
NYA_ConstCString gny_guild_rank_name(u64 subject) __attr_no_discard;
