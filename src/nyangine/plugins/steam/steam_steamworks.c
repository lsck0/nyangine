/**
 * @file steam_steamworks.c
 *
 * The real backend: every NYA_SteamBackend entry forwarded to the Steamworks flat API.
 *
 * Included from steam.c under NYA_PLUGIN_STEAM and nowhere else, so a build without the Steamworks
 * library never names one of these symbols and never has to link it.
 *
 * The SDK's headers are C++, so the exports are declared here by hand rather than included. Each
 * declaration is copied from public/steam/steam_api_flat.h; the interface accessors carry a version
 * number (`SteamAPI_SteamFriends_v018`) which an SDK bump changes, and a mismatch is a link error
 * rather than a silent misdispatch, which is why the versioned names are used rather than the inline
 * wrappers around them.
 * */

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE FLAT API
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef void ISteamUser;
typedef void ISteamFriends;
typedef void ISteamMatchmaking;
typedef void ISteamUserStats;
typedef void ISteamRemoteStorage;
typedef void ISteamNetworkingMessages;

typedef s32  ESteamAPIInitResult;
typedef char SteamErrMsg[1024];
typedef s32  HSteamPipe;
typedef u64  SteamAPICall_t;

/** CallbackMsg_t, which SteamAPI_ManualDispatch_GetNextCallback fills. */
typedef struct {
    s32 steam_user;
    s32 callback;
    u8* param;
    s32 param_size;
} _NYA_SteamCallbackMsg;

/** SteamAPICallCompleted_t, the callback that says an asynchronous call's result is ready. */
typedef struct {
    SteamAPICall_t call;
    s32            callback;
    u32            param_size;
} _NYA_SteamCallCompleted;

/** k_iSteamUtilsCallbacks + 3. */
#define _NYA_STEAM_CALLBACK_CALL_COMPLETED 703

static_assert(sizeof(_NYA_SteamCallbackMsg) == 24, "CallbackMsg_t is two int, a pointer and an int");
static_assert(sizeof(_NYA_SteamCallCompleted) == 16, "SteamAPICallCompleted_t is a u64 and two u32");

extern ESteamAPIInitResult SteamAPI_InitFlat(SteamErrMsg* out_message);
extern void                SteamAPI_Shutdown(void);
extern bool                SteamAPI_RestartAppIfNecessary(u32 app_id);
extern HSteamPipe          SteamAPI_GetHSteamPipe(void);

extern void SteamAPI_ManualDispatch_Init(void);
extern void SteamAPI_ManualDispatch_RunFrame(HSteamPipe pipe);
extern bool SteamAPI_ManualDispatch_GetNextCallback(HSteamPipe pipe, _NYA_SteamCallbackMsg* out_message);
extern void SteamAPI_ManualDispatch_FreeLastCallback(HSteamPipe pipe);
extern bool SteamAPI_ManualDispatch_GetAPICallResult(HSteamPipe pipe, SteamAPICall_t call, void* out_callback, s32 callback_size, s32 expected_callback,
                                                     bool* out_failed);

extern ISteamUser* SteamAPI_SteamUser_v023(void);
extern u64         SteamAPI_ISteamUser_GetSteamID(ISteamUser* self);

extern ISteamFriends* SteamAPI_SteamFriends_v018(void);
extern const char*    SteamAPI_ISteamFriends_GetPersonaName(ISteamFriends* self);
extern const char*    SteamAPI_ISteamFriends_GetFriendPersonaName(ISteamFriends* self, u64 user);
extern void           SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(ISteamFriends* self, u64 lobby);
extern bool           SteamAPI_ISteamFriends_SetRichPresence(ISteamFriends* self, const char* key, const char* value);
extern void           SteamAPI_ISteamFriends_ClearRichPresence(ISteamFriends* self);

extern ISteamMatchmaking* SteamAPI_SteamMatchmaking_v009(void);
extern SteamAPICall_t     SteamAPI_ISteamMatchmaking_CreateLobby(ISteamMatchmaking* self, s32 kind, s32 max_members);
extern SteamAPICall_t     SteamAPI_ISteamMatchmaking_JoinLobby(ISteamMatchmaking* self, u64 lobby);
extern void               SteamAPI_ISteamMatchmaking_LeaveLobby(ISteamMatchmaking* self, u64 lobby);
extern bool               SteamAPI_ISteamMatchmaking_InviteUserToLobby(ISteamMatchmaking* self, u64 lobby, u64 invitee);
extern SteamAPICall_t     SteamAPI_ISteamMatchmaking_RequestLobbyList(ISteamMatchmaking* self);
extern void               SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(ISteamMatchmaking* self, const char* key, const char* value, s32 comparison);
extern void               SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter(ISteamMatchmaking* self, s32 max_results);
extern u64                SteamAPI_ISteamMatchmaking_GetLobbyByIndex(ISteamMatchmaking* self, s32 index);
extern s32                SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(ISteamMatchmaking* self, u64 lobby);
extern u64                SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(ISteamMatchmaking* self, u64 lobby, s32 index);
extern const char*        SteamAPI_ISteamMatchmaking_GetLobbyData(ISteamMatchmaking* self, u64 lobby, const char* key);
extern bool               SteamAPI_ISteamMatchmaking_SetLobbyData(ISteamMatchmaking* self, u64 lobby, const char* key, const char* value);
extern const char*        SteamAPI_ISteamMatchmaking_GetLobbyMemberData(ISteamMatchmaking* self, u64 lobby, u64 user, const char* key);
extern void               SteamAPI_ISteamMatchmaking_SetLobbyMemberData(ISteamMatchmaking* self, u64 lobby, const char* key, const char* value);
extern s32                SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit(ISteamMatchmaking* self, u64 lobby);
extern u64                SteamAPI_ISteamMatchmaking_GetLobbyOwner(ISteamMatchmaking* self, u64 lobby);

extern ISteamUserStats* SteamAPI_SteamUserStats_v013(void);
extern bool             SteamAPI_ISteamUserStats_GetStatInt32(ISteamUserStats* self, const char* name, s32* out_value);
extern bool             SteamAPI_ISteamUserStats_GetStatFloat(ISteamUserStats* self, const char* name, f32* out_value);
extern bool             SteamAPI_ISteamUserStats_SetStatInt32(ISteamUserStats* self, const char* name, s32 value);
extern bool             SteamAPI_ISteamUserStats_SetStatFloat(ISteamUserStats* self, const char* name, f32 value);
extern bool             SteamAPI_ISteamUserStats_GetAchievement(ISteamUserStats* self, const char* name, bool* out_achieved);
extern bool             SteamAPI_ISteamUserStats_SetAchievement(ISteamUserStats* self, const char* name);
extern bool             SteamAPI_ISteamUserStats_ClearAchievement(ISteamUserStats* self, const char* name);
extern bool             SteamAPI_ISteamUserStats_StoreStats(ISteamUserStats* self);
extern bool             SteamAPI_ISteamUserStats_IndicateAchievementProgress(ISteamUserStats* self, const char* name, u32 current, u32 max);

extern ISteamRemoteStorage* SteamAPI_SteamRemoteStorage_v016(void);
extern bool                 SteamAPI_ISteamRemoteStorage_FileWrite(ISteamRemoteStorage* self, const char* name, const void* data, s32 size);
extern s32                  SteamAPI_ISteamRemoteStorage_FileRead(ISteamRemoteStorage* self, const char* name, void* out_data, s32 capacity);
extern bool                 SteamAPI_ISteamRemoteStorage_FileDelete(ISteamRemoteStorage* self, const char* name);
extern bool                 SteamAPI_ISteamRemoteStorage_FileExists(ISteamRemoteStorage* self, const char* name);
extern s32                  SteamAPI_ISteamRemoteStorage_GetFileSize(ISteamRemoteStorage* self, const char* name);
extern bool                 SteamAPI_ISteamRemoteStorage_GetQuota(ISteamRemoteStorage* self, u64* out_total, u64* out_available);
extern bool                 SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount(ISteamRemoteStorage* self);
extern bool                 SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp(ISteamRemoteStorage* self);

/**
 * SteamNetworkingMessage_t, mirrored so the fields this file reads can be found in it.
 * */
typedef struct _NYA_SteamNetworkingMessage _NYA_SteamNetworkingMessage;

struct _NYA_SteamNetworkingMessage {
    void*                        data;
    s32                          size;
    u32                          connection;
    _NYA_SteamNetworkingIdentity peer;
    s64                          connection_user_data;
    s64                          time_received_us;
    s64                          message_number;
    void                         (*free_data)(_NYA_SteamNetworkingMessage* message);
    void                         (*release)(_NYA_SteamNetworkingMessage* message);
    s32                          channel;
    s32                          flags;
    s64                          user_data;
    u16                          lane;
    u16                          padding;
};

static_assert(sizeof(_NYA_SteamNetworkingMessage) == 216, "SteamNetworkingMessage_t layout moved; the receive path reads the wrong fields");

/** k_nSteamNetworkingSend_Reliable, and the unreliable value, which is zero. */
#define _NYA_STEAM_SEND_UNRELIABLE 0
#define _NYA_STEAM_SEND_RELIABLE   8

/**
 * k_nSteamNetworkingSend_AutoRestartBrokenSession, which is what keeps a session that timed out from
 * needing the game to notice and reconnect. Without it a quiet peer's first message after the timeout
 * is dropped with k_EResultNoConnection.
 * */
#define _NYA_STEAM_SEND_AUTO_RESTART 32

extern ISteamNetworkingMessages* SteamAPI_SteamNetworkingMessages_SteamAPI_v002(void);
extern s32  SteamAPI_ISteamNetworkingMessages_SendMessageToUser(ISteamNetworkingMessages* self, const _NYA_SteamNetworkingIdentity* remote, const void* data,
                                                                u32 size, s32 flags, s32 channel);
extern s32  SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(ISteamNetworkingMessages* self, s32 channel, _NYA_SteamNetworkingMessage** out_messages,
                                                                       s32 max_messages);
extern bool SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(ISteamNetworkingMessages* self, const _NYA_SteamNetworkingIdentity* remote);
extern bool SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(ISteamNetworkingMessages* self, const _NYA_SteamNetworkingIdentity* remote);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Where a received message's bytes are copied to, so the SDK's own allocation is released inside the
 * receive call rather than held until the caller is done with it.
 * */
typedef struct {
    HSteamPipe pipe;

    u8  receive_buffer[NYA_STEAM_MAX_RECEIVE * NYA_STEAM_MAX_MESSAGE];
    u32 receive_used;
} _NYA_SteamworksState;

NYA_INTERNAL _NYA_SteamworksState _NYA_STEAMWORKS = { 0 };

/** A SteamNetworkingIdentity naming one Steam account, which is the only kind this module addresses. */
NYA_INTERNAL _NYA_SteamNetworkingIdentity _nya_steamworks_identity(u64 user) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

_NYA_SteamNetworkingIdentity _nya_steamworks_identity(u64 user) {
    return (_NYA_SteamNetworkingIdentity){
        .type       = _NYA_STEAM_IDENTITY_TYPE_STEAM_ID,
        .size_bytes = (s32)sizeof(u64),
        .steam_id   = user,
    };
}

NYA_INTERNAL NYA_SteamInitResult _nya_steamworks_connect(OUT char* out_message, u64 capacity) {
    nya_assert(out_message != nullptr);
    nya_assert(capacity > 0);

    SteamErrMsg message = { 0 };

    ESteamAPIInitResult result = SteamAPI_InitFlat(&message);

    message[sizeof(message) - 1] = '\0';
    (void)snprintf(out_message, capacity, "%s", message);

    if (result < 0 || result >= NYA_SYSTEM_STEAM_INIT_COUNT) return NYA_SYSTEM_STEAM_INIT_FAILED_GENERIC;
    if (result != NYA_SYSTEM_STEAM_INIT_OK) return (NYA_SteamInitResult)result;

    // manual dispatch, not SteamAPI_RunCallbacks: the automatic one delivers through C++ callback
    // objects registered by CCallback templates, which this engine has no way to declare. The two are
    // mutually exclusive, so nothing may call SteamAPI_RunCallbacks after this.
    SteamAPI_ManualDispatch_Init();
    _NYA_STEAMWORKS.pipe = SteamAPI_GetHSteamPipe();

    return NYA_SYSTEM_STEAM_INIT_OK;
}

NYA_INTERNAL void _nya_steamworks_disconnect(void) {
    SteamAPI_Shutdown();

    _NYA_STEAMWORKS = (_NYA_SteamworksState){ 0 };
}

NYA_INTERNAL void _nya_steamworks_run_callbacks(void) {
    if (_NYA_STEAMWORKS.pipe == 0) return;

    SteamAPI_ManualDispatch_RunFrame(_NYA_STEAMWORKS.pipe);

    _NYA_SteamCallbackMsg message = { 0 };

    /*
     * Bounded: a client flooding callbacks must not hold the frame.
     */
    // sixty-four is well past what a full lobby produces in one frame, and whatever is left waits for
    // the next one rather than being dropped.
    for (u32 drained = 0; drained < 64 && SteamAPI_ManualDispatch_GetNextCallback(_NYA_STEAMWORKS.pipe, &message); drained++) {
        if (message.callback == _NYA_STEAM_CALLBACK_CALL_COMPLETED && message.param != nullptr
            && message.param_size >= (s32)sizeof(_NYA_SteamCallCompleted)) {
            /*
             * The result of an asynchronous call: create a lobby, join one, search for them.
             */
            const _NYA_SteamCallCompleted* completed = (const _NYA_SteamCallCompleted*)message.param;

            // bounded by the largest result this module decodes, so a call whose result is bigger than
            // anything here is skipped rather than heap-allocated on a number Steam chose.
            u8 result[256] = { 0 };

            if (completed->param_size <= sizeof(result)) {
                bool failed = false;

                if (SteamAPI_ManualDispatch_GetAPICallResult(_NYA_STEAMWORKS.pipe, completed->call, result, (s32)completed->param_size,
                                                             completed->callback, &failed)
                    && !failed) {
                    nya_steam_on_callback((u32)completed->callback, result, completed->param_size);
                }
            }

            SteamAPI_ManualDispatch_FreeLastCallback(_NYA_STEAMWORKS.pipe);
            continue;
        }

        if (message.param != nullptr && message.param_size > 0) {
            nya_steam_on_callback((u32)message.callback, message.param, (u32)message.param_size);
        }

        // required after every GetNextCallback that returned true, before the next one.
        SteamAPI_ManualDispatch_FreeLastCallback(_NYA_STEAMWORKS.pipe);
    }
}

NYA_INTERNAL b8 _nya_steamworks_restart_if_necessary(u32 app_id) {
    return SteamAPI_RestartAppIfNecessary(app_id);
}

NYA_INTERNAL u64              _nya_steamworks_user_id(void) { return SteamAPI_ISteamUser_GetSteamID(SteamAPI_SteamUser_v023()); }
NYA_INTERNAL NYA_ConstCString _nya_steamworks_user_name(void) { return SteamAPI_ISteamFriends_GetPersonaName(SteamAPI_SteamFriends_v018()); }
NYA_INTERNAL NYA_ConstCString _nya_steamworks_friend_name(u64 user) { return SteamAPI_ISteamFriends_GetFriendPersonaName(SteamAPI_SteamFriends_v018(), user); }

NYA_INTERNAL b8 _nya_steamworks_lobby_create(u32 kind, u32 max_members) {
    // a zero handle is Steam refusing to even start the call, which is what a disconnected client does.
    return SteamAPI_ISteamMatchmaking_CreateLobby(SteamAPI_SteamMatchmaking_v009(), (s32)kind, (s32)max_members) != 0;
}

NYA_INTERNAL b8 _nya_steamworks_lobby_join(u64 lobby) {
    return SteamAPI_ISteamMatchmaking_JoinLobby(SteamAPI_SteamMatchmaking_v009(), lobby) != 0;
}

NYA_INTERNAL void _nya_steamworks_lobby_leave(u64 lobby) {
    SteamAPI_ISteamMatchmaking_LeaveLobby(SteamAPI_SteamMatchmaking_v009(), lobby);
}

NYA_INTERNAL b8 _nya_steamworks_lobby_list_request(NYA_ConstCString key, NYA_ConstCString value, u32 max_results) {
    ISteamMatchmaking* matchmaking = SteamAPI_SteamMatchmaking_v009();

    // the filters apply to the next RequestLobbyList and are cleared by it, so they go first.
    if (key != nullptr && value != nullptr) {
        // 0 is k_ELobbyComparisonEqual.
        SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(matchmaking, key, value, 0);
    }

    SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter(matchmaking, (s32)max_results);

    return SteamAPI_ISteamMatchmaking_RequestLobbyList(matchmaking) != 0;
}

NYA_INTERNAL u64 _nya_steamworks_lobby_list_at(u32 index) {
    return SteamAPI_ISteamMatchmaking_GetLobbyByIndex(SteamAPI_SteamMatchmaking_v009(), (s32)index);
}

NYA_INTERNAL NYA_ConstCString _nya_steamworks_lobby_data_get(u64 lobby, NYA_ConstCString key) {
    return SteamAPI_ISteamMatchmaking_GetLobbyData(SteamAPI_SteamMatchmaking_v009(), lobby, key);
}

NYA_INTERNAL b8 _nya_steamworks_lobby_data_set(u64 lobby, NYA_ConstCString key, NYA_ConstCString value) {
    return SteamAPI_ISteamMatchmaking_SetLobbyData(SteamAPI_SteamMatchmaking_v009(), lobby, key, value);
}

NYA_INTERNAL NYA_ConstCString _nya_steamworks_lobby_member_data_get(u64 lobby, u64 user, NYA_ConstCString key) {
    return SteamAPI_ISteamMatchmaking_GetLobbyMemberData(SteamAPI_SteamMatchmaking_v009(), lobby, user, key);
}

NYA_INTERNAL void _nya_steamworks_lobby_member_data_set(u64 lobby, NYA_ConstCString key, NYA_ConstCString value) {
    SteamAPI_ISteamMatchmaking_SetLobbyMemberData(SteamAPI_SteamMatchmaking_v009(), lobby, key, value);
}

NYA_INTERNAL u32 _nya_steamworks_lobby_member_count(u64 lobby) {
    s32 count = SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(SteamAPI_SteamMatchmaking_v009(), lobby);

    // Steam returns a signed count and zero for a lobby this client is not in; a negative one would be
    // a huge unsigned count to every caller above.
    return count > 0 ? (u32)count : 0;
}

NYA_INTERNAL u64 _nya_steamworks_lobby_member_at(u64 lobby, u32 index) {
    return SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(SteamAPI_SteamMatchmaking_v009(), lobby, (s32)index);
}

NYA_INTERNAL u64 _nya_steamworks_lobby_owner(u64 lobby) {
    return SteamAPI_ISteamMatchmaking_GetLobbyOwner(SteamAPI_SteamMatchmaking_v009(), lobby);
}

NYA_INTERNAL u32 _nya_steamworks_lobby_member_limit(u64 lobby) {
    s32 limit = SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit(SteamAPI_SteamMatchmaking_v009(), lobby);

    return limit > 0 ? (u32)limit : 0;
}

NYA_INTERNAL b8 _nya_steamworks_lobby_invite(u64 lobby, u64 user) {
    return SteamAPI_ISteamMatchmaking_InviteUserToLobby(SteamAPI_SteamMatchmaking_v009(), lobby, user);
}

NYA_INTERNAL b8 _nya_steamworks_overlay_invite_open(u64 lobby) {
    // Steam's call returns nothing and quietly does nothing when the overlay is off, so there is no way
    // to tell the two apart from here. True, and a game wanting certainty should offer a join secret
    // beside the button; see nya_steam_lobby_invite_open.
    SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(SteamAPI_SteamFriends_v018(), lobby);

    return true;
}

NYA_INTERNAL b8 _nya_steamworks_achievement_get(NYA_ConstCString name, OUT b8* out_unlocked) {
    bool unlocked = false;

    if (!SteamAPI_ISteamUserStats_GetAchievement(SteamAPI_SteamUserStats_v013(), name, &unlocked)) return false;

    *out_unlocked = unlocked;

    return true;
}

NYA_INTERNAL b8 _nya_steamworks_achievement_set(NYA_ConstCString name, b8 unlocked) {
    ISteamUserStats* stats = SteamAPI_SteamUserStats_v013();

    return unlocked ? SteamAPI_ISteamUserStats_SetAchievement(stats, name) : SteamAPI_ISteamUserStats_ClearAchievement(stats, name);
}

NYA_INTERNAL b8 _nya_steamworks_achievement_progress(NYA_ConstCString name, u32 current, u32 max) {
    return SteamAPI_ISteamUserStats_IndicateAchievementProgress(SteamAPI_SteamUserStats_v013(), name, current, max);
}

NYA_INTERNAL b8 _nya_steamworks_stat_get_int(NYA_ConstCString name, OUT s32* out_value) {
    return SteamAPI_ISteamUserStats_GetStatInt32(SteamAPI_SteamUserStats_v013(), name, out_value);
}

NYA_INTERNAL b8 _nya_steamworks_stat_set_int(NYA_ConstCString name, s32 value) {
    return SteamAPI_ISteamUserStats_SetStatInt32(SteamAPI_SteamUserStats_v013(), name, value);
}

NYA_INTERNAL b8 _nya_steamworks_stat_get_float(NYA_ConstCString name, OUT f32* out_value) {
    return SteamAPI_ISteamUserStats_GetStatFloat(SteamAPI_SteamUserStats_v013(), name, out_value);
}

NYA_INTERNAL b8 _nya_steamworks_stat_set_float(NYA_ConstCString name, f32 value) {
    return SteamAPI_ISteamUserStats_SetStatFloat(SteamAPI_SteamUserStats_v013(), name, value);
}

NYA_INTERNAL b8 _nya_steamworks_stats_store(void) {
    return SteamAPI_ISteamUserStats_StoreStats(SteamAPI_SteamUserStats_v013());
}

NYA_INTERNAL b8 _nya_steamworks_cloud_enabled(void) {
    ISteamRemoteStorage* storage = SteamAPI_SteamRemoteStorage_v016();

    // both switches: the player may turn the Cloud off for the account or for this game alone, and
    // writing while either is off puts the file somewhere Steam will never sync.
    return SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount(storage) && SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp(storage);
}

NYA_INTERNAL b8 _nya_steamworks_cloud_quota(OUT u64* out_total, OUT u64* out_available) {
    return SteamAPI_ISteamRemoteStorage_GetQuota(SteamAPI_SteamRemoteStorage_v016(), out_total, out_available);
}

NYA_INTERNAL b8 _nya_steamworks_cloud_exists(NYA_ConstCString name) {
    return SteamAPI_ISteamRemoteStorage_FileExists(SteamAPI_SteamRemoteStorage_v016(), name);
}

NYA_INTERNAL u64 _nya_steamworks_cloud_size(NYA_ConstCString name) {
    s32 size = SteamAPI_ISteamRemoteStorage_GetFileSize(SteamAPI_SteamRemoteStorage_v016(), name);

    return size > 0 ? (u64)size : 0;
}

NYA_INTERNAL b8 _nya_steamworks_cloud_write(NYA_ConstCString name, const u8* data, u32 size) {
    return SteamAPI_ISteamRemoteStorage_FileWrite(SteamAPI_SteamRemoteStorage_v016(), name, data, (s32)size);
}

NYA_INTERNAL s32 _nya_steamworks_cloud_read(NYA_ConstCString name, OUT u8* out_data, u32 capacity) {
    return SteamAPI_ISteamRemoteStorage_FileRead(SteamAPI_SteamRemoteStorage_v016(), name, out_data, (s32)capacity);
}

NYA_INTERNAL b8 _nya_steamworks_cloud_delete(NYA_ConstCString name) {
    return SteamAPI_ISteamRemoteStorage_FileDelete(SteamAPI_SteamRemoteStorage_v016(), name);
}

NYA_INTERNAL b8 _nya_steamworks_rich_presence_set(NYA_ConstCString key, NYA_ConstCString value) {
    return SteamAPI_ISteamFriends_SetRichPresence(SteamAPI_SteamFriends_v018(), key, value);
}

NYA_INTERNAL void _nya_steamworks_rich_presence_clear(void) {
    SteamAPI_ISteamFriends_ClearRichPresence(SteamAPI_SteamFriends_v018());
}

NYA_INTERNAL b8 _nya_steamworks_p2p_send(u64 user, const u8* data, u32 size, b8 reliable, u32 channel) {
    _NYA_SteamNetworkingIdentity remote = _nya_steamworks_identity(user);

    s32 flags = (reliable ? _NYA_STEAM_SEND_RELIABLE : _NYA_STEAM_SEND_UNRELIABLE) | _NYA_STEAM_SEND_AUTO_RESTART;

    // k_EResultOK. Everything else is a session that is not there, a message too large, or a peer that
    // has blocked this account; all of them are the caller's peer being unusable rather than a crash.
    return SteamAPI_ISteamNetworkingMessages_SendMessageToUser(SteamAPI_SteamNetworkingMessages_SteamAPI_v002(), &remote, data, size, flags, (s32)channel)
        == _NYA_STEAM_RESULT_OK;
}

NYA_INTERNAL u32 _nya_steamworks_p2p_receive(u32 channel, OUT NYA_SteamMessage* out_messages, u32 capacity) {
    nya_assert(capacity <= NYA_STEAM_MAX_RECEIVE);

    _NYA_SteamNetworkingMessage* messages[NYA_STEAM_MAX_RECEIVE] = { nullptr };

    s32 taken = SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(SteamAPI_SteamNetworkingMessages_SteamAPI_v002(), (s32)channel, messages,
                                                                           (s32)capacity);

    if (taken <= 0) return 0;

    // Steam promised at most `capacity`; a larger answer would already have written past the array
    // above, so this is a check on the library rather than on the caller.
    nya_assert((u32)taken <= capacity, "Steam returned %d messages for a request of %u", taken, capacity);

    _NYA_STEAMWORKS.receive_used = 0;

    u32 kept = 0;

    for (s32 i = 0; i < taken; i++) {
        _NYA_SteamNetworkingMessage* message = messages[i];
        if (message == nullptr) continue;

        // the payload came off the network from another player's client. copied into this module's own
        // buffer, so the SDK's allocation is released inside this call and nothing above holds a
        // pointer into it.
        b8 addressable = message->peer.type == _NYA_STEAM_IDENTITY_TYPE_STEAM_ID && message->peer.steam_id != 0;
        b8 fits        = message->size > 0 && message->size <= (s32)NYA_STEAM_MAX_MESSAGE
                  && _NYA_STEAMWORKS.receive_used + (u32)message->size <= sizeof(_NYA_STEAMWORKS.receive_buffer);

        if (addressable && fits && message->data != nullptr) {
            u8* copy = _NYA_STEAMWORKS.receive_buffer + _NYA_STEAMWORKS.receive_used;

            nya_memcpy(copy, message->data, (u64)message->size);
            _NYA_STEAMWORKS.receive_used += (u32)message->size;

            out_messages[kept] = (NYA_SteamMessage){
                .sender   = { .value = message->peer.steam_id },
                .data     = copy,
                .size     = (u32)message->size,
                .reliable = (message->flags & _NYA_STEAM_SEND_RELIABLE) != 0,
            };

            kept++;
        }

        // released whether it was kept or dropped: the SDK counts a reference per message and leaks
        // the buffer otherwise.
        if (message->release != nullptr) message->release(message);
    }

    return kept;
}

NYA_INTERNAL b8 _nya_steamworks_p2p_accept(u64 user) {
    _NYA_SteamNetworkingIdentity remote = _nya_steamworks_identity(user);

    return SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(SteamAPI_SteamNetworkingMessages_SteamAPI_v002(), &remote);
}

NYA_INTERNAL void _nya_steamworks_p2p_close(u64 user) {
    _NYA_SteamNetworkingIdentity remote = _nya_steamworks_identity(user);

    (void)SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(SteamAPI_SteamNetworkingMessages_SteamAPI_v002(), &remote);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE TABLE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL const NYA_SteamBackend _NYA_STEAMWORKS_BACKEND = {
    .name = "steamworks",

    .connect              = &_nya_steamworks_connect,
    .disconnect           = &_nya_steamworks_disconnect,
    .run_callbacks        = &_nya_steamworks_run_callbacks,
    .restart_if_necessary = &_nya_steamworks_restart_if_necessary,

    .user_id     = &_nya_steamworks_user_id,
    .user_name   = &_nya_steamworks_user_name,
    .friend_name = &_nya_steamworks_friend_name,

    .lobby_create       = &_nya_steamworks_lobby_create,
    .lobby_join         = &_nya_steamworks_lobby_join,
    .lobby_leave        = &_nya_steamworks_lobby_leave,
    .lobby_list_request = &_nya_steamworks_lobby_list_request,
    .lobby_list_at      = &_nya_steamworks_lobby_list_at,

    .lobby_data_get        = &_nya_steamworks_lobby_data_get,
    .lobby_data_set        = &_nya_steamworks_lobby_data_set,
    .lobby_member_data_get = &_nya_steamworks_lobby_member_data_get,
    .lobby_member_data_set = &_nya_steamworks_lobby_member_data_set,

    .lobby_member_count = &_nya_steamworks_lobby_member_count,
    .lobby_member_at    = &_nya_steamworks_lobby_member_at,
    .lobby_owner        = &_nya_steamworks_lobby_owner,
    .lobby_member_limit = &_nya_steamworks_lobby_member_limit,

    .lobby_invite        = &_nya_steamworks_lobby_invite,
    .overlay_invite_open = &_nya_steamworks_overlay_invite_open,

    .achievement_get      = &_nya_steamworks_achievement_get,
    .achievement_set      = &_nya_steamworks_achievement_set,
    .achievement_progress = &_nya_steamworks_achievement_progress,
    .stat_get_int         = &_nya_steamworks_stat_get_int,
    .stat_set_int         = &_nya_steamworks_stat_set_int,
    .stat_get_float       = &_nya_steamworks_stat_get_float,
    .stat_set_float       = &_nya_steamworks_stat_set_float,
    .stats_store          = &_nya_steamworks_stats_store,

    .cloud_enabled = &_nya_steamworks_cloud_enabled,
    .cloud_quota   = &_nya_steamworks_cloud_quota,
    .cloud_exists  = &_nya_steamworks_cloud_exists,
    .cloud_size    = &_nya_steamworks_cloud_size,
    .cloud_write   = &_nya_steamworks_cloud_write,
    .cloud_read    = &_nya_steamworks_cloud_read,
    .cloud_delete  = &_nya_steamworks_cloud_delete,

    .rich_presence_set   = &_nya_steamworks_rich_presence_set,
    .rich_presence_clear = &_nya_steamworks_rich_presence_clear,

    .p2p_send    = &_nya_steamworks_p2p_send,
    .p2p_receive = &_nya_steamworks_p2p_receive,
    .p2p_accept  = &_nya_steamworks_p2p_accept,
    .p2p_close   = &_nya_steamworks_p2p_close,
};

const NYA_SteamBackend* _nya_steam_backend_default(void) {
    return &_NYA_STEAMWORKS_BACKEND;
}
