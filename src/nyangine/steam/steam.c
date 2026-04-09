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
extern b8                  SteamAPI_IsSteamRunning(void);
extern b8                  SteamAPI_RestartAppIfNecessary(u32 unOwnAppID);

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

NYA_SteamInitResult nya_system_steam_init(OUT NYA_CString err_msg) {
    SteamErrMsg         raw_err = { 0 };
    ESteamAPIInitResult result  = SteamAPI_InitFlat(&raw_err);

    if (err_msg != nullptr && result != NYA_SYSTEM_STEAM_INIT_OK) {
        for (s32 i = 0; i < 1024 && raw_err[i] != '\0'; i++) err_msg[i] = raw_err[i];
    }

    return (NYA_SteamInitResult)result;
}

void nya_system_steam_deinit(void) {
    SteamAPI_Shutdown();
}

/*
 * ─────────────────────────────────────────────────────────
 * STEAM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */
