/**
 * @file net_port.h
 *
 * Asking the system which port is free instead of guessing one.
 *
 * ```c
 * u16 port = 0;
 * NYA_TRY(nya_net_port_pick(NYA_NET_PROTOCOL_TCP, &port));
 * ```
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_NetProtocol NYA_NetProtocol;

/** Which port space a number is asked for in: TCP and UDP number theirs separately. */
enum NYA_NetProtocol {
    NYA_NET_PROTOCOL_UDP = 0,
    NYA_NET_PROTOCOL_TCP,

    NYA_NET_PROTOCOL_COUNT,
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * A free local port, chosen by the system: a socket is bound to port zero, the number the kernel put on it
 * is read back, and the socket is closed again. Nothing else on the machine holds that number while this
 * returns, which a scan over a fixed window of ports cannot promise — two copies of a suite running side by
 * side scan the same window and land on the same number.
 *
 * Public rather than internal to net_udp.c, which is its first caller through
 * `nya_net_transport_listen(transport, 0)`, because a listener whose port someone else validates has to ask
 * the question itself: the HTTP server refuses a zero port in its config, so its test asks here and hands
 * over a real number. SDL_net reports no call for the port a socket ended up bound to, so this is the only
 * way to learn one.
 * */
NYA_API NYA_Error nya_net_port_pick(NYA_NetProtocol protocol, OUT u16* out_port) __attr_no_discard;
