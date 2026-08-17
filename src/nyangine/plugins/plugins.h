/**
 * @file plugins.h
 *
 * Optional modules that wrap a vendored third party library, each behind its own flag.
 *
 * A plugin is not part of the engine the way base or core are. It exists because some games want
 * HTTP or a database and most do not, and because the library behind it is a real cost: link time,
 * binary size, a system dependency, an attack surface. Nothing here is compiled unless asked for.
 *
 * | Flag               | Brings in                | Needs                                        |
 * | ---                | ---                      | ---                                          |
 * | NYA_PLUGIN_CURL    | plugins/curl/request.h   | libcurl, and a TLS stack                     |
 * | NYA_PLUGIN_SQLITE  | plugins/sqlite/sql.h     | libsqlite3, plus the sqlean and sqlite-vec archives |
 * | NYA_PLUGIN_STEAM   | plugins/steam/steam.h    | the Steamworks redistributable, shipped beside the binary |
 * | NYA_PLUGIN_DISCORD | plugins/discord/discord.h | nothing; it speaks Discord's local IPC protocol directly |
 *
 * The sqlite plugin registers both extension bundles on every connection it opens, so a database
 * gets vector search and sqlean's functions with no further opting in. See sql.h.
 *
 * They live outside base for a reason beyond taste. base.c is a unity aggregate that the build
 * system itself compiles, and the build system is what *builds* libcurl and libsqlite3 — on a fresh
 * checkout neither library exists yet. A base that reached for them could never bootstrap. As
 * plugins they are simply off for the build tool, which is the honest description of the situation
 * rather than a workaround for it.
 *
 * Both wrap their results in NYA_Object, so a row from a query and a JSON response body are the
 * same type, and serde can move either to disk without a conversion in between.
 * */
#pragma once

#ifdef NYA_PLUGIN_CURL
#include "nyangine/plugins/curl/request.h"
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
