/**
 * @file steam.h
 *
 * The Steamworks client: the connection, lobbies, peer-to-peer messaging, achievements, stats and Cloud.
 * nya_app drives the connection from `NYA_AppOptions.steam_app_id`: relaunch through Steam when started
 * outside it, connect, pump callbacks once a frame, disconnect.
 *
 * Functions
 *
 *   nya_system_steam_restart_if_necessary  relaunch through the client; call before anything is brought up
 *   nya_system_steam_init / _update / _deinit  the connection and its once-a-frame callback pump
 *   nya_steam_is_connected                 whether any of the rest will do anything
 *   nya_steam_user_id, nya_steam_user_name who is signed in
 *   nya_steam_poll                         what the client reported this frame
 *   nya_steam_lobby_create / _join / _leave / _current   the lobby this player is in
 *   nya_steam_lobby_list_request / _count / _at          finding somebody else's
 *   nya_steam_lobby_data_get / _set                      the lobby's own key-value table
 *   nya_steam_lobby_member_count / _at / _owner / _limit  who is in it
 *   nya_steam_lobby_member_data_get / _set               a member's own table, such as their readiness
 *   nya_steam_lobby_invite / _invite_open                invite one friend, or open the overlay to pick
 *   nya_steam_achievement_get / _set / _clear / _progress
 *   nya_steam_stat_get_int / _set_int / _get_float / _set_float / nya_steam_stats_store
 *   nya_steam_cloud_enabled / _quota / _write / _read / _exists / _delete / _size
 *   nya_steam_rich_presence_set / _clear   the "join game" line on a friend's list
 *
 * ```c
 * // In the menu, hosting.
 * NYA_EXPECT(nya_steam_lobby_create(NYA_STEAM_LOBBY_FRIENDS_ONLY, 8));
 *
 * // Once the LOBBY_ENTERED event arrives, publish how to reach this game and let friends in.
 * NYA_EXPECT(nya_steam_lobby_data_set("join", secret));
 * (void)nya_steam_lobby_invite_open();
 *
 * // Every frame.
 * NYA_SteamEvent event;
 * while (nya_steam_poll(&event)) { ... }
 * ```
 *
 * Why it looks like this
 *
 * - Every call is safe with no Steam client running, in a build without the plugin, and before init.
 *   It returns NYA_ERROR_NOT_SUPPORTED or an empty answer and changes nothing, so a game needs no
 *   `#ifdef` and no "is Steam up" branch around a feature that is simply off. That is the documented
 *   behaviour of `steam_app_id` and everything here holds to it.
 * - The module is compiled into every build; only the Steamworks library is behind NYA_PLUGIN_STEAM.
 *   A backend table decides at startup which of the two implementations is installed, which is also
 *   what lets a test run the whole module, the lobby bookkeeping and the Steam transport against a
 *   fake client. See NYA_SteamBackend.
 * - Asynchronous results arrive as queued events rather than callbacks, for the same reason Discord's
 *   do: joining a lobby changes screens, and a callback would do that from inside the callback pump.
 * - Steam's own strings are copied into fixed buffers at the boundary. Lobby data is written by other
 *   players and is never trusted, bounded or escaped by the client.
 *
 * A shipped depot must not contain steam_appid.txt, or a copy started outside Steam never relaunches
 * through it. Put one beside a development build to run it without the client.
 *
 * Steam Cloud: the save root (nya_save_root) is what this game keeps. Two ways to have Steam carry it,
 * and this module implements the second:
 *
 * - Auto-Cloud, configured in the partner site with no code at all. Add one root override per platform,
 *   `Linux: {app}/../../.local/share`, `Windows: WinAppDataLocal`, with path `gnyame` and pattern `*`,
 *   excluding `logs/`. Steam then syncs the whole directory around the process.
 * - The API below, for a game that wants to choose what is synced or to read a save written on another
 *   machine before the local one is touched. nya_steam_cloud_write mirrors one save-relative file into
 *   the Cloud; nya_steam_cloud_read brings it back.
 * */
#pragma once

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/** How long a lobby data key may be, buffer included. Steam's own limit is 255. */
#define NYA_STEAM_MAX_KEY 256

/**
 * How long a lobby data value may be, buffer included. Steam's `k_cubChatMetadataMax` is 8192, and a
 * value is the only thing here another player writes, so the buffer is sized to its hard limit.
 * */
#define NYA_STEAM_MAX_VALUE 8192

/** How long a persona name may be, buffer included. Steam's limit is 32 plus room for UTF-8. */
#define NYA_STEAM_MAX_NAME 128

/**
 * How long a rich presence "connect" string may be, buffer included. Steam's own
 * `k_cchMaxRichPresenceValueLength` is 256, and this is the only value carried inside an event, so it
 * is sized to that rather than to NYA_STEAM_MAX_VALUE.
 * */
#define NYA_STEAM_MAX_CONNECT 256

/**
 * How many lobbies one list request keeps.
 *
 * Steam returns up to 50 by default and a server browser shows a page at a time, so 64 covers a full
 * answer with room to spare. Anything past it is dropped with a warning rather than growing an array
 * from a number the back end chose.
 * */
#define NYA_STEAM_MAX_LOBBIES 64

/**
 * How many events are held before the oldest is dropped. Drained every frame; the only way to fill it
 * is a lobby whose members are all changing state at once, and sixteen covers a full lobby doing that.
 * */
#define NYA_STEAM_MAX_EVENTS 16

/** How many messages one peer-to-peer receive call takes at a time. */
#define NYA_STEAM_MAX_RECEIVE 32

/** The longest peer-to-peer message this module will carry, which is one network datagram. */
#define NYA_STEAM_MAX_MESSAGE 1200

// ───────────────────────────────────── TYPES ─────────────────────────────────────

typedef enum NYA_SteamInitResult NYA_SteamInitResult;
typedef enum NYA_SteamLobbyKind  NYA_SteamLobbyKind;
typedef enum NYA_SteamEventKind  NYA_SteamEventKind;
typedef struct NYA_SteamId       NYA_SteamId;
typedef struct NYA_SteamEvent    NYA_SteamEvent;
typedef struct NYA_SteamMessage  NYA_SteamMessage;
typedef struct NYA_SteamBackend  NYA_SteamBackend;

enum NYA_SteamInitResult {
    NYA_SYSTEM_STEAM_INIT_OK               = 0,
    NYA_SYSTEM_STEAM_INIT_FAILED_GENERIC   = 1,
    NYA_SYSTEM_STEAM_INIT_NO_STEAM_CLIENT  = 2,
    NYA_SYSTEM_STEAM_INIT_VERSION_MISMATCH = 3,
    NYA_SYSTEM_STEAM_INIT_COUNT,
};

/**
 * A Steam account or lobby, as a type rather than a bare `u64`.
 *
 * A lobby id and a user id are both 64 bit numbers and are constantly passed side by side, which is
 * exactly the parameter list the style this engine is written in refuses to have.
 * */
struct NYA_SteamId {
    u64 value;
};

#define NYA_STEAM_ID_NONE ((NYA_SteamId){ .value = 0 })

/** Whether an id names anything. A zero id is Steam's own "nobody". */
NYA_API b8 nya_steam_id_is_set(NYA_SteamId id) __attr_no_discard;

/** Whether two ids are the same account or lobby. */
NYA_API b8 nya_steam_id_equals(NYA_SteamId a, NYA_SteamId b) __attr_no_discard;

/** Who may find and join a lobby. The values are Steam's ELobbyType and are sent to it as they are. */
enum NYA_SteamLobbyKind {
    /** Invite only, and invisible to everyone else. */
    NYA_STEAM_LOBBY_PRIVATE = 0,

    /** Friends of anybody in it can see and join it. The usual choice for a co-op game. */
    NYA_STEAM_LOBBY_FRIENDS_ONLY = 1,

    /** Anybody can find it through nya_steam_lobby_list_request. */
    NYA_STEAM_LOBBY_PUBLIC = 2,

    /** Joinable by id, but never returned by a search and not shown on a friend's list. */
    NYA_STEAM_LOBBY_INVISIBLE = 3,

    NYA_STEAM_LOBBY_KIND_COUNT,
};

enum NYA_SteamEventKind {
    NYA_STEAM_EVENT_NONE = 0,

    /** This player is now in `lobby`. Sent for a lobby this player created and one they joined alike. */
    NYA_STEAM_EVENT_LOBBY_ENTERED,

    /** Creating or joining a lobby did not work. `reason` says which Steam result came back. */
    NYA_STEAM_EVENT_LOBBY_FAILED,

    /** Somebody joined or left `lobby`. `user` is who, and the member list has already changed. */
    NYA_STEAM_EVENT_LOBBY_MEMBER_CHANGED,

    /** A lobby's data, or one member's, changed. Re-read whatever the game shows from it. */
    NYA_STEAM_EVENT_LOBBY_DATA_CHANGED,

    /** A lobby search finished. Read it with nya_steam_lobby_list_count and _at. */
    NYA_STEAM_EVENT_LOBBY_LIST,

    /**
     * The player accepted an invite or clicked "join game" on a friend's list. `lobby` is what to join
     * and `secret` is what a rich presence "connect" string carried, whichever the friend published.
     * */
    NYA_STEAM_EVENT_JOIN_REQUESTED,

    /**
     * A peer wants to send this process messages. Drained by nya_steam_p2p_poll, not nya_steam_poll:
     * the transport owns these and a game watching lobbies must not be able to swallow one.
     * */
    NYA_STEAM_EVENT_SESSION_REQUEST,

    /** A peer-to-peer session broke. Also nya_steam_p2p_poll's. */
    NYA_STEAM_EVENT_SESSION_FAILED,

    NYA_STEAM_EVENT_KIND_COUNT,
};

/**
 * One thing the Steam client reported, drained by nya_steam_poll.
 * */
struct NYA_SteamEvent {
    NYA_SteamEventKind kind;

    /** The lobby this is about, for every LOBBY_ event and for JOIN_REQUESTED. */
    NYA_SteamId lobby;

    /** Who it is about: the member who changed, the friend who invited, the peer who sent. */
    NYA_SteamId user;

    /** Steam's EResult for a LOBBY_FAILED, as it came back. Zero for everything else. */
    u32 reason;

    /** A rich presence connect string, for JOIN_REQUESTED. Empty when the invite was a lobby id. */
    char secret[NYA_STEAM_MAX_CONNECT];
};

/** One peer-to-peer message handed back by the backend. `data` dies on the next receive call. */
struct NYA_SteamMessage {
    NYA_SteamId sender;
    const u8*   data;
    u32         size;
    b8          reliable;
};

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

// ───────────────────────────────────── SYSTEM FUNCTIONS ─────────────────────────────────────

/**
 * True when Steam is relaunching the game through the client and this process should exit now. Call before anything
 * else is brought up.
 * */
NYA_API b8 nya_system_steam_restart_if_necessary(u32 app_id) __attr_no_discard;

/** Connects to the Steam client. Logs and returns the reason when it cannot; the game runs on without Steam. */
NYA_API NYA_SteamInitResult nya_system_steam_init(void);

/** Dispatches Steam's callbacks. Once a frame, on the main thread. Does nothing while not connected. */
NYA_API void nya_system_steam_update(void);

NYA_API void nya_system_steam_deinit(void);

// ───────────────────────────────────── THE CONNECTION ─────────────────────────────────────

/** Whether the client connection is up. Everything below answers emptily while this is false. */
NYA_API b8 nya_steam_is_connected(void) __attr_no_discard;

/** This player's account, or NYA_STEAM_ID_NONE while nothing is connected. */
NYA_API NYA_SteamId nya_steam_user_id(void) __attr_no_discard;

/** This player's persona name, or an empty string. Owned by the module. */
NYA_API NYA_ConstCString nya_steam_user_name(void) __attr_no_discard;

/**
 * A friend's or lobby member's persona name, or an empty string when Steam has not cached one yet.
 * Owned by the module and valid until the next call.
 * */
NYA_API NYA_ConstCString nya_steam_friend_name(NYA_SteamId user) __attr_no_discard;

/**
 * Drains one queued lobby or invite event. False when there are none left. Call after
 * nya_system_steam_update.
 *
 * Peer-to-peer session events are not in this queue; see nya_steam_p2p_poll.
 * */
NYA_API b8 nya_steam_poll(OUT NYA_SteamEvent* out_event);

// ───────────────────────────────────── LOBBIES ─────────────────────────────────────

/**
 * Asks Steam for a new lobby. The answer arrives as LOBBY_ENTERED or LOBBY_FAILED, never here.
 *
 * `max_members` is clamped to 1..250, which is Steam's own range.
 * */
NYA_API NYA_Error nya_steam_lobby_create(NYA_SteamLobbyKind kind, u32 max_members) __attr_no_discard;

/** Joins `lobby`. The answer arrives as LOBBY_ENTERED or LOBBY_FAILED. */
NYA_API NYA_Error nya_steam_lobby_join(NYA_SteamId lobby) __attr_no_discard;

/** Leaves whatever lobby this player is in. A no-op when there is none, so a teardown path can call it. */
NYA_API void nya_steam_lobby_leave(void);

/** The lobby this player is in, or NYA_STEAM_ID_NONE. */
NYA_API NYA_SteamId nya_steam_lobby_current(void) __attr_no_discard;

/**
 * Starts a search for public lobbies, optionally only those whose `key` is `value`. The answer arrives
 * as a LOBBY_LIST event; until then the previous list is still readable.
 *
 * `max_results` is clamped to NYA_STEAM_MAX_LOBBIES.
 * */
NYA_API NYA_Error nya_steam_lobby_list_request(NYA_ConstCString key, NYA_ConstCString value, u32 max_results) __attr_no_discard;

/** How many lobbies the last finished search found. */
NYA_API u32 nya_steam_lobby_list_count(void) __attr_no_discard;

/** The lobby at `index`, or NYA_STEAM_ID_NONE past the end. */
NYA_API NYA_SteamId nya_steam_lobby_list_at(u32 index) __attr_no_discard;

/**
 * Reads a key from a lobby's own table. Empty when the key is unset or nothing is connected.
 *
 * The value was written by whoever owns that lobby, so it is another player's bytes: treat it as input.
 * The returned string is owned by the module and valid until the next call.
 * */
NYA_API NYA_ConstCString nya_steam_lobby_data_get(NYA_SteamId lobby, NYA_ConstCString key) __attr_no_discard;

/** Writes a key on the lobby this player owns. Only the owner may, and Steam ignores anyone else. */
NYA_API NYA_Error nya_steam_lobby_data_set(NYA_ConstCString key, NYA_ConstCString value) __attr_no_discard;

/** How many people are in `lobby`. */
NYA_API u32 nya_steam_lobby_member_count(NYA_SteamId lobby) __attr_no_discard;

/** The member at `index`, or NYA_STEAM_ID_NONE past the end. */
NYA_API NYA_SteamId nya_steam_lobby_member_at(NYA_SteamId lobby, u32 index) __attr_no_discard;

/** Who owns `lobby`, which is who may write its data and who the game should treat as the host. */
NYA_API NYA_SteamId nya_steam_lobby_owner(NYA_SteamId lobby) __attr_no_discard;

/** How many people `lobby` holds. Zero when it is not known. */
NYA_API u32 nya_steam_lobby_member_limit(NYA_SteamId lobby) __attr_no_discard;

/** Reads one member's own key, such as whether they are ready. Same lifetime as the lobby's. */
NYA_API NYA_ConstCString nya_steam_lobby_member_data_get(NYA_SteamId lobby, NYA_SteamId user, NYA_ConstCString key) __attr_no_discard;

/** Writes one of this player's own keys in the current lobby. Anyone may write their own. */
NYA_API NYA_Error nya_steam_lobby_member_data_set(NYA_ConstCString key, NYA_ConstCString value) __attr_no_discard;

/** Invites one friend to the current lobby. They get a Steam invite and a JOIN_REQUESTED if they accept. */
NYA_API NYA_Error nya_steam_lobby_invite(NYA_SteamId user) __attr_no_discard;

/**
 * Opens the Steam overlay on the invite dialog for the current lobby, so the player picks the friends.
 *
 * Needs the overlay, which a client started with it disabled does not have; the error says so and a game
 * should fall back to its own friend list or to a copyable join secret.
 * */
NYA_API NYA_Error nya_steam_lobby_invite_open(void) __attr_no_discard;

// ───────────────────────────────────── ACHIEVEMENTS AND STATS ─────────────────────────────────────

// Achievements and stats are written locally and pushed with nya_steam_stats_store; Steam shows the unlock toast on the store, not the set, so a game that never stores never congratulates anybody.

/** Whether `name` is unlocked. False when nothing is connected or the name is not one of the game's. */
NYA_API b8 nya_steam_achievement_get(NYA_ConstCString name) __attr_no_discard;

/** Unlocks `name`. Idempotent: unlocking one that is already unlocked does nothing and is not an error. */
NYA_API NYA_Error nya_steam_achievement_set(NYA_ConstCString name) __attr_no_discard;

/** Locks `name` again. For testing an achievement, and for a game that resets progress. */
NYA_API NYA_Error nya_steam_achievement_clear(NYA_ConstCString name) __attr_no_discard;

/**
 * Shows the "12 of 50" progress toast for a partially complete achievement.
 *
 * Steam only displays it at the notification steps configured for the achievement, and refuses
 * `current` at or above `max`, where the unlock itself is what should be sent.
 * */
NYA_API NYA_Error nya_steam_achievement_progress(NYA_ConstCString name, u32 current, u32 max) __attr_no_discard;

/** Reads an integer stat. Zero when nothing is connected or the stat is not one of the game's. */
NYA_API s32 nya_steam_stat_get_int(NYA_ConstCString name) __attr_no_discard;

NYA_API NYA_Error nya_steam_stat_set_int(NYA_ConstCString name, s32 value) __attr_no_discard;

/** Reads a floating point stat. Zero when nothing is connected. */
NYA_API f32 nya_steam_stat_get_float(NYA_ConstCString name) __attr_no_discard;

NYA_API NYA_Error nya_steam_stat_set_float(NYA_ConstCString name, f32 value) __attr_no_discard;

/**
 * Pushes every set achievement and stat to Steam. Once at a natural break, not per change: Steam rate
 * limits this and a game storing per kill spends its budget and loses the rest.
 * */
NYA_API NYA_Error nya_steam_stats_store(void) __attr_no_discard;

// ───────────────────────────────────── CLOUD ─────────────────────────────────────

/**
 * Whether the Cloud is on for this account and this game. Both switches must be on, and a player may
 * turn either off, so this is asked rather than assumed.
 * */
NYA_API b8 nya_steam_cloud_enabled(void) __attr_no_discard;

/** How much Cloud space the game has, and how much is left. False when nothing is connected. */
NYA_API b8 nya_steam_cloud_quota(OUT u64* out_total_bytes, OUT u64* out_available_bytes) __attr_no_discard;

/** Whether `name` is in the Cloud. */
NYA_API b8 nya_steam_cloud_exists(NYA_ConstCString name) __attr_no_discard;

/** How large `name` is in the Cloud, or zero. */
NYA_API u64 nya_steam_cloud_size(NYA_ConstCString name) __attr_no_discard;

/** Writes `size` bytes into the Cloud under `name`. */
NYA_API NYA_Error nya_steam_cloud_write(NYA_ConstCString name, const u8* data, u64 size) __attr_no_discard;

/**
 * Reads `name` out of the Cloud into `out_data`, writing how many bytes it was.
 *
 * The file was written by this game on another machine, so it is input like any other: the size is
 * checked against `capacity` before anything is copied, and a file larger than the buffer is an error
 * rather than a truncated save.
 * */
NYA_API NYA_Error nya_steam_cloud_read(NYA_ConstCString name, OUT u8* out_data, u64 capacity, OUT u64* out_size) __attr_no_discard;

/** Removes `name` from the Cloud. Not an error when it was not there. */
NYA_API NYA_Error nya_steam_cloud_delete(NYA_ConstCString name) __attr_no_discard;

// ───────────────────────────────────── RICH PRESENCE ─────────────────────────────────────

/**
 * Sets one rich presence key. `steam_display` selects a localization token from the partner site, and
 * `connect` is the string a friend's client hands back as a JOIN_REQUESTED secret.
 * */
NYA_API NYA_Error nya_steam_rich_presence_set(NYA_ConstCString key, NYA_ConstCString value) __attr_no_discard;

/** Clears every rich presence key. What leaving a session, or quitting, wants. */
NYA_API void nya_steam_rich_presence_clear(void);

// ───────────────────────────────────── PEER TO PEER ─────────────────────────────────────

// Exposed rather than private because net_steam.c (its only caller) lives in net/ while the connection lives here, and a game reaching past the transport should not reimplement the connection.

/** Sends one message to `user`, reliably or not, on `channel`. */
NYA_API NYA_Error nya_steam_p2p_send(NYA_SteamId user, const u8* data, u64 size, b8 reliable, u32 channel) __attr_no_discard;

/**
 * Takes up to `capacity` messages waiting on `channel`, newest last, and returns how many.
 *
 * The bytes each message points at are owned by the module and die on the next call, so a caller that
 * keeps one copies it.
 * */
NYA_API u32 nya_steam_p2p_receive(u32 channel, OUT NYA_SteamMessage* out_messages, u32 capacity) __attr_no_discard;

/**
 * Drains one SESSION_REQUEST or SESSION_FAILED. A queue of its own, so the transport draining it and a
 * menu draining nya_steam_poll never take each other's events.
 * */
NYA_API b8 nya_steam_p2p_poll(OUT NYA_SteamEvent* out_event);

/**
 * Accepts a session a SESSION_REQUEST announced. Until this is called nothing from that peer arrives.
 * */
NYA_API NYA_Error nya_steam_p2p_accept(NYA_SteamId user) __attr_no_discard;

/** Closes the session with `user`. Idempotent. */
NYA_API void nya_steam_p2p_close(NYA_SteamId user);

// ───────────────────────────────────── THE BACKEND ─────────────────────────────────────

/**
 * What the module needs from a Steam client, as a table it can be handed.
 *
 * One entry per SDK call this module makes, in the SDK's own shape, so the real implementation is a
 * direct forward and nothing above it knows whether it is talking to Steam. It exists so a test can run
 * the lobby bookkeeping, the callback decoder and the Steam network transport against a fake client:
 * there is no Steam client in CI, and a transport that is only ever exercised by hand is a transport
 * nobody knows the shape of.
 *
 * Every entry may be null, and the module checks before calling: a fake implements what its test needs.
 * */
struct NYA_SteamBackend {
    /** For log lines. "steamworks" for the real one. */
    NYA_ConstCString name;

    /** SteamAPI_InitFlat. Returns a NYA_SteamInitResult and fills `out_message` with Steam's own text. */
    NYA_SteamInitResult (*connect)(OUT char* out_message, u64 capacity);

    void (*disconnect)(void);

    /**
     * One frame of Steam's callback queue. Each callback is handed to _nya_steam_on_callback with the
     * id and the bytes Steam produced, which is what makes the decoder testable without a client.
     * */
    void (*run_callbacks)(void);

    b8 (*restart_if_necessary)(u32 app_id);

    u64              (*user_id)(void);
    NYA_ConstCString (*user_name)(void);
    NYA_ConstCString (*friend_name)(u64 user);

    b8   (*lobby_create)(u32 kind, u32 max_members);
    b8   (*lobby_join)(u64 lobby);
    void (*lobby_leave)(u64 lobby);
    b8   (*lobby_list_request)(NYA_ConstCString key, NYA_ConstCString value, u32 max_results);
    u64  (*lobby_list_at)(u32 index);

    NYA_ConstCString (*lobby_data_get)(u64 lobby, NYA_ConstCString key);
    b8               (*lobby_data_set)(u64 lobby, NYA_ConstCString key, NYA_ConstCString value);
    NYA_ConstCString (*lobby_member_data_get)(u64 lobby, u64 user, NYA_ConstCString key);
    void             (*lobby_member_data_set)(u64 lobby, NYA_ConstCString key, NYA_ConstCString value);

    u32 (*lobby_member_count)(u64 lobby);
    u64 (*lobby_member_at)(u64 lobby, u32 index);
    u64 (*lobby_owner)(u64 lobby);
    u32 (*lobby_member_limit)(u64 lobby);

    b8 (*lobby_invite)(u64 lobby, u64 user);
    b8 (*overlay_invite_open)(u64 lobby);

    b8 (*achievement_get)(NYA_ConstCString name, OUT b8* out_unlocked);
    b8 (*achievement_set)(NYA_ConstCString name, b8 unlocked);
    b8 (*achievement_progress)(NYA_ConstCString name, u32 current, u32 max);
    b8 (*stat_get_int)(NYA_ConstCString name, OUT s32* out_value);
    b8 (*stat_set_int)(NYA_ConstCString name, s32 value);
    b8 (*stat_get_float)(NYA_ConstCString name, OUT f32* out_value);
    b8 (*stat_set_float)(NYA_ConstCString name, f32 value);
    b8 (*stats_store)(void);

    b8  (*cloud_enabled)(void);
    b8  (*cloud_quota)(OUT u64* out_total, OUT u64* out_available);
    b8  (*cloud_exists)(NYA_ConstCString name);
    u64 (*cloud_size)(NYA_ConstCString name);
    b8  (*cloud_write)(NYA_ConstCString name, const u8* data, u32 size);
    s32 (*cloud_read)(NYA_ConstCString name, OUT u8* out_data, u32 capacity);
    b8  (*cloud_delete)(NYA_ConstCString name);

    b8   (*rich_presence_set)(NYA_ConstCString key, NYA_ConstCString value);
    void (*rich_presence_clear)(void);

    b8  (*p2p_send)(u64 user, const u8* data, u32 size, b8 reliable, u32 channel);
    u32 (*p2p_receive)(u32 channel, OUT NYA_SteamMessage* out_messages, u32 capacity);
    b8  (*p2p_accept)(u64 user);
    void (*p2p_close)(u64 user);
};

/**
 * Installs a backend, in place of the one nya_system_steam_init would pick.
 *
 * For tests only, and it must be called before nya_system_steam_init. Passing null puts the real one
 * back. The real backend is chosen by the build: the Steamworks library with NYA_PLUGIN_STEAM, and
 * nothing at all without it, which is what makes every call above safe in a build that has no Steam.
 * */
NYA_API void nya_steam_backend_set(const NYA_SteamBackend* backend);

/**
 * Where a backend hands one of Steam's callbacks in. The id is Steam's `k_iCallback` and the bytes are
 * the callback struct exactly as Steam laid it out; anything this module does not recognise is ignored.
 * */
NYA_API void nya_steam_on_callback(u32 callback_id, const void* data, u32 size);
