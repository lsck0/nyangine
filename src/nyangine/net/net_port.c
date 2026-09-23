#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_port_pick(NYA_NetProtocol protocol, OUT u16* out_port) {
    nya_assert(out_port != nullptr);
    nya_assert(protocol < NYA_NET_PROTOCOL_COUNT, "unknown protocol %d", (int)protocol);

    *out_port = 0;

    if (nya_os_socket_start() != NYA_OS_SOCKET_OK) return nya_error(NYA_ERROR_IO, "the host's socket library could not start");
    defer nya_os_socket_stop();

    /*
     * A socket on port zero, which the host answers with a free number, and then the socket is closed
     * again. The number can be taken by somebody else between the close and whatever binds it next —
     * that race is why a server binds zero itself and reads back what it got. This exists for the
     * callers that cannot: a test that has to know the port before the thing it is testing starts.
     */
    NYA_OsSocket socket = NYA_OS_SOCKET_NONE;

    NYA_OsSocketStatus opened = protocol == NYA_NET_PROTOCOL_TCP ? nya_os_socket_open(NYA_OS_SOCKET_LISTENER, 0, 0, &socket) : nya_os_socket_open(NYA_OS_SOCKET_DATAGRAM, 0, 0, &socket);

    if (opened != NYA_OS_SOCKET_OK) return nya_error(NYA_ERROR_IO, "could not open a socket to ask the system for a port");
    defer nya_os_socket_close(socket);

    NYA_OsAddress bound = { 0 };
    if (nya_os_socket_address(socket, &bound) != NYA_OS_SOCKET_OK) return nya_error(NYA_ERROR_IO, "the system bound a port but would not say which");

    if (bound.port == 0) return nya_error(NYA_ERROR_NOT_OK, "the system reported port zero for a bound socket");

    *out_port = bound.port;

    return NYA_OK;
}
