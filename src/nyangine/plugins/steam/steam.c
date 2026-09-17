#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STEAM SDK FLAT API (C-compatible exports from steam_api library)
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef s32  ESteamAPIInitResult;
typedef char SteamErrMsg[1024];

extern ESteamAPIInitResult SteamAPI_InitFlat(SteamErrMsg* pOutErrMsg);
extern void                SteamAPI_Shutdown(void);
extern void                SteamAPI_RunCallbacks(void);
extern bool                SteamAPI_RestartAppIfNecessary(u32 unOwnAppID);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL b8 _nya_steam_connected = false;

NYA_INTERNAL NYA_ConstCString _NYA_STEAM_INIT_RESULT_NAME[NYA_SYSTEM_STEAM_INIT_COUNT] = {
    [NYA_SYSTEM_STEAM_INIT_OK]               = "ok",
    [NYA_SYSTEM_STEAM_INIT_FAILED_GENERIC]   = "failed",
    [NYA_SYSTEM_STEAM_INIT_NO_STEAM_CLIENT]  = "no client",
    [NYA_SYSTEM_STEAM_INIT_VERSION_MISMATCH] = "client out of date",
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

b8 nya_system_steam_restart_if_necessary(u32 app_id) {
    nya_assert(app_id != 0);

    return SteamAPI_RestartAppIfNecessary(app_id);
}

NYA_SteamInitResult nya_system_steam_init(void) {
    nya_assert(!_nya_steam_connected);

    SteamErrMsg message = { 0 };
    s32         result  = SteamAPI_InitFlat(&message);
    message[sizeof(message) - 1] = '\0';

    // a result this SDK does not name reads as a generic failure rather than indexing past the table.
    if (result < 0 || result >= NYA_SYSTEM_STEAM_INIT_COUNT) result = NYA_SYSTEM_STEAM_INIT_FAILED_GENERIC;

    if (result != NYA_SYSTEM_STEAM_INIT_OK) {
        nya_log_warn("Steam is unavailable (%s), continuing without it: %s", _NYA_STEAM_INIT_RESULT_NAME[result], message);
        return (NYA_SteamInitResult)result;
    }

    _nya_steam_connected = true;
    nya_log_info("Connected to Steam.");

    return NYA_SYSTEM_STEAM_INIT_OK;
}

void nya_system_steam_update(void) {
    if (!_nya_steam_connected) return;

    SteamAPI_RunCallbacks();
}

void nya_system_steam_deinit(void) {
    if (!_nya_steam_connected) return;

    SteamAPI_Shutdown();
    _nya_steam_connected = false;
}

/*
 * ─────────────────────────────────────────────────────────
 * STEAM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

b8 nya_steam_is_connected(void) {
    return _nya_steam_connected;
}
