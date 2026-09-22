#include "nyangine/nyangine.h"

#include "SDL3_net/SDL_net.h"

/*
 * The one place in the engine that opens a socket without SDL_net. SDL_net has no call that reports the
 * port a socket was bound to, and port zero is answered by the kernel, so the only way to see the number
 * is to hold the socket ourselves for the length of one getsockname.
 */
#if OS_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

#if OS_WINDOWS
typedef SOCKET _NYA_NetRawSocket;
typedef int    _NYA_NetSockLength;
#define _NYA_NET_RAW_INVALID  INVALID_SOCKET
#define _nya_net_raw_close(s) (void)closesocket(s)
#else
typedef int       _NYA_NetRawSocket;
typedef socklen_t _NYA_NetSockLength;
#define _NYA_NET_RAW_INVALID  (-1)
#define _nya_net_raw_close(s) (void)close(s)
#endif

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_net_port_pick(NYA_NetProtocol protocol, OUT u16* out_port) {
    nya_assert(out_port != nullptr);
    nya_assert(protocol < NYA_NET_PROTOCOL_COUNT, "unknown protocol %d", (int)protocol);

    *out_port = 0;

    /*
     * Only for WSAStartup, which has to have run before socket() on Windows and which SDL_net owns here.
     * Reference counted, so this neither starts nor stops anything a transport already has up.
     */
    if (!NET_Init()) return nya_error(NYA_ERROR_NOT_OK, "SDL_net could not start: %s", SDL_GetError());
    defer NET_Quit();

    const int type = protocol == NYA_NET_PROTOCOL_TCP ? SOCK_STREAM : SOCK_DGRAM;

    _NYA_NetRawSocket handle = socket(AF_INET, type, 0);
    if (handle == _NYA_NET_RAW_INVALID) return nya_error(NYA_ERROR_IO, "could not open a socket to ask the system for a port");

    defer _nya_net_raw_close(handle);

    // every interface, because that is what a listener binds: a number free only on loopback is no answer.
    struct sockaddr_in wanted = {
        .sin_family      = AF_INET,
        .sin_port        = 0,
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(handle, (const struct sockaddr*)&wanted, sizeof(wanted)) != 0) {
        return nya_error(NYA_ERROR_IO, "could not bind port zero to ask the system for a port");
    }

    struct sockaddr_in  bound  = { 0 };
    _NYA_NetSockLength length = (_NYA_NetSockLength)sizeof(bound);

    if (getsockname(handle, (struct sockaddr*)&bound, &length) != 0) {
        return nya_error(NYA_ERROR_IO, "the system bound a port but would not say which");
    }

    const u16 port = ntohs(bound.sin_port);
    if (port == 0) return nya_error(NYA_ERROR_NOT_OK, "the system reported port zero for a bound socket");

    *out_port = port;

    return NYA_OK;
}
