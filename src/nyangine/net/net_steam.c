#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STEAM TRANSPORT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * A stub, deliberately, and a stub with a shape.
 *
 * What this will be is Steam's ISteamNetworkingSockets: relayed peer-to-peer connections that reach
 * a player behind a NAT without port forwarding, which is the entire reason to prefer it over the
 * UDP transport for anything but a LAN. Steam's relay network does the hole punching and, failing
 * that, carries the traffic itself.
 *
 * It is a stub because the Steamworks SDK is not on the link line — see plugins/steam/steam.h, which
 * documents the flags it needs and why no build rule names them yet. Writing the implementation
 * against a library that is not linked would produce code no compiler has ever seen, which is the
 * state that file was in until this session.
 *
 * What exists here instead is the *seam*: nya_net_transport_steam_create is callable, reports
 * NYA_ERROR_NOT_SUPPORTED, and the server and client above are already written against it. So
 * turning it on is this file plus a build rule, and nothing above changes.
 *
 * ## What it will have to do differently from UDP
 *
 * Almost nothing at this layer, which is the point of the transport interface. Steam's own sockets
 * already provide reliability, ordering, fragmentation and duplicate suppression — so where net_udp.c
 * implements the contract at the top of net_transport.h, this file will mostly *forward* to it. The
 * work is in the parts UDP does not have: a lobby to join rather than an address to resolve, and a
 * SteamID rather than an address:port for a peer.
 */

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_transport_steam_create(NYA_Arena* arena, OUT NYA_NetTransport** out_transport) {
    nya_assert(arena != nullptr);
    nya_assert(out_transport != nullptr);

    *out_transport = nullptr;

#ifndef NYA_PLUGIN_STEAM
    return nya_error(NYA_ERROR_NOT_SUPPORTED, "the Steam transport needs -DNYA_PLUGIN_STEAM; see plugins/steam/steam.h");
#else
    /*
     * The plugin is compiled but the transport is not written yet.
     *
     * Reported as unsupported rather than asserted, because a game that offers "host via Steam" in a
     * menu should be able to grey the option out from this answer rather than crash on it. When this
     * file grows an implementation, that menu item starts working with no change above.
     */
    return nya_error(NYA_ERROR_NOT_SUPPORTED, "the Steam transport is not implemented yet; use the UDP transport");
#endif
}
