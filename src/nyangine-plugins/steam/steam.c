#include "nyangine-core/nyangine.h"

// ───────────────────────────────────── STEAM'S CALLBACK IDS AND STRUCTS ─────────────────────────────────────

// Mirrored from the C++ Steamworks SDK headers, laid out as `#pragma pack(8)` leaves them on x86-64; a static assert per size catches an SDK field move. Always compiled, so the decoder is testable with a fake client.

#define _NYA_STEAM_CALLBACK_LOBBY_INVITE         503
#define _NYA_STEAM_CALLBACK_LOBBY_ENTER          504
#define _NYA_STEAM_CALLBACK_LOBBY_DATA_UPDATE    505
#define _NYA_STEAM_CALLBACK_LOBBY_CHAT_UPDATE    506
#define _NYA_STEAM_CALLBACK_LOBBY_MATCH_LIST     510
#define _NYA_STEAM_CALLBACK_LOBBY_CREATED        513
#define _NYA_STEAM_CALLBACK_GAME_LOBBY_JOIN      333
#define _NYA_STEAM_CALLBACK_RICH_PRESENCE_JOIN   337
#define _NYA_STEAM_CALLBACK_SESSION_REQUEST      1251
#define _NYA_STEAM_CALLBACK_SESSION_FAILED       1252

/** Steam's own k_EResultOK. Everything else is a failure of one kind or another. */
#define _NYA_STEAM_RESULT_OK 1

/**
 * k_EChatMemberStateChangeEntered. The rest of the bitfield (left, disconnected, kicked, banned) all
 * mean the member is gone, so only this one bit is read.
 * */
#define _NYA_STEAM_CHAT_MEMBER_ENTERED 0x0001

typedef struct {
    u64 user;
    u64 lobby;
    u64 game;
} _NYA_SteamLobbyInvite;

typedef struct {
    u64 lobby;
    u32 chat_permissions;
    u8  locked;
    u32 response;
} _NYA_SteamLobbyEnter;

typedef struct {
    u64 lobby;
    u64 member;
    u8  success;
} _NYA_SteamLobbyDataUpdate;

typedef struct {
    u64 lobby;
    u64 user_changed;
    u64 making_change;
    u32 member_state_change;
} _NYA_SteamLobbyChatUpdate;

typedef struct {
    u32 lobbies_matching;
} _NYA_SteamLobbyMatchList;

typedef struct {
    u32 result;
    u64 lobby;
} _NYA_SteamLobbyCreated;

typedef struct {
    u64 lobby;
    u64 friend_id;
} _NYA_SteamGameLobbyJoinRequested;

/** k_cchMaxRichPresenceValueLength, which is what the SDK sizes the connect string to. */
#define _NYA_STEAM_RICH_PRESENCE_VALUE_MAX 256

typedef struct {
    u64  friend_id;
    char connect[_NYA_STEAM_RICH_PRESENCE_VALUE_MAX];
} _NYA_SteamRichPresenceJoinRequested;

/**
 * SteamNetworkingIdentity, of which only the leading type tag and the steam id are read.
 * */
typedef struct {
    u32 type;
    s32 size_bytes;
    u64 steam_id;
    u32 reserved[30];
} _NYA_SteamNetworkingIdentity;

/** k_ESteamNetworkingIdentityType_SteamID. Anything else is a peer this transport cannot address. */
#define _NYA_STEAM_IDENTITY_TYPE_STEAM_ID 16

typedef struct {
    _NYA_SteamNetworkingIdentity remote;
} _NYA_SteamSessionRequest;

// The SDK's own sizes on x86-64: a mismatch after an SDK bump is a silently misread callback that reads as a lobby id that never resolves.
static_assert(sizeof(_NYA_SteamLobbyInvite) == 24, "LobbyInvite_t is three u64");
static_assert(sizeof(_NYA_SteamLobbyEnter) == 24, "LobbyEnter_t pads the bool out to the next u32");
static_assert(sizeof(_NYA_SteamLobbyChatUpdate) == 32, "LobbyChatUpdate_t is three u64 and a u32");
static_assert(sizeof(_NYA_SteamLobbyCreated) == 16, "LobbyCreated_t pads EResult out to the u64's alignment");
static_assert(sizeof(_NYA_SteamGameLobbyJoinRequested) == 16, "GameLobbyJoinRequested_t is two CSteamID");
static_assert(sizeof(_NYA_SteamNetworkingIdentity) == 136, "SteamNetworkingIdentity reserves 32 u32 after the type tag");

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

typedef struct {
    const NYA_SteamBackend* backend;

    /** Set by a test before init; keeps nya_system_steam_init from installing the real one over it. */
    b8 backend_is_overridden;

    b8 connected;

    NYA_SteamId user;
    NYA_SteamId lobby;

    /** The last finished lobby search. Overwritten whole by the next LOBBY_LIST. */
    NYA_SteamId lobbies[NYA_STEAM_MAX_LOBBIES];
    u32         lobby_count;

    /** How many the pending search asked for, so a back end returning more is cut rather than trusted. */
    u32 lobby_list_limit;

    NYA_SteamEvent events[NYA_STEAM_MAX_EVENTS];
    u32            event_count;
    u64            events_dropped;

    /**
     * Session requests and failures, drained by the transport rather than by a game's menu. Separate
     * because both queues are drained in the same frame by different code, and one consumer taking the
     * other's events is the bug that shape invites.
     * */
    NYA_SteamEvent p2p_events[NYA_STEAM_MAX_EVENTS];
    u32            p2p_event_count;
    u64            p2p_events_dropped;

    /** Where the copies nya_steam_lobby_data_get and friends hand out live until the next call. */
    char scratch[NYA_STEAM_MAX_VALUE];
} _NYA_SteamSystem;

NYA_INTERNAL _NYA_SteamSystem _NYA_STEAM = { 0 };

NYA_INTERNAL NYA_ConstCString _NYA_STEAM_INIT_RESULT_NAME[NYA_SYSTEM_STEAM_INIT_COUNT] = {
    [NYA_SYSTEM_STEAM_INIT_OK]               = "ok",
    [NYA_SYSTEM_STEAM_INIT_FAILED_GENERIC]   = "failed",
    [NYA_SYSTEM_STEAM_INIT_NO_STEAM_CLIENT]  = "no client",
    [NYA_SYSTEM_STEAM_INIT_VERSION_MISMATCH] = "client out of date",
};

/**
 * Appends to one of the two queues, dropping the oldest when it is full.
 * */
NYA_INTERNAL void _nya_steam_queue_push(NYA_SteamEvent* queue, OUT u32* count, OUT u64* dropped, NYA_ConstCString what, NYA_SteamEvent event);

/** Takes the oldest from one of the two queues. */
NYA_INTERNAL b8 _nya_steam_queue_poll(NYA_SteamEvent* queue, OUT u32* count, OUT NYA_SteamEvent* out_event);

/** The lobby and invite queue. */
NYA_INTERNAL void _nya_steam_event_push(NYA_SteamEvent event);

/** The transport's queue. */
NYA_INTERNAL void _nya_steam_p2p_event_push(NYA_SteamEvent event);

/** Copies a string Steam handed back into the scratch buffer, bounded. Never returns null. */
NYA_INTERNAL NYA_ConstCString _nya_steam_scratch_set(NYA_ConstCString text) __attr_no_discard;

/** Whether a key or value a caller passed is one Steam will accept, which is what bounds the buffers. */
NYA_INTERNAL b8 _nya_steam_key_is_valid(NYA_ConstCString key) __attr_no_discard;

/** The error every call returns when there is no client to make it against. */
NYA_INTERNAL NYA_Error _nya_steam_unavailable(NYA_ConstCString what) __attr_no_discard;

/** The backend the build provides, or null when there is none. Defined at the bottom of this file. */
NYA_INTERNAL const NYA_SteamBackend* _nya_steam_backend_default(void) __attr_no_discard;

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

b8 nya_steam_id_is_set(NYA_SteamId id) {
    return id.value != 0;
}

b8 nya_steam_id_equals(NYA_SteamId a, NYA_SteamId b) {
    return a.value == b.value;
}

// ───────────────────────────────────── SYSTEM FUNCTIONS ─────────────────────────────────────

void nya_steam_backend_set(const NYA_SteamBackend* backend) {
    nya_assert(!_NYA_STEAM.connected, "the Steam backend cannot be swapped while a client is connected");

    _NYA_STEAM.backend               = backend;
    _NYA_STEAM.backend_is_overridden = backend != nullptr;
}

b8 nya_system_steam_restart_if_necessary(u32 app_id) {
    nya_assert(app_id != 0);

    const NYA_SteamBackend* backend = _NYA_STEAM.backend_is_overridden ? _NYA_STEAM.backend : _nya_steam_backend_default();

    // No library to ask, so nothing relaunches and the game carries on — the branch a debug build takes, and why gnyame passes its app id unconditionally.
    if (backend == nullptr || backend->restart_if_necessary == nullptr) return false;

    return backend->restart_if_necessary(app_id);
}

NYA_SteamInitResult nya_system_steam_init(void) {
    nya_assert(!_NYA_STEAM.connected, "nya_system_steam_init called twice without a deinit");

    if (!_NYA_STEAM.backend_is_overridden) _NYA_STEAM.backend = _nya_steam_backend_default();

    if (_NYA_STEAM.backend == nullptr || _NYA_STEAM.backend->connect == nullptr) {
        // Once, at info: a build without the Steamworks library is ordinary for every target but the two Steam ones, and no player can fix it.
        nya_log_info("This build has no Steamworks library; Steam lobbies, invites, achievements and Cloud are off.");
        return NYA_SYSTEM_STEAM_INIT_NO_STEAM_CLIENT;
    }

    char message[1024] = { 0 };

    NYA_SteamInitResult result = _NYA_STEAM.backend->connect(message, sizeof(message));

    // A result this SDK does not name reads as a generic failure rather than indexing past the table.
    if ((u32)result >= NYA_SYSTEM_STEAM_INIT_COUNT) result = NYA_SYSTEM_STEAM_INIT_FAILED_GENERIC;

    if (result != NYA_SYSTEM_STEAM_INIT_OK) {
        nya_log_warn("Steam is unavailable (%s), continuing without it: %s", _NYA_STEAM_INIT_RESULT_NAME[result], message);
        return result;
    }

    _NYA_STEAM.connected      = true;
    _NYA_STEAM.user           = (NYA_SteamId){ .value = _NYA_STEAM.backend->user_id != nullptr ? _NYA_STEAM.backend->user_id() : 0 };
    _NYA_STEAM.lobby          = NYA_STEAM_ID_NONE;
    _NYA_STEAM.lobby_count    = 0;
    _NYA_STEAM.event_count    = 0;
    _NYA_STEAM.events_dropped = 0;

    nya_log_info("Connected to Steam as '%s' (%llu).", nya_steam_user_name(), (unsigned long long)_NYA_STEAM.user.value);

    return NYA_SYSTEM_STEAM_INIT_OK;
}

void nya_system_steam_update(void) {
    if (!_NYA_STEAM.connected) return;

    nya_assert(_NYA_STEAM.backend != nullptr);

    if (_NYA_STEAM.backend->run_callbacks != nullptr) _NYA_STEAM.backend->run_callbacks();
}

void nya_system_steam_deinit(void) {
    if (!_NYA_STEAM.connected) return;

    nya_assert(_NYA_STEAM.backend != nullptr);

    // Before the connection goes: leaving tells the other members, where closing the pipe would leave this player in the lobby until Steam times them out.
    nya_steam_lobby_leave();

    if (_NYA_STEAM.backend->disconnect != nullptr) _NYA_STEAM.backend->disconnect();

    const NYA_SteamBackend* backend    = _NYA_STEAM.backend;
    b8                      overridden = _NYA_STEAM.backend_is_overridden;

    _NYA_STEAM = (_NYA_SteamSystem){ 0 };

    // A test's fake survives a deinit, so a test can bring the module up twice against it.
    if (overridden) {
        _NYA_STEAM.backend               = backend;
        _NYA_STEAM.backend_is_overridden = true;
    }
}

// ───────────────────────────────────── THE CONNECTION ─────────────────────────────────────

b8 nya_steam_is_connected(void) {
    return _NYA_STEAM.connected;
}

NYA_SteamId nya_steam_user_id(void) {
    return _NYA_STEAM.connected ? _NYA_STEAM.user : NYA_STEAM_ID_NONE;
}

NYA_ConstCString nya_steam_user_name(void) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->user_name == nullptr) return "";

    return _nya_steam_scratch_set(_NYA_STEAM.backend->user_name());
}

NYA_ConstCString nya_steam_friend_name(NYA_SteamId user) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->friend_name == nullptr) return "";
    if (!nya_steam_id_is_set(user)) return "";

    return _nya_steam_scratch_set(_NYA_STEAM.backend->friend_name(user.value));
}

b8 nya_steam_poll(OUT NYA_SteamEvent* out_event) {
    return _nya_steam_queue_poll(_NYA_STEAM.events, &_NYA_STEAM.event_count, out_event);
}

// ───────────────────────────────────── LOBBIES ─────────────────────────────────────

NYA_Error nya_steam_lobby_create(NYA_SteamLobbyKind kind, u32 max_members) {
    nya_assert(kind < NYA_STEAM_LOBBY_KIND_COUNT);

    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_create == nullptr) return _nya_steam_unavailable("create a lobby");

    // Clamped rather than refused: 250 is Steam's ceiling, and a game asking for more has a bug that should not cost the player their session.
    u32 members = nya_clamp(max_members, 1U, 250U);

    if (members != max_members) nya_log_warn("A Steam lobby holds 1 to 250 members; %u became %u.", max_members, members);

    if (!_NYA_STEAM.backend->lobby_create((u32)kind, members)) return nya_error(NYA_ERROR_NOT_OK, "Steam refused to start creating a lobby");

    return NYA_OK;
}

NYA_Error nya_steam_lobby_join(NYA_SteamId lobby) {
    if (!nya_steam_id_is_set(lobby)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Steam lobby id of zero");
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_join == nullptr) return _nya_steam_unavailable("join a lobby");

    // The old one first: Steam allows being in several, but this module tracks one, so a second join would make `current` disagree with the player.
    if (nya_steam_id_is_set(_NYA_STEAM.lobby) && !nya_steam_id_equals(_NYA_STEAM.lobby, lobby)) nya_steam_lobby_leave();

    if (!_NYA_STEAM.backend->lobby_join(lobby.value)) return nya_error(NYA_ERROR_NOT_OK, "Steam refused to start joining the lobby");

    return NYA_OK;
}

void nya_steam_lobby_leave(void) {
    if (!_NYA_STEAM.connected) return;
    if (!nya_steam_id_is_set(_NYA_STEAM.lobby)) return;

    if (_NYA_STEAM.backend->lobby_leave != nullptr) _NYA_STEAM.backend->lobby_leave(_NYA_STEAM.lobby.value);

    _NYA_STEAM.lobby = NYA_STEAM_ID_NONE;
}

NYA_SteamId nya_steam_lobby_current(void) {
    return _NYA_STEAM.connected ? _NYA_STEAM.lobby : NYA_STEAM_ID_NONE;
}

NYA_Error nya_steam_lobby_list_request(NYA_ConstCString key, NYA_ConstCString value, u32 max_results) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_list_request == nullptr) return _nya_steam_unavailable("search for lobbies");

    // Both or neither: a key with no value filters on the empty string, which matches nothing and reads as a broken search.
    b8 filtered = key != nullptr && key[0] != '\0';

    if (filtered && (value == nullptr || value[0] == '\0')) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a lobby search key with no value");
    if (filtered && !_nya_steam_key_is_valid(key)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a lobby search key longer than Steam accepts");

    u32 limit = max_results == 0 ? NYA_STEAM_MAX_LOBBIES : nya_min(max_results, (u32)NYA_STEAM_MAX_LOBBIES);

    _NYA_STEAM.lobby_list_limit = limit;

    if (!_NYA_STEAM.backend->lobby_list_request(filtered ? key : nullptr, filtered ? value : nullptr, limit)) {
        return nya_error(NYA_ERROR_NOT_OK, "Steam refused to start a lobby search");
    }

    return NYA_OK;
}

u32 nya_steam_lobby_list_count(void) {
    return _NYA_STEAM.connected ? _NYA_STEAM.lobby_count : 0;
}

NYA_SteamId nya_steam_lobby_list_at(u32 index) {
    if (!_NYA_STEAM.connected) return NYA_STEAM_ID_NONE;

    // An index past the end is a caller reading a list that shrank between frames — an ordinary race, not a bug — so it answers nothing instead of asserting.
    if (index >= _NYA_STEAM.lobby_count) return NYA_STEAM_ID_NONE;

    return _NYA_STEAM.lobbies[index];
}

NYA_ConstCString nya_steam_lobby_data_get(NYA_SteamId lobby, NYA_ConstCString key) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_data_get == nullptr) return "";
    if (!nya_steam_id_is_set(lobby) || !_nya_steam_key_is_valid(key)) return "";

    return _nya_steam_scratch_set(_NYA_STEAM.backend->lobby_data_get(lobby.value, key));
}

NYA_Error nya_steam_lobby_data_set(NYA_ConstCString key, NYA_ConstCString value) {
    if (!_nya_steam_key_is_valid(key)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Steam lobby key that is empty or too long");
    if (value == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a null Steam lobby value");

    if (strnlen(value, NYA_STEAM_MAX_VALUE) >= NYA_STEAM_MAX_VALUE) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Steam lobby value longer than %d bytes", NYA_STEAM_MAX_VALUE);
    }

    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_data_set == nullptr) return _nya_steam_unavailable("write lobby data");
    if (!nya_steam_id_is_set(_NYA_STEAM.lobby)) return nya_error(NYA_ERROR_NOT_FOUND, "not in a Steam lobby");

    if (!_NYA_STEAM.backend->lobby_data_set(_NYA_STEAM.lobby.value, key, value)) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "Steam refused the lobby data write; only the owner may");
    }

    return NYA_OK;
}

u32 nya_steam_lobby_member_count(NYA_SteamId lobby) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_member_count == nullptr) return 0;
    if (!nya_steam_id_is_set(lobby)) return 0;

    return _NYA_STEAM.backend->lobby_member_count(lobby.value);
}

NYA_SteamId nya_steam_lobby_member_at(NYA_SteamId lobby, u32 index) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_member_at == nullptr) return NYA_STEAM_ID_NONE;
    if (!nya_steam_id_is_set(lobby)) return NYA_STEAM_ID_NONE;
    if (index >= nya_steam_lobby_member_count(lobby)) return NYA_STEAM_ID_NONE;

    return (NYA_SteamId){ .value = _NYA_STEAM.backend->lobby_member_at(lobby.value, index) };
}

NYA_SteamId nya_steam_lobby_owner(NYA_SteamId lobby) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_owner == nullptr) return NYA_STEAM_ID_NONE;
    if (!nya_steam_id_is_set(lobby)) return NYA_STEAM_ID_NONE;

    return (NYA_SteamId){ .value = _NYA_STEAM.backend->lobby_owner(lobby.value) };
}

u32 nya_steam_lobby_member_limit(NYA_SteamId lobby) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_member_limit == nullptr) return 0;
    if (!nya_steam_id_is_set(lobby)) return 0;

    return _NYA_STEAM.backend->lobby_member_limit(lobby.value);
}

NYA_ConstCString nya_steam_lobby_member_data_get(NYA_SteamId lobby, NYA_SteamId user, NYA_ConstCString key) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_member_data_get == nullptr) return "";
    if (!nya_steam_id_is_set(lobby) || !nya_steam_id_is_set(user) || !_nya_steam_key_is_valid(key)) return "";

    return _nya_steam_scratch_set(_NYA_STEAM.backend->lobby_member_data_get(lobby.value, user.value, key));
}

NYA_Error nya_steam_lobby_member_data_set(NYA_ConstCString key, NYA_ConstCString value) {
    if (!_nya_steam_key_is_valid(key)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Steam lobby key that is empty or too long");
    if (value == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a null Steam lobby value");

    if (strnlen(value, NYA_STEAM_MAX_VALUE) >= NYA_STEAM_MAX_VALUE) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Steam lobby value longer than %d bytes", NYA_STEAM_MAX_VALUE);
    }

    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_member_data_set == nullptr) return _nya_steam_unavailable("write lobby member data");
    if (!nya_steam_id_is_set(_NYA_STEAM.lobby)) return nya_error(NYA_ERROR_NOT_FOUND, "not in a Steam lobby");

    // Steam's setter has no return; it queues the write and reports it as a LOBBY_DATA_CHANGED.
    _NYA_STEAM.backend->lobby_member_data_set(_NYA_STEAM.lobby.value, key, value);

    return NYA_OK;
}

NYA_Error nya_steam_lobby_invite(NYA_SteamId user) {
    if (!nya_steam_id_is_set(user)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Steam user id of zero");
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->lobby_invite == nullptr) return _nya_steam_unavailable("invite a friend");
    if (!nya_steam_id_is_set(_NYA_STEAM.lobby)) return nya_error(NYA_ERROR_NOT_FOUND, "not in a Steam lobby to invite anybody to");

    if (!_NYA_STEAM.backend->lobby_invite(_NYA_STEAM.lobby.value, user.value)) return nya_error(NYA_ERROR_NOT_OK, "Steam refused the invite");

    return NYA_OK;
}

NYA_Error nya_steam_lobby_invite_open(void) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->overlay_invite_open == nullptr) return _nya_steam_unavailable("open the Steam overlay");
    if (!nya_steam_id_is_set(_NYA_STEAM.lobby)) return nya_error(NYA_ERROR_NOT_FOUND, "not in a Steam lobby to invite anybody to");

    if (!_NYA_STEAM.backend->overlay_invite_open(_NYA_STEAM.lobby.value)) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "the Steam overlay is not available; invite a friend by id instead");
    }

    return NYA_OK;
}

// ───────────────────────────────────── ACHIEVEMENTS AND STATS ─────────────────────────────────────

b8 nya_steam_achievement_get(NYA_ConstCString name) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->achievement_get == nullptr) return false;
    if (!_nya_steam_key_is_valid(name)) return false;

    b8 unlocked = false;

    if (!_NYA_STEAM.backend->achievement_get(name, &unlocked)) return false;

    return unlocked;
}

NYA_Error nya_steam_achievement_set(NYA_ConstCString name) {
    if (!_nya_steam_key_is_valid(name)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an achievement name that is empty or too long");
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->achievement_set == nullptr) return _nya_steam_unavailable("unlock an achievement");

    if (!_NYA_STEAM.backend->achievement_set(name, true)) return nya_error(NYA_ERROR_NOT_FOUND, "Steam does not know the achievement '%s'", name);

    return NYA_OK;
}

NYA_Error nya_steam_achievement_clear(NYA_ConstCString name) {
    if (!_nya_steam_key_is_valid(name)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an achievement name that is empty or too long");
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->achievement_set == nullptr) return _nya_steam_unavailable("clear an achievement");

    if (!_NYA_STEAM.backend->achievement_set(name, false)) return nya_error(NYA_ERROR_NOT_FOUND, "Steam does not know the achievement '%s'", name);

    return NYA_OK;
}

NYA_Error nya_steam_achievement_progress(NYA_ConstCString name, u32 current, u32 max) {
    if (!_nya_steam_key_is_valid(name)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an achievement name that is empty or too long");

    // Steam refuses the call outright at or past the maximum, where the unlock is the right message.
    if (max == 0 || current >= max) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "achievement progress %u of %u; unlock it instead", current, max);

    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->achievement_progress == nullptr) return _nya_steam_unavailable("show achievement progress");

    if (!_NYA_STEAM.backend->achievement_progress(name, current, max)) {
        return nya_error(NYA_ERROR_NOT_FOUND, "Steam does not know the achievement '%s'", name);
    }

    return NYA_OK;
}

s32 nya_steam_stat_get_int(NYA_ConstCString name) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->stat_get_int == nullptr) return 0;
    if (!_nya_steam_key_is_valid(name)) return 0;

    s32 value = 0;

    if (!_NYA_STEAM.backend->stat_get_int(name, &value)) return 0;

    return value;
}

NYA_Error nya_steam_stat_set_int(NYA_ConstCString name, s32 value) {
    if (!_nya_steam_key_is_valid(name)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a stat name that is empty or too long");
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->stat_set_int == nullptr) return _nya_steam_unavailable("set a stat");

    if (!_NYA_STEAM.backend->stat_set_int(name, value)) return nya_error(NYA_ERROR_NOT_FOUND, "Steam does not know the integer stat '%s'", name);

    return NYA_OK;
}

f32 nya_steam_stat_get_float(NYA_ConstCString name) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->stat_get_float == nullptr) return 0.0F;
    if (!_nya_steam_key_is_valid(name)) return 0.0F;

    f32 value = 0.0F;

    if (!_NYA_STEAM.backend->stat_get_float(name, &value)) return 0.0F;

    return value;
}

NYA_Error nya_steam_stat_set_float(NYA_ConstCString name, f32 value) {
    if (!_nya_steam_key_is_valid(name)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a stat name that is empty or too long");

    // A NaN stat comes back as NaN forever with no way to clear it from the client, so it is refused rather than written once and regretted.
    if (isnan((f64)value) || isinf((f64)value)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a stat value that is not a finite number");

    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->stat_set_float == nullptr) return _nya_steam_unavailable("set a stat");

    if (!_NYA_STEAM.backend->stat_set_float(name, value)) return nya_error(NYA_ERROR_NOT_FOUND, "Steam does not know the float stat '%s'", name);

    return NYA_OK;
}

NYA_Error nya_steam_stats_store(void) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->stats_store == nullptr) return _nya_steam_unavailable("store stats");

    if (!_NYA_STEAM.backend->stats_store()) return nya_error(NYA_ERROR_NOT_OK, "Steam refused to store the stats");

    return NYA_OK;
}

// ───────────────────────────────────── CLOUD ─────────────────────────────────────

b8 nya_steam_cloud_enabled(void) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->cloud_enabled == nullptr) return false;

    return _NYA_STEAM.backend->cloud_enabled();
}

b8 nya_steam_cloud_quota(OUT u64* out_total_bytes, OUT u64* out_available_bytes) {
    nya_assert(out_total_bytes != nullptr);
    nya_assert(out_available_bytes != nullptr);

    *out_total_bytes     = 0;
    *out_available_bytes = 0;

    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->cloud_quota == nullptr) return false;

    return _NYA_STEAM.backend->cloud_quota(out_total_bytes, out_available_bytes);
}

b8 nya_steam_cloud_exists(NYA_ConstCString name) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->cloud_exists == nullptr) return false;
    if (!_nya_steam_key_is_valid(name)) return false;

    return _NYA_STEAM.backend->cloud_exists(name);
}

u64 nya_steam_cloud_size(NYA_ConstCString name) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->cloud_size == nullptr) return 0;
    if (!_nya_steam_key_is_valid(name)) return 0;

    return _NYA_STEAM.backend->cloud_size(name);
}

NYA_Error nya_steam_cloud_write(NYA_ConstCString name, const u8* data, u64 size) {
    if (!_nya_steam_key_is_valid(name)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Cloud file name that is empty or too long");
    if (data == nullptr && size != 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Cloud write with no data");

    // Steam's own limit on one file; refused rather than split, since a game that hit it wants to know.
    if (size > (u64)INT32_MAX) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Cloud file larger than Steam accepts");

    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->cloud_write == nullptr) return _nya_steam_unavailable("write to the Steam Cloud");
    if (!nya_steam_cloud_enabled()) return nya_error(NYA_ERROR_PERMISSION_DENIED, "Steam Cloud is turned off for this account or this game");

    if (!_NYA_STEAM.backend->cloud_write(name, data, (u32)size)) return nya_error(NYA_ERROR_IO, "the Steam Cloud write failed; the quota may be full");

    return NYA_OK;
}

NYA_Error nya_steam_cloud_read(NYA_ConstCString name, OUT u8* out_data, u64 capacity, OUT u64* out_size) {
    nya_assert(out_data != nullptr);
    nya_assert(out_size != nullptr);

    *out_size = 0;

    if (!_nya_steam_key_is_valid(name)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Cloud file name that is empty or too long");
    if (capacity == 0 || capacity > (u64)INT32_MAX) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Cloud read buffer Steam cannot fill");

    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->cloud_read == nullptr) return _nya_steam_unavailable("read from the Steam Cloud");
    if (!_NYA_STEAM.backend->cloud_exists(name)) return nya_error(NYA_ERROR_NOT_FOUND, "'%s' is not in the Steam Cloud", name);

    // The size before the read: the file was written on another machine, maybe a later version, and a truncated save is worse than no save.
    u64 size = _NYA_STEAM.backend->cloud_size(name);

    if (size > capacity) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "'%s' is %llu bytes and the buffer holds %llu", name, (unsigned long long)size,
                                          (unsigned long long)capacity);

    s32 read = _NYA_STEAM.backend->cloud_read(name, out_data, (u32)capacity);

    if (read < 0) return nya_error(NYA_ERROR_IO, "the Steam Cloud read failed");

    // A short read means the file changed between the size and the read, which Steam allows.
    *out_size = (u64)read;

    return NYA_OK;
}

NYA_Error nya_steam_cloud_delete(NYA_ConstCString name) {
    if (!_nya_steam_key_is_valid(name)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Cloud file name that is empty or too long");
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->cloud_delete == nullptr) return _nya_steam_unavailable("delete from the Steam Cloud");

    // Not an error: a teardown deleting a file that was never written should not have to check first.
    if (!_NYA_STEAM.backend->cloud_exists(name)) return NYA_OK;

    if (!_NYA_STEAM.backend->cloud_delete(name)) return nya_error(NYA_ERROR_IO, "the Steam Cloud delete failed");

    return NYA_OK;
}

// ───────────────────────────────────── RICH PRESENCE ─────────────────────────────────────

NYA_Error nya_steam_rich_presence_set(NYA_ConstCString key, NYA_ConstCString value) {
    if (!_nya_steam_key_is_valid(key)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a rich presence key that is empty or too long");
    if (value == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a null rich presence value");

    if (strnlen(value, NYA_STEAM_MAX_CONNECT) >= NYA_STEAM_MAX_CONNECT) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a rich presence value longer than %d bytes", NYA_STEAM_MAX_CONNECT);
    }

    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->rich_presence_set == nullptr) return _nya_steam_unavailable("set rich presence");

    if (!_NYA_STEAM.backend->rich_presence_set(key, value)) return nya_error(NYA_ERROR_NOT_OK, "Steam refused the rich presence key '%s'", key);

    return NYA_OK;
}

void nya_steam_rich_presence_clear(void) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->rich_presence_clear == nullptr) return;

    _NYA_STEAM.backend->rich_presence_clear();
}

// ───────────────────────────────────── PEER TO PEER ─────────────────────────────────────

NYA_Error nya_steam_p2p_send(NYA_SteamId user, const u8* data, u64 size, b8 reliable, u32 channel) {
    if (!nya_steam_id_is_set(user)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Steam user id of zero");
    if (data == nullptr || size == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an empty peer to peer message");

    // The transport fragments above this, so anything larger is a caller that did not.
    if (size > NYA_STEAM_MAX_MESSAGE) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a %llu byte message; the limit is %d", (unsigned long long)size,
                                                       NYA_STEAM_MAX_MESSAGE);

    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->p2p_send == nullptr) return _nya_steam_unavailable("send to a peer");

    if (!_NYA_STEAM.backend->p2p_send(user.value, data, (u32)size, reliable, channel)) {
        return nya_error(NYA_ERROR_NOT_OK, "Steam could not deliver to %llu", (unsigned long long)user.value);
    }

    return NYA_OK;
}

u32 nya_steam_p2p_receive(u32 channel, OUT NYA_SteamMessage* out_messages, u32 capacity) {
    nya_assert(out_messages != nullptr);
    nya_assert(capacity > 0);

    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->p2p_receive == nullptr) return 0;

    u32 taken = _NYA_STEAM.backend->p2p_receive(channel, out_messages, nya_min(capacity, (u32)NYA_STEAM_MAX_RECEIVE));

    // A backend claiming more than it was asked for wrote past the caller's array, so this is checked, not trusted: the real one is a shared library.
    nya_assert(taken <= capacity, "the %s backend returned %u messages for a buffer of %u", _NYA_STEAM.backend->name, taken, capacity);

    return taken;
}

b8 nya_steam_p2p_poll(OUT NYA_SteamEvent* out_event) {
    return _nya_steam_queue_poll(_NYA_STEAM.p2p_events, &_NYA_STEAM.p2p_event_count, out_event);
}

NYA_Error nya_steam_p2p_accept(NYA_SteamId user) {
    if (!nya_steam_id_is_set(user)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Steam user id of zero");
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->p2p_accept == nullptr) return _nya_steam_unavailable("accept a peer");

    if (!_NYA_STEAM.backend->p2p_accept(user.value)) return nya_error(NYA_ERROR_NOT_OK, "Steam refused the session with %llu",
                                                                     (unsigned long long)user.value);

    return NYA_OK;
}

void nya_steam_p2p_close(NYA_SteamId user) {
    if (!_NYA_STEAM.connected || _NYA_STEAM.backend->p2p_close == nullptr) return;
    if (!nya_steam_id_is_set(user)) return;

    _NYA_STEAM.backend->p2p_close(user.value);
}

// ───────────────────────────────────── THE CALLBACK DECODER ─────────────────────────────────────

void nya_steam_on_callback(u32 callback_id, const void* data, u32 size) {
    // Every callback is size-checked before a field is read: Steam's structs are a runtime ABI, so a short buffer is a version mismatch, dropped rather than asserted on.
    if (data == nullptr) return;

    switch (callback_id) {
        case _NYA_STEAM_CALLBACK_LOBBY_CREATED: {
            if (size < sizeof(_NYA_SteamLobbyCreated)) break;

            const _NYA_SteamLobbyCreated* created = data;

            if (created->result != _NYA_STEAM_RESULT_OK || created->lobby == 0) {
                _nya_steam_event_push((NYA_SteamEvent){ .kind = NYA_STEAM_EVENT_LOBBY_FAILED, .reason = created->result });
                break;
            }

            // No LOBBY_ENTERED here: Steam sends a LobbyEnter_t for a created lobby too, and raising both would push the lobby screen twice.
            _NYA_STEAM.lobby = (NYA_SteamId){ .value = created->lobby };
        } break;

        case _NYA_STEAM_CALLBACK_LOBBY_ENTER: {
            if (size < sizeof(_NYA_SteamLobbyEnter)) break;

            const _NYA_SteamLobbyEnter* entered = data;

            if (entered->lobby == 0) break;

            // k_EChatRoomEnterResponseSuccess is 1; everything else (full, banned, gone, limited account) means this player is not in it.
            if (entered->response != 1) {
                _NYA_STEAM.lobby = NYA_STEAM_ID_NONE;
                _nya_steam_event_push((NYA_SteamEvent){ .kind = NYA_STEAM_EVENT_LOBBY_FAILED, .lobby = { .value = entered->lobby }, .reason = entered->response });
                break;
            }

            _NYA_STEAM.lobby = (NYA_SteamId){ .value = entered->lobby };
            _nya_steam_event_push((NYA_SteamEvent){ .kind = NYA_STEAM_EVENT_LOBBY_ENTERED, .lobby = _NYA_STEAM.lobby });
        } break;

        case _NYA_STEAM_CALLBACK_LOBBY_CHAT_UPDATE: {
            if (size < sizeof(_NYA_SteamLobbyChatUpdate)) break;

            const _NYA_SteamLobbyChatUpdate* update = data;

            _nya_steam_event_push((NYA_SteamEvent){
                .kind   = NYA_STEAM_EVENT_LOBBY_MEMBER_CHANGED,
                .lobby  = { .value = update->lobby },
                .user   = { .value = update->user_changed },
                .reason = update->member_state_change,
            });

            // This player was the one who left (kick, ban and leave all arrive here and end the session the same way), so their lobby is now none.
            b8 entered = (update->member_state_change & _NYA_STEAM_CHAT_MEMBER_ENTERED) != 0;

            if (!entered && update->user_changed == _NYA_STEAM.user.value && update->lobby == _NYA_STEAM.lobby.value) {
                _NYA_STEAM.lobby = NYA_STEAM_ID_NONE;
            }
        } break;

        case _NYA_STEAM_CALLBACK_LOBBY_DATA_UPDATE: {
            if (size < sizeof(_NYA_SteamLobbyDataUpdate)) break;

            const _NYA_SteamLobbyDataUpdate* update = data;

            // A failed update is Steam saying the lobby is gone, which the member list already reports.
            if (update->success == 0) break;

            _nya_steam_event_push((NYA_SteamEvent){
                .kind  = NYA_STEAM_EVENT_LOBBY_DATA_CHANGED,
                .lobby = { .value = update->lobby },
                .user  = { .value = update->member },
            });
        } break;

        case _NYA_STEAM_CALLBACK_LOBBY_MATCH_LIST: {
            if (size < sizeof(_NYA_SteamLobbyMatchList)) break;

            const _NYA_SteamLobbyMatchList* list = data;

            u32 limit = _NYA_STEAM.lobby_list_limit == 0 ? NYA_STEAM_MAX_LOBBIES : _NYA_STEAM.lobby_list_limit;
            u32 count = nya_min(list->lobbies_matching, limit);

            if (list->lobbies_matching > count) {
                nya_log_warn("Steam returned %u lobbies; keeping the first %u.", list->lobbies_matching, count);
            }

            _NYA_STEAM.lobby_count = 0;

            for (u32 i = 0; i < count; i++) {
                if (_NYA_STEAM.backend == nullptr || _NYA_STEAM.backend->lobby_list_at == nullptr) break;

                u64 lobby = _NYA_STEAM.backend->lobby_list_at(i);

                // A zero id mid-list is a row Steam could not resolve; skipped, so the list a game shows has no unjoinable holes.
                if (lobby == 0) continue;

                _NYA_STEAM.lobbies[_NYA_STEAM.lobby_count] = (NYA_SteamId){ .value = lobby };
                _NYA_STEAM.lobby_count++;
            }

            _nya_steam_event_push((NYA_SteamEvent){ .kind = NYA_STEAM_EVENT_LOBBY_LIST });
        } break;

        case _NYA_STEAM_CALLBACK_LOBBY_INVITE: {
            if (size < sizeof(_NYA_SteamLobbyInvite)) break;

            const _NYA_SteamLobbyInvite* invite = data;

            // An invite not yet accepted, raised as a join request so a game can show its own prompt; accepting from the overlay produces a GameLobbyJoinRequested.
            _nya_steam_event_push((NYA_SteamEvent){
                .kind  = NYA_STEAM_EVENT_JOIN_REQUESTED,
                .lobby = { .value = invite->lobby },
                .user  = { .value = invite->user },
            });
        } break;

        case _NYA_STEAM_CALLBACK_GAME_LOBBY_JOIN: {
            if (size < sizeof(_NYA_SteamGameLobbyJoinRequested)) break;

            const _NYA_SteamGameLobbyJoinRequested* request = data;

            if (request->lobby == 0) break;

            _nya_steam_event_push((NYA_SteamEvent){
                .kind  = NYA_STEAM_EVENT_JOIN_REQUESTED,
                .lobby = { .value = request->lobby },
                .user  = { .value = request->friend_id },
            });
        } break;

        case _NYA_STEAM_CALLBACK_RICH_PRESENCE_JOIN: {
            if (size < sizeof(_NYA_SteamRichPresenceJoinRequested)) break;

            const _NYA_SteamRichPresenceJoinRequested* request = data;

            NYA_SteamEvent event = { .kind = NYA_STEAM_EVENT_JOIN_REQUESTED, .user = { .value = request->friend_id } };

            /* The connect string came from another player's client and may not be terminated. */
            u64 length = strnlen(request->connect, sizeof(request->connect));

            if (length == 0 || length >= sizeof(event.secret)) break;

            nya_memcpy(event.secret, request->connect, length);
            event.secret[length] = '\0';

            _nya_steam_event_push(event);
        } break;

        case _NYA_STEAM_CALLBACK_SESSION_REQUEST: {
            if (size < sizeof(_NYA_SteamSessionRequest)) break;

            const _NYA_SteamSessionRequest* request = data;

            // Only a Steam account can be addressed back, so an IP or generic identity is dropped: the transport answers by steam id and has nothing to answer one of those with.
            if (request->remote.type != _NYA_STEAM_IDENTITY_TYPE_STEAM_ID) break;
            if (request->remote.steam_id == 0) break;

            _nya_steam_p2p_event_push((NYA_SteamEvent){ .kind = NYA_STEAM_EVENT_SESSION_REQUEST, .user = { .value = request->remote.steam_id } });
        } break;

        case _NYA_STEAM_CALLBACK_SESSION_FAILED: {
            // SteamNetConnectionInfo_t opens with the identity, so only that prefix is read; the rest is diagnostics whose layout this module then need not track across SDK versions.
            if (size < sizeof(_NYA_SteamNetworkingIdentity)) break;

            const _NYA_SteamNetworkingIdentity* remote = data;

            if (remote->type != _NYA_STEAM_IDENTITY_TYPE_STEAM_ID) break;
            if (remote->steam_id == 0) break;

            _nya_steam_p2p_event_push((NYA_SteamEvent){ .kind = NYA_STEAM_EVENT_SESSION_FAILED, .user = { .value = remote->steam_id } });
        } break;

        default: break; // a callback this build does not act on, which is most of them
    }
}

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

void _nya_steam_queue_push(NYA_SteamEvent* queue, OUT u32* count, OUT u64* dropped, NYA_ConstCString what, NYA_SteamEvent event) {
    nya_assert(queue != nullptr);
    nya_assert(count != nullptr);
    nya_assert(event.kind > NYA_STEAM_EVENT_NONE && event.kind < NYA_STEAM_EVENT_KIND_COUNT);
    nya_assert(*count <= NYA_STEAM_MAX_EVENTS);

    if (*count == NYA_STEAM_MAX_EVENTS) {
        nya_memmove(&queue[0], &queue[1], (NYA_STEAM_MAX_EVENTS - 1) * sizeof(NYA_SteamEvent));
        *count = NYA_STEAM_MAX_EVENTS - 1;

        // once per connection: a lobby churning members would otherwise write a line per member.
        if (*dropped == 0) nya_log_warn("Steam: more than %d unread %s events; the oldest are being dropped.", NYA_STEAM_MAX_EVENTS, what);

        *dropped += 1;
    }

    queue[*count] = event;
    *count += 1;
}

b8 _nya_steam_queue_poll(NYA_SteamEvent* queue, OUT u32* count, OUT NYA_SteamEvent* out_event) {
    nya_assert(queue != nullptr);
    nya_assert(count != nullptr);
    nya_assert(out_event != nullptr);

    *out_event = (NYA_SteamEvent){ 0 };

    if (*count == 0) return false;

    nya_assert(*count <= NYA_STEAM_MAX_EVENTS);

    *out_event = queue[0];

    *count -= 1;
    nya_memmove(&queue[0], &queue[1], *count * sizeof(NYA_SteamEvent));

    queue[*count] = (NYA_SteamEvent){ 0 };

    nya_assert(out_event->kind > NYA_STEAM_EVENT_NONE && out_event->kind < NYA_STEAM_EVENT_KIND_COUNT);

    return true;
}

void _nya_steam_event_push(NYA_SteamEvent event) {
    _nya_steam_queue_push(_NYA_STEAM.events, &_NYA_STEAM.event_count, &_NYA_STEAM.events_dropped, "lobby", event);
}

void _nya_steam_p2p_event_push(NYA_SteamEvent event) {
    _nya_steam_queue_push(_NYA_STEAM.p2p_events, &_NYA_STEAM.p2p_event_count, &_NYA_STEAM.p2p_events_dropped, "session", event);
}

NYA_ConstCString _nya_steam_scratch_set(NYA_ConstCString text) {
    if (text == nullptr) {
        _NYA_STEAM.scratch[0] = '\0';
        return _NYA_STEAM.scratch;
    }

    // Bounded, since the bytes are another player's lobby value and the buffer is sized to Steam's own limit, not to something this module assumes.
    u64 length = strnlen(text, sizeof(_NYA_STEAM.scratch) - 1);

    nya_memcpy(_NYA_STEAM.scratch, text, length);
    _NYA_STEAM.scratch[length] = '\0';

    return _NYA_STEAM.scratch;
}

b8 _nya_steam_key_is_valid(NYA_ConstCString key) {
    if (key == nullptr) return false;

    u64 length = strnlen(key, NYA_STEAM_MAX_KEY);

    return length > 0 && length < NYA_STEAM_MAX_KEY;
}

NYA_Error _nya_steam_unavailable(NYA_ConstCString what) {
    return nya_error(NYA_ERROR_NOT_SUPPORTED, "no Steam client to %s", what);
}

// ───────────────────────────────────── THE STEAMWORKS BACKEND ─────────────────────────────────────

#ifndef NYA_PLUGIN_STEAM

const NYA_SteamBackend* _nya_steam_backend_default(void) {
    // No Steamworks library in this build: null, not a stub table, since every call above already checks and stubs would make nya_steam_is_connected lie.
    return nullptr;
}

#else

#include "nyangine-plugins/steam/steam_steamworks.c"

#endif // NYA_PLUGIN_STEAM
