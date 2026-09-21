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
// After sql.h: a table binds a described type to a connection and takes its key as an NYA_SqlValue.
#include "nyangine/plugins/sqlite/orm.h"
#endif

// Always: the module is a facade over a backend table, and only the backend is behind
// NYA_PLUGIN_STEAM. A build without the Steamworks library installs no backend and every call answers
// "not supported", which is what lets a game call it with no #ifdef of its own. See steam.h.
#include "nyangine/plugins/steam/steam.h"

#ifdef NYA_PLUGIN_DISCORD
#include "nyangine/plugins/discord/discord.h"
#endif

#ifdef NYA_PLUGIN_LUA
#include "nyangine/plugins/lua/lua.h"
#endif
