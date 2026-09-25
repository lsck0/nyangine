/**
 * @file plugins.h
 * */
#pragma once

#ifdef NYA_PLUGIN_CURL
#include "nyangine-core/plugins/curl/request.h"
// After request.h: a websocket's options reuse NYA_RequestHeader rather than restating it.
#include "nyangine-core/plugins/curl/websocket.h"
// The Discord bot client, curl's dependent, not NYA_PLUGIN_DISCORD's (that flag is the GameSDK below). See discord_bot/discord_gateway.h.
#include "nyangine-core/plugins/discord_bot/discord_gateway.h"
#include "nyangine-core/plugins/discord_bot/discord_rest.h"
// The Telegram bot client, curl's dependent too; a polling shape rather than a socket, so a separate client. See telegram.h.
#include "nyangine-core/plugins/telegram_bot/telegram.h"
// The Twitch bot's ear: nothing is pushed until subscribed to, and a subscription is made over HTTP against the session this hands out. See twitch_eventsub.h.
#include "nyangine-core/plugins/twitch_bot/twitch_eventsub.h"
// After the socket: the calls that subscribe it to something and answer over it. See twitch_helix.h.
#include "nyangine-core/plugins/twitch_bot/twitch_helix.h"
// A relying party for "log in with X": the authorization code flow with PKCE, curl's dependent too. See oidc.h.
#include "nyangine-core/plugins/oidc/oidc.h"
#endif

// Always, like steam below: needs nothing vendored or linked; spawns `gpg` when present and answers "not available" otherwise. See pgp.h.
#include "nyangine-core/plugins/pgp/pgp.h"

// Always: a facade over a backend table; only the backend is behind NYA_PLUGIN_STEAM, so a build without Steamworks answers "not supported". See steam.h.
#include "nyangine-core/plugins/steam/steam.h"

#ifdef NYA_PLUGIN_DISCORD
#include "nyangine-core/plugins/discord/discord.h"
#endif

#ifdef NYA_PLUGIN_LUA
#include "nyangine-core/plugins/lua/lua.h"
#endif
