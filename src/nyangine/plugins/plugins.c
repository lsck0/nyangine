#ifdef NYA_PLUGIN_CURL
#include "nyangine/plugins/curl/request.c"
#endif

#ifdef NYA_PLUGIN_SQLITE
#include "nyangine/plugins/sqlite/sql.c"
#endif

#ifdef NYA_PLUGIN_STEAM
#include "nyangine/plugins/steam/steam.c"
#endif

#ifdef NYA_PLUGIN_DISCORD
#include "nyangine/plugins/discord/discord.c"
#endif

#ifdef NYA_PLUGIN_LUA
#include "nyangine/plugins/lua/lua.c"
// After lua.c: the engine table is registered through nya_lua_register, and it reaches core and
// entity APIs that a host tool build does not have. Gated with the rest of the plugin.
#ifndef NYA_NO_SDL
#include "nyangine/plugins/lua/lua_engine.c"
#endif
#endif
