#include "gnyame/gnyame.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The session's table, its arena, and the two roles a caller names. */
static struct {
    NYA_Arena*       arena;
    NYA_Permissions* permissions;

    u32 officer;
    u32 host;
} GNY_GUILD = { 0 };

/**
 * The local player as a subject.
 *
 * Peer indices start at zero and so do subject ids, and zero is NYA_PERMISSION_SYSTEM, which answers
 * yes to everything. So a peer's subject id is its index plus one, and this is what the host uses
 * before any peer exists.
 * */
#define GNY_GUILD_HOST_SUBJECT 1ULL

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void gny_guild_start(void) {
    gny_guild_stop();

    GNY_GUILD.arena       = nya_arena_create(.name = "gny_guild");
    GNY_GUILD.permissions = nya_permissions_create(GNY_GUILD.arena);

    if (GNY_GUILD.permissions == nullptr) {
        nya_log_error("The guild table could not be made; nobody will be able to moderate this session.");
        return;
    }

    // everyone plays. That is the deny by default with exactly one thing allowed: being here.
    NYA_EXPECT(
        nya_permission_role_edit(GNY_GUILD.permissions, NYA_PERMISSION_SYSTEM, NYA_PERMISSION_ROLE_EVERYONE, 0, GNY_PERMISSION_PLAY,
                                 nya_clock_get_timestamp_s()),
        "while letting everyone play"
    );

    NYA_EXPECT(
        nya_permission_role_add(GNY_GUILD.permissions, NYA_PERMISSION_SYSTEM, "officer", 10,
                                GNY_PERMISSION_PLAY | GNY_PERMISSION_KICK | NYA_PERMISSION_MANAGE_SUBJECTS, nya_clock_get_timestamp_s(),
                                &GNY_GUILD.officer),
        "while adding the officer role"
    );

    NYA_EXPECT(
        nya_permission_role_add(GNY_GUILD.permissions, NYA_PERMISSION_SYSTEM, "host", 20,
                                GNY_PERMISSION_PLAY | GNY_PERMISSION_KICK | GNY_PERMISSION_MANAGE_SESSION | NYA_PERMISSION_MANAGE_ROLES |
                                    NYA_PERMISSION_MANAGE_SUBJECTS,
                                nya_clock_get_timestamp_s(), &GNY_GUILD.host),
        "while adding the host role"
    );

    // what this game's bits are called, so an editor — a UI panel or a route — can draw a row per
    // permission without being told what game it is editing.
    NYA_EXPECT(nya_permission_label_set(GNY_GUILD.permissions, GNY_PERMISSION_PLAY, "play"), "while labelling PLAY");
    NYA_EXPECT(nya_permission_label_set(GNY_GUILD.permissions, GNY_PERMISSION_KICK, "kick"), "while labelling KICK");
    NYA_EXPECT(nya_permission_label_set(GNY_GUILD.permissions, GNY_PERMISSION_MANAGE_SESSION, "manage session"), "while labelling MANAGE_SESSION");

    // the process running the session owns it. A client owns nothing: its table is the server's word
    // about what it may ask for, and the server checks again anyway.
    if (GNY_LAUNCH.role == NYA_NET_ROLE_SERVER) {
        NYA_EXPECT(
            nya_permissions_owner_set(GNY_GUILD.permissions, NYA_PERMISSION_SYSTEM, GNY_GUILD_HOST_SUBJECT, nya_clock_get_timestamp_s()),
            "while making the host the owner"
        );

        NYA_EXPECT(
            nya_permission_role_grant(GNY_GUILD.permissions, NYA_PERMISSION_SYSTEM, GNY_GUILD_HOST_SUBJECT, GNY_GUILD.host,
                                      nya_clock_get_timestamp_s()),
            "while giving the host its role"
        );
    }
}

void gny_guild_stop(void) {
    if (GNY_GUILD.arena == nullptr) return;

    // emptied before the arena goes, so anything still holding the table sees a session with nobody in
    // it rather than the last one's roles.
    if (GNY_GUILD.permissions != nullptr) nya_permissions_destroy(GNY_GUILD.permissions);

    nya_arena_destroy(GNY_GUILD.arena);

    nya_memset(&GNY_GUILD, 0, sizeof(GNY_GUILD));
}

NYA_Permissions* gny_guild(void) {
    return GNY_GUILD.permissions;
}

u64 gny_guild_subject(NYA_NetPeerId peer) {
    return (u64)peer.index + 1;
}

u64 gny_guild_local(void) {
    if (GNY_LAUNCH.role == NYA_NET_ROLE_SERVER) return GNY_GUILD_HOST_SUBJECT;

    return gny_guild_subject(nya_net_client_peer());
}

void gny_guild_join(NYA_NetPeerId peer) {
    if (GNY_GUILD.permissions == nullptr) return;

    u64 subject = gny_guild_subject(peer);

    // a joining peer holds @everyone and nothing else, which is already what an unknown subject
    // resolves to. Adding them makes them show up in a list of who is here.
    NYA_Error granted =
        nya_permission_role_grant(GNY_GUILD.permissions, NYA_PERMISSION_SYSTEM, subject, NYA_PERMISSION_ROLE_EVERYONE, nya_clock_get_timestamp_s());

    // @everyone cannot be granted, and that refusal is the right answer: they already hold it. The
    // table learns about them the first time anything else is written about them.
    nya_unused(granted);
}

b8 gny_guild_may(u64 subject, enum GnyPermission permission) {
    if (GNY_GUILD.permissions == nullptr) return false;

    return nya_permission_has(GNY_GUILD.permissions, subject, GNY_GUILD_SESSION, (NYA_Permission)permission);
}

NYA_Error gny_guild_kick(u64 actor, u64 target) {
    if (GNY_GUILD.permissions == nullptr) return nya_error(NYA_ERROR_NOT_OK, "there is no session to kick anybody from");

    if (!gny_guild_may(actor, GNY_PERMISSION_KICK)) return nya_error(NYA_ERROR_PERMISSION_DENIED, "that player may not kick");

    // and the hierarchy, which is what stops an officer from kicking the host or a fellow officer.
    if (!nya_permission_outranks(GNY_GUILD.permissions, actor, target)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "that player does not outrank the one being kicked");
    }

    if (GNY_LAUNCH.role != NYA_NET_ROLE_SERVER) return nya_error(NYA_ERROR_NOT_SUPPORTED, "only the server drops a player");

    if (target == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "there is no peer with that id");

    nya_net_server_kick((NYA_NetPeerId){ .index = (u16)(target - 1), .generation = 0 }, NYA_NET_DISCONNECT_KICKED);

    nya_log_info("Subject %llu kicked subject %llu.", (unsigned long long)actor, (unsigned long long)target);

    return NYA_OK;
}

NYA_ConstCString gny_guild_rank_name(u64 subject) {
    if (GNY_GUILD.permissions == nullptr) return "";

    u64 roles   = nya_permission_subject_roles(GNY_GUILD.permissions, subject);
    u32 highest = NYA_PERMISSION_ROLE_EVERYONE;

    for (u32 role = 0; role < nya_permission_role_count(GNY_GUILD.permissions); role++) {
        if ((roles & (1ULL << role)) == 0) continue;

        if (nya_permission_role_position(GNY_GUILD.permissions, role) >= nya_permission_role_position(GNY_GUILD.permissions, highest)) highest = role;
    }

    NYA_ConstCString name = nya_permission_role_name(GNY_GUILD.permissions, highest);

    return name != nullptr ? name : "";
}
