/**
 * @file plugins.h
 * */
#pragma once

#ifdef NYA_PLUGIN_CURL
#include "nyangine/plugins/curl/request.h"
// After request.h: a websocket's options reuse NYA_RequestHeader rather than restating it.
#include "nyangine/plugins/curl/websocket.h"
// The Discord bot client, which is curl's dependent and not NYA_PLUGIN_DISCORD's: that flag is the
// GameSDK below, which a bot has nothing to do with. See discord_bot/discord_gateway.h.
#include "nyangine/plugins/discord_bot/discord_gateway.h"
#include "nyangine/plugins/discord_bot/discord_rest.h"
// The Telegram bot client, curl's dependent for the same reason. A different protocol shape — polling
// rather than a socket — so a separate client rather than one with two backends. See telegram.h.
#include "nyangine/plugins/telegram_bot/telegram.h"
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
