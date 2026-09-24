/**
 * @file net.h
 *
 * The wire: a datagram to a peer, encrypted and authenticated, over whichever transport is available.
 * Nothing here knows what a datagram carries.
 *
 * ```
 * net_types.h      the vocabulary: peer ids, the bounds, the roles, the disconnect reasons
 * net_crypto.h     the long term identities and the X25519 handshake keys are derived from
 * net_message.h    one reliable, ordered, encrypted message stream between two endpoints
 * net_transport.h  the seam every transport satisfies: UDP, Steam's relay and an in-process loopback
 * net_config.h     the launch configuration and the join secret a friend list carries
 * net_port.h       picking a port, and saying which one was actually bound
 * ```
 *
 * The other half of networking is `replicate`, which is a world crossing this wire: snapshots,
 * commands and prediction, all of them written in entities. That half sits above the app loop and this
 * one sits below it. See replicate.h, which says why the line is where it is.
 *
 * A program that only needs to send bytes to a peer — a tool, a relay, a service — links this and
 * nothing above it.
 * */
#pragma once

#include "nyangine/net/net_types.h"
/**/
// net_bytes.h is not included here: its NYA_INTERNAL codecs would warn as undefined statics; the .c files include it themselves.
#include "nyangine/net/net_config.h"
#include "nyangine/net/net_crypto.h"
#include "nyangine/net/net_message.h"
#include "nyangine/net/net_port.h"
#include "nyangine/net/net_transport.h"
