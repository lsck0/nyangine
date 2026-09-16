#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STEAM TRANSPORT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * A stub, deliberately, and a stub with a shape.
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
     */
    return nya_error(NYA_ERROR_NOT_SUPPORTED, "the Steam transport is not implemented yet; use the UDP transport");
#endif
}
