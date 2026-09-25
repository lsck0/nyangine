#include "nyangine-core/nyangine.h"

// PUBLIC API IMPLEMENTATION

NYA_Error nya_net_port_pick(NYA_NetProtocol protocol, OUT u16* out_port) {
    nya_assert(out_port != nullptr);
    nya_assert(protocol < NYA_NET_PROTOCOL_COUNT, "unknown protocol %d", (int)protocol);

    *out_port = 0;

    if (nya_os_socket_start() != NYA_OS_SOCKET_OK) return nya_error(NYA_ERROR_IO, "the host's socket library could not start");
    defer nya_os_socket_stop();

    // Bind port zero, read back the free number, close: racy, for callers (a test) that must know the port beforehand.
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
