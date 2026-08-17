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

/*
 * `err_msg_capacity` is not optional, and the result is always terminated.
 *
 * This took a bare NYA_CString and copied up to 1024 bytes into it with no idea how big it was, so
 * the only safe call was one passing a buffer of exactly SteamErrMsg's size — which the header did
 * not say and could not enforce. It also stopped at the source's NUL without ever writing one, so a
 * caller whose buffer was not already zeroed got an unterminated string back and the next thing to
 * read it ran off the end.
 *
 * Not reachable today: steam.c is in no translation unit — nyangine.c does not include it — so none
 * of this has ever been compiled. Fixed rather than left, because the file is here to be switched
 * on later and a buffer bug is a poor thing to hand the person who does it.
 */
NYA_SteamInitResult nya_system_steam_init(OUT NYA_CString err_msg, u64 err_msg_capacity) {
    SteamErrMsg         raw_err = { 0 };
    ESteamAPIInitResult result  = SteamAPI_InitFlat(&raw_err);

    if (err_msg != nullptr && err_msg_capacity > 0) {
        u64 copied = 0;
        while (copied + 1 < err_msg_capacity && copied < sizeof(SteamErrMsg) && raw_err[copied] != '\0') {
            err_msg[copied] = raw_err[copied];
            copied++;
        }
        err_msg[copied] = '\0';
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
