/**
 * @file plugins.h
 * */
#pragma once

#ifdef NYA_PLUGIN_CURL
#include "nyangine/plugins/curl/request.h"
// After request.h: a websocket's options reuse NYA_RequestHeader rather than restating it.
#include "nyangine/plugins/curl/websocket.h"
#endif

#ifdef NYA_PLUGIN_SQLITE
#include "nyangine/plugins/sqlite/sql.h"
#endif

#ifdef NYA_PLUGIN_STEAM
#include "nyangine/plugins/steam/steam.h"
#endif

#ifdef NYA_PLUGIN_DISCORD
#include "nyangine/plugins/discord/discord.h"
#endif

#ifdef NYA_PLUGIN_LUA
#include "nyangine/plugins/lua/lua.h"
#endif
