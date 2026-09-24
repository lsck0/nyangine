#ifdef NYA_PLUGIN_CURL
#include "nyangine/plugins/curl/request.c"
// After request.c: it owns the one curl_global_init for the process.
#include "nyangine/plugins/curl/websocket.c"
// After the websocket, which its default transport is written in terms of.
#include "nyangine/plugins/discord_bot/discord_gateway.c"
// After the gateway, whose token check and bounded copy it reuses rather than writing a second pair.
#include "nyangine/plugins/discord_bot/discord_rest.c"
#include "nyangine/plugins/telegram_bot/telegram.c"
#include "nyangine/plugins/twitch_bot/twitch_eventsub.c"
#include "nyangine/plugins/twitch_bot/twitch_helix.c"
#include "nyangine/plugins/oidc/oidc.c"
#endif

#include "nyangine/plugins/pgp/pgp.c"

// Always, per plugins.h. steam.c includes steam_steamworks.c itself under NYA_PLUGIN_STEAM, so the flat API symbols are named only in a build that links the library.
#include "nyangine/plugins/steam/steam.c"

#ifdef NYA_PLUGIN_DISCORD
#include "nyangine/plugins/discord/discord.c"
#endif

#ifdef NYA_PLUGIN_LUA
#include "nyangine/plugins/lua/lua.c"
// After lua.c: the engine table reaches core and entity APIs a host tool build lacks, so it is gated with the rest of the plugin.
#ifndef NYA_NO_SDL
#include "nyangine/plugins/lua/lua_engine.c"
#endif
#endif
