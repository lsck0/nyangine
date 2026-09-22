/**
 * @file net.h
 *
 * ```
 * single player      server, nobody listening, one local player
 * open to LAN        the same server, now listening; the world does not change
 * dedicated server   the same server, headless, no local player          (--server)
 * joining a game     client                                              (--connect host:port)
 * ```
 * */
#pragma once

#include "nyangine/net/net_types.h"
/**/
// net_bytes.h is not included: its codecs are NYA_INTERNAL, so every translation unit including
// nyangine.h without nyangine.c (the game DLL) would warn about static declarations never defined.
// The .c files that need it include it themselves.
#include "nyangine/net/net_command.h"
#include "nyangine/net/net_config.h"
#include "nyangine/net/net_crypto.h"
#include "nyangine/net/net_message.h"
#include "nyangine/net/net_port.h"
#include "nyangine/net/net_transport.h"
// Names NYA_Entity, so it pulls core_entity.h in on its own rather than relying on nyangine.h's order.
#include "nyangine/net/net_snapshot.h"
/**/
// The two halves of the architecture. Last: each names the transport, the snapshot and the command.
#include "nyangine/net/net_client.h"
#include "nyangine/net/net_server.h"
/**/
// Last of all: chat is a layer over the event channel both of those provide, and names their sends.
#include "nyangine/net/net_chat.h"
