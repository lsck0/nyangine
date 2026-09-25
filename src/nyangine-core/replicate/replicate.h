/**
 * @file replicate.h
 *
 * A world on the wire: what the server has, what the client predicts, and the difference between them.
 *
 * ```
 * single player      server, nobody listening, one local player
 * open to LAN        the same server, now listening; the world does not change
 * dedicated server   the same server, headless, no local player          (--server)
 * joining a game     client                                              (--connect host:port)
 * ```
 *
 * ```
 * replicate_command.h   one tick of a player's intent, and the function that turns it into movement
 * replicate_snapshot.h  the replicated world at one tick, the delta of two, and who stands for whom
 * replicate_server.h    the authority: admits players, applies commands, sends snapshots, rewinds
 * replicate_client.h    the other side: predicts, reconciles, and interpolates what it does not own
 * replicate_chat.h      a layer over the event channel both of those provide
 * ```
 *
 * ── why this is not in `net` ──
 *
 * `net` is a datagram to a peer: a transport, an encrypted session, a reliable stream. None of it names
 * an entity, and a program that only sends bytes — a tool, a relay, a service — should link that and
 * stop there. Everything here is written in entities instead: a snapshot is captured out of the world
 * and applied back into it, a command moves an entity, prediction rolls one back and replays it. So the
 * two halves sit on opposite sides of the app loop, and `net` moved below it while this stayed above.
 *
 * A directory under `net/` would not have said that. The module order (src/nyangine-build/lint.c) reads a
 * module's name off the first path segment under `src/nyangine/`, so a subdirectory is still `net` to
 * the rule that enforces the order, and the split would be a naming convention rather than something
 * the build refuses to let drift. It is a module because the line between the two halves is a rule.
 *
 * ── why the names are still nya_net_ ──
 *
 * A module's name is where its files live, not a prefix every symbol it declares has to wear: `core`
 * declares nya_entity_ and nya_app_, `renderer` declares nya_render2d_ and nya_gpu_. These functions
 * are the network as a player and a caller meet it — `nya_net_client_connect` connects to a game
 * server — and renaming fifty of them across gnyame, the examples and the tests would cost every
 * caller a change and buy nobody a better name.
 *
 * ── the frame ──
 *
 * The server's tick and the client's tick are engine systems named `net_server` and `net_client`, both
 * `after` the entity system, registered by nya_net_server_start and nya_net_client_attach and removed
 * again when either goes away. They are registered here rather than in core_app.c's list because this
 * module sits above the app loop: core drives what is below it, and what is above it drives itself.
 *
 * A program with no app loop calls nya_net_server_tick and nya_net_client_tick itself, which is what
 * every test in tests/nyangine/replicate does.
 * */
#pragma once

#include "nyangine-core/replicate/replicate_command.h"
// Names NYA_Entity, so it pulls core_entity.h in on its own rather than relying on nyangine.h's order.
#include "nyangine-core/replicate/replicate_snapshot.h"
/**/
// The two halves of the architecture. Last: each names the transport, the snapshot and the command.
#include "nyangine-core/replicate/replicate_client.h"
#include "nyangine-core/replicate/replicate_server.h"
/**/
// Last of all: chat is a layer over the event channel both of those provide, and names their sends.
#include "nyangine-core/replicate/replicate_chat.h"
