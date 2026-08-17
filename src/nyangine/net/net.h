/**
 * @file net.h
 *
 * Authoritative client/server multiplayer: one world that is right, and any number of views of it.
 *
 * ## The shape
 *
 * There is exactly one authoritative simulation and it belongs to **the server**. A **client** has a
 * world too, but that world is a *replica*: it is written by snapshots rather than by simulation,
 * except for the one entity the local player predicts forward. Everything below follows from that.
 *
 * Single player is not a special case and not a separate mode. It is a server with no remote peers:
 *
 * ```
 * single player      server, nobody listening, one local player
 * open to LAN        the same server, now listening — nothing about the world changes
 * dedicated server   the same server, headless, no local player          (--server)
 * joining a game     client                                              (--connect host:port)
 * ```
 *
 * That is the Minecraft and Terraria arrangement, and the reason for it is not elegance: it means a
 * bug cannot exist in multiplayer and not in single player, because there is one code path. It is
 * also what makes "open to LAN" a runtime call rather than a different executable.
 *
 * ## One executable
 *
 * There is no separate server binary. `--server` selects a dedicated server, and the process then
 * runs headless with no window and no renderer. See nya_net_config_from_args.
 *
 * ## What single player costs
 *
 * Nearly nothing, by construction rather than by optimisation:
 *
 * - **No snapshots are produced at all** when there are no remote peers. Snapshotting is driven by
 *   the set of clients that need one, and an empty set means the server simulates and stops.
 * - **The local player is not predicted.** Prediction exists to hide latency; on a listen server the
 *   client reads the server's own world and there is no latency to hide. See
 *   nya_net_transport_is_local.
 * - **Nothing is serialised.** The loopback transport hands buffers between endpoints in one
 *   process; there is no encode/decode pair on the local path.
 *
 * So the cost of the netcode in single player is a branch per tick asking whether anyone is
 * listening.
 *
 * ## What is sent, and how
 *
 * - **Server to client: snapshots**, unreliable, every tick. A snapshot is a complete statement of
 *   the world at a tick, delta-compressed against the newest one that client has acknowledged. A
 *   lost snapshot is superseded rather than resent — resending would deliver stale truth after
 *   fresher truth had already arrived.
 * - **Client to server: commands**, unreliable, every tick. What the player is *trying* to do, as
 *   actions and aim rather than as positions — a client never tells the server where it is. Each
 *   packet repeats the last several ticks of commands, so a lost packet loses nothing.
 * - **Everything else: events**, reliable and ordered. The handshake, the world description, a chat
 *   line, a player joining or leaving.
 *
 * ## Prediction, and its honest limit
 *
 * A client applies its own commands immediately rather than waiting a round trip to see the result,
 * and reconciles when the server's answer arrives: if the authoritative state at tick T differs from
 * what was predicted at T, the client snaps to it and replays every command since.
 *
 * **Only the local player is predicted.** Not the world. Rolling back a whole Box2D or Box3D world
 * per correction is not something either solver can do cheaply, and pretending otherwise is how a
 * netcode becomes a rewrite of the physics engine. This is the same choice Source and Overwatch
 * make, and it is why prediction runs through a game-supplied movement function — see
 * nya_net_prediction_set — rather than through the solver.
 *
 * Everything that is not the local player is interpolated between the last two snapshots, which is
 * smooth and slightly in the past, and which is what every player has always been looking at.
 * */
#pragma once

#include "nyangine/net/net_types.h"
/**/
// net_bytes.h is deliberately absent: the byte codecs are NYA_INTERNAL, so declaring them here would
// hand a static-and-never-defined declaration to every translation unit that includes nyangine.h
// without nyangine.c — the game DLL, chiefly — and each one warns about it. The .c files that use
// them include it themselves.
#include "nyangine/net/net_command.h"
#include "nyangine/net/net_config.h"
#include "nyangine/net/net_message.h"
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
