#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
// after winsock2.h, which has to be included before windows.h or the older winsock declarations win.
#include <windows.h>
#include <ws2tcpip.h>
// For SIO_UDP_CONNRESET (not in ws2tcpip.h), which stops one refused datagram taking a udp server down; mingw-w64 keeps it in mswsock.h, not mstcpip.h.
#include <mswsock.h>

#include <stdio.h>
#include <string.h>

#include "nyangine-std/os/os_socket.h"

static_assert(sizeof(SOCKET) <= sizeof(u64), "NYA_OsSocket must hold a SOCKET");

// INTERNAL

/**
 * How many callers asked for the library.
 *
 * WSAStartup is counted by the host as well, but WSACleanup on the last of them tears the library out
 * from under anything that kept a socket, so the count is kept here too and only the last stop calls
 * it. Not atomic: the header says sockets are opened from one thread, and the two callers that exist
 * — a server and a transport — start on the thread that starts them.
 * */
NYA_INTERNAL u32 _NYA_OS_SOCKET_STARTS = 0;

/** A SOCKET as a handle and back, one higher so that zero means no socket. INVALID_SOCKET is ~0 here. */
NYA_INTERNAL SOCKET _nya_os_socket_handle(NYA_OsSocket socket) {
    return socket.handle == 0 ? INVALID_SOCKET : (SOCKET)(socket.handle - 1);
}

NYA_INTERNAL NYA_OsSocket _nya_os_socket_of(SOCKET handle) {
    return (NYA_OsSocket){ .handle = handle == INVALID_SOCKET ? 0 : (u64)handle + 1 };
}

/** What the host's last error means to a caller here. */
NYA_INTERNAL NYA_OsSocketStatus _nya_os_socket_status(s32 error) {
    switch (error) {
        case 0:                  return NYA_OS_SOCKET_OK;

        case WSAEWOULDBLOCK:
        case WSAEINPROGRESS:
        case WSAEALREADY:        return NYA_OS_SOCKET_WOULD_BLOCK;

        case WSAECONNRESET:
        case WSAECONNABORTED:
        case WSAENOTCONN:
        case WSAESHUTDOWN:       return NYA_OS_SOCKET_CLOSED;

        case WSAECONNREFUSED:    return NYA_OS_SOCKET_REFUSED;

        case WSAEADDRINUSE:
        case WSAEACCES:          return NYA_OS_SOCKET_IN_USE;

        case WSAENETUNREACH:
        case WSAEHOSTUNREACH:
        case WSAEAFNOSUPPORT:    return NYA_OS_SOCKET_UNREACHABLE;

        default:                 return NYA_OS_SOCKET_FAILED;
    }
}

/** The last error as this module's answer, which on Windows is a call rather than a variable. */
NYA_INTERNAL NYA_OsSocketStatus _nya_os_socket_last(void) {
    return _nya_os_socket_status(WSAGetLastError());
}

/** Turns a fresh socket into the kind this module promises: non-blocking, and not inherited. */
NYA_INTERNAL b8 _nya_os_socket_prepare(SOCKET handle) {
    u_long non_blocking = 1;

    if (ioctlsocket(handle, FIONBIO, &non_blocking) != 0) return false;

    // A socket a child process inherits is a port that stays bound after the server is gone.
    (void)SetHandleInformation((HANDLE)handle, HANDLE_FLAG_INHERIT, 0);

    return true;
}

/** The engine's address as the host's, which is where the two families stop being one type. */
NYA_INTERNAL u64 _nya_os_address_to_host(NYA_OsAddress address, OUT struct sockaddr_storage* out_storage) {
    memset(out_storage, 0, sizeof(struct sockaddr_storage));

    if (address.kind == NYA_OS_ADDRESS_V4) {
        struct sockaddr_in* v4 = (struct sockaddr_in*)out_storage;

        v4->sin_family = AF_INET;
        v4->sin_port   = htons(address.port);
        memcpy(&v4->sin_addr, address.bytes, 4);

        return sizeof(struct sockaddr_in);
    }

    if (address.kind == NYA_OS_ADDRESS_V6) {
        struct sockaddr_in6* v6 = (struct sockaddr_in6*)out_storage;

        v6->sin6_family   = AF_INET6;
        v6->sin6_port     = htons(address.port);
        v6->sin6_scope_id = address.scope;
        memcpy(&v6->sin6_addr, address.bytes, 16);

        return sizeof(struct sockaddr_in6);
    }

    return 0;
}

/** And back, unmapping a v4 address that arrived on a dual stack socket. See the linux file for why. */
NYA_INTERNAL b8 _nya_os_address_from_host(const struct sockaddr* host, OUT NYA_OsAddress* out_address) {
    memset(out_address, 0, sizeof(NYA_OsAddress));

    if (host->sa_family == AF_INET) {
        const struct sockaddr_in* v4 = (const struct sockaddr_in*)host;

        out_address->kind = NYA_OS_ADDRESS_V4;
        out_address->port = ntohs(v4->sin_port);
        memcpy(out_address->bytes, &v4->sin_addr, 4);

        return true;
    }

    if (host->sa_family == AF_INET6) {
        const struct sockaddr_in6* v6 = (const struct sockaddr_in6*)host;

        out_address->port  = ntohs(v6->sin6_port);
        out_address->scope = v6->sin6_scope_id;

        const u8* bytes = (const u8*)&v6->sin6_addr;

        static const u8 MAPPED_PREFIX[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };

        if (memcmp(bytes, MAPPED_PREFIX, sizeof(MAPPED_PREFIX)) == 0) {
            out_address->kind = NYA_OS_ADDRESS_V4;
            memcpy(out_address->bytes, bytes + 12, 4);

            return true;
        }

        out_address->kind = NYA_OS_ADDRESS_V6;
        memcpy(out_address->bytes, bytes, 16);

        return true;
    }

    return false;
}

/** Binds one socket to exactly the address it was given, in that address's own family. */
NYA_INTERNAL NYA_OsSocketStatus _nya_os_socket_bind_one(s32 type, NYA_OsAddress address, OUT SOCKET* out_handle) {
    *out_handle = INVALID_SOCKET;

    SOCKET handle = socket(address.kind == NYA_OS_ADDRESS_V6 ? AF_INET6 : AF_INET, type, 0);
    if (handle == INVALID_SOCKET) return _nya_os_socket_last();

    if (!_nya_os_socket_prepare(handle)) {
        (void)closesocket(handle);
        return NYA_OS_SOCKET_FAILED;
    }

    s32 on = 1;
    (void)setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof(on));

    struct sockaddr_storage storage = { 0 };
    u64                     size    = _nya_os_address_to_host(address, &storage);

    if (bind(handle, (const struct sockaddr*)&storage, (s32)size) != 0) {
        NYA_OsSocketStatus status = _nya_os_socket_last();

        (void)closesocket(handle);

        return status;
    }

    *out_handle = handle;

    return NYA_OS_SOCKET_OK;
}

/** Opens a socket of `type` bound to `port`, dual stack where the host allows it. See the linux file. */
NYA_INTERNAL NYA_OsSocketStatus _nya_os_socket_bind(s32 type, NYA_OsAddress address, OUT SOCKET* out_handle) {
    *out_handle = INVALID_SOCKET;

    // An address the caller named is bound in its own family and nothing else; see the linux file.
    if (address.kind != NYA_OS_ADDRESS_NONE) return _nya_os_socket_bind_one(type, address, out_handle);

    u16 port = address.port;

    b8     dual   = true;
    SOCKET handle = socket(AF_INET6, type, 0);

    if (handle == INVALID_SOCKET) {
        dual   = false;
        handle = socket(AF_INET, type, 0);
    }

    if (handle == INVALID_SOCKET) return _nya_os_socket_last();

    if (!_nya_os_socket_prepare(handle)) {
        (void)closesocket(handle);
        return NYA_OS_SOCKET_FAILED;
    }

    // SO_REUSEADDR on Windows lets another process steal a listening port, so use SO_EXCLUSIVEADDRUSE; a restart still binds, as Windows has no listening TIME_WAIT.
    s32 on = 1;
    (void)setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof(on));

    if (dual) {
        s32 off = 0;

        if (setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof(off)) != 0) {
            (void)closesocket(handle);

            handle = socket(AF_INET, type, 0);
            if (handle == INVALID_SOCKET) return _nya_os_socket_last();

            if (!_nya_os_socket_prepare(handle)) {
                (void)closesocket(handle);
                return NYA_OS_SOCKET_FAILED;
            }

            (void)setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&on, sizeof(on));
            dual = false;
        }
    }

    struct sockaddr_storage storage = { 0 };
    u64                     size    = _nya_os_address_to_host(nya_os_address_any(dual ? NYA_OS_ADDRESS_V6 : NYA_OS_ADDRESS_V4, port), &storage);

    if (bind(handle, (const struct sockaddr*)&storage, (s32)size) != 0) {
        NYA_OsSocketStatus status = _nya_os_socket_last();

        (void)closesocket(handle);

        return status;
    }

    *out_handle = handle;

    return NYA_OS_SOCKET_OK;
}

// THE LIBRARY

NYA_OsSocketStatus nya_os_socket_start(void) {
    if (_NYA_OS_SOCKET_STARTS > 0) {
        _NYA_OS_SOCKET_STARTS += 1;
        return NYA_OS_SOCKET_OK;
    }

    WSADATA data = { 0 };

    // 2.2, which every Windows this builds for has had since Windows 98 and which getaddrinfo and the v6 stack need.
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return NYA_OS_SOCKET_FAILED;

    _NYA_OS_SOCKET_STARTS = 1;

    return NYA_OS_SOCKET_OK;
}

s64 nya_os_socket_descriptor(NYA_OsSocket socket) {
    return socket.handle == 0 ? -1 : (s64)(socket.handle - 1);
}

void nya_os_socket_stop(void) {
    if (_NYA_OS_SOCKET_STARTS == 0) return;

    _NYA_OS_SOCKET_STARTS -= 1;

    if (_NYA_OS_SOCKET_STARTS == 0) (void)WSACleanup();
}

// OPENING

NYA_OsSocketStatus nya_os_socket_open(NYA_OsSocketKind kind, u16 port, u32 backlog, NYA_OsSocket* out_socket) {
    return nya_os_socket_open_at(kind, (NYA_OsAddress){ .port = port }, backlog, out_socket);
}

NYA_OsSocketStatus nya_os_socket_open_at(NYA_OsSocketKind kind, NYA_OsAddress address, u32 backlog, NYA_OsSocket* out_socket) {
    *out_socket = NYA_OS_SOCKET_NONE;

    if (kind >= NYA_OS_SOCKET_KIND_COUNT) return NYA_OS_SOCKET_FAILED;

    b8 listener = kind == NYA_OS_SOCKET_LISTENER;

    SOCKET             handle = INVALID_SOCKET;
    NYA_OsSocketStatus status = _nya_os_socket_bind(listener ? SOCK_STREAM : SOCK_DGRAM, address, &handle);

    if (status != NYA_OS_SOCKET_OK) return status;

    if (listener) {
        if (listen(handle, (s32)(backlog > 0 ? backlog : NYA_OS_SOCKET_BACKLOG)) != 0) {
            status = _nya_os_socket_last();

            (void)closesocket(handle);

            return status;
        }
    } else {
        // Windows raises WSAECONNRESET on the next read after a refused datagram, downing a good server; SIO_UDP_CONNRESET turns that off (no Linux equivalent).
        DWORD off      = 0;
        DWORD returned = 0;

        (void)WSAIoctl(handle, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &returned, nullptr, nullptr);
    }

    *out_socket = _nya_os_socket_of(handle);

    return NYA_OS_SOCKET_OK;
}

NYA_OsSocketStatus nya_os_socket_accept(NYA_OsSocket listener, NYA_OsSocket* out_socket, NYA_OsAddress* out_from) {
    *out_socket = NYA_OS_SOCKET_NONE;
    memset(out_from, 0, sizeof(NYA_OsAddress));

    SOCKET handle = _nya_os_socket_handle(listener);
    if (handle == INVALID_SOCKET) return NYA_OS_SOCKET_FAILED;

    struct sockaddr_storage storage = { 0 };
    s32                     size    = sizeof(storage);

    SOCKET accepted = accept(handle, (struct sockaddr*)&storage, &size);

    if (accepted == INVALID_SOCKET) return _nya_os_socket_last();

    if (!_nya_os_socket_prepare(accepted)) {
        (void)closesocket(accepted);
        return NYA_OS_SOCKET_FAILED;
    }

    (void)_nya_os_address_from_host((const struct sockaddr*)&storage, out_from);

    *out_socket = _nya_os_socket_of(accepted);

    return NYA_OS_SOCKET_OK;
}

NYA_OsSocketStatus nya_os_socket_connect(NYA_OsAddress address, NYA_OsSocket* out_socket) {
    *out_socket = NYA_OS_SOCKET_NONE;

    if (address.kind == NYA_OS_ADDRESS_NONE) return NYA_OS_SOCKET_UNREACHABLE;

    SOCKET handle = socket(address.kind == NYA_OS_ADDRESS_V6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (handle == INVALID_SOCKET) return _nya_os_socket_last();

    if (!_nya_os_socket_prepare(handle)) {
        (void)closesocket(handle);
        return NYA_OS_SOCKET_FAILED;
    }

    struct sockaddr_storage storage = { 0 };
    u64                     size    = _nya_os_address_to_host(address, &storage);

    s32                connected = connect(handle, (const struct sockaddr*)&storage, (s32)size);
    NYA_OsSocketStatus status    = connected == 0 ? NYA_OS_SOCKET_OK : _nya_os_socket_last();

    if (status != NYA_OS_SOCKET_OK && status != NYA_OS_SOCKET_WOULD_BLOCK) {
        (void)closesocket(handle);
        return status;
    }

    *out_socket = _nya_os_socket_of(handle);

    return status;
}

void nya_os_socket_close(NYA_OsSocket socket) {
    SOCKET handle = _nya_os_socket_handle(socket);

    if (handle == INVALID_SOCKET) return;

    (void)closesocket(handle);
}

// MOVING BYTES

NYA_OsSocketStatus nya_os_socket_send_to(NYA_OsSocket socket, NYA_OsAddress to, const u8* data, u64 size) {
    SOCKET handle = _nya_os_socket_handle(socket);
    if (handle == INVALID_SOCKET) return NYA_OS_SOCKET_FAILED;

    struct sockaddr_storage storage = { 0 };
    u64                     length  = _nya_os_address_to_host(to, &storage);

    if (length == 0) return NYA_OS_SOCKET_UNREACHABLE;

    s32 sent = sendto(handle, (const char*)data, (s32)size, 0, (const struct sockaddr*)&storage, (s32)length);

    if (sent < 0) return _nya_os_socket_last();

    return (u64)sent == size ? NYA_OS_SOCKET_OK : NYA_OS_SOCKET_FAILED;
}

NYA_OsSocketStatus nya_os_socket_receive_from(NYA_OsSocket socket, u8* out_data, u64 capacity, u64* out_size, NYA_OsAddress* out_from) {
    *out_size = 0;
    memset(out_from, 0, sizeof(NYA_OsAddress));

    SOCKET handle = _nya_os_socket_handle(socket);
    if (handle == INVALID_SOCKET) return NYA_OS_SOCKET_FAILED;

    struct sockaddr_storage storage = { 0 };
    s32                     size    = sizeof(storage);

    s32 read = recvfrom(handle, (char*)out_data, (s32)capacity, 0, (struct sockaddr*)&storage, &size);

    if (read < 0) return _nya_os_socket_last();

    (void)_nya_os_address_from_host((const struct sockaddr*)&storage, out_from);

    *out_size = (u64)read;

    return NYA_OS_SOCKET_OK;
}

NYA_OsSocketStatus nya_os_socket_send(NYA_OsSocket socket, const u8* data, u64 size, u64* out_sent) {
    *out_sent = 0;

    SOCKET handle = _nya_os_socket_handle(socket);
    if (handle == INVALID_SOCKET) return NYA_OS_SOCKET_FAILED;

    s32 sent = send(handle, (const char*)data, (s32)size, 0);

    if (sent < 0) return _nya_os_socket_last();

    *out_sent = (u64)sent;

    return NYA_OS_SOCKET_OK;
}

NYA_OsSocketStatus nya_os_socket_receive(NYA_OsSocket socket, u8* out_data, u64 capacity, u64* out_read) {
    *out_read = 0;

    SOCKET handle = _nya_os_socket_handle(socket);
    if (handle == INVALID_SOCKET) return NYA_OS_SOCKET_FAILED;

    s32 read = recv(handle, (char*)out_data, (s32)capacity, 0);

    if (read < 0) return _nya_os_socket_last();

    if (read == 0) return NYA_OS_SOCKET_CLOSED;

    *out_read = (u64)read;

    return NYA_OS_SOCKET_OK;
}

// WAITING, AND ASKING

NYA_OsSocketStatus nya_os_socket_wait(NYA_OsSocketWait* sockets, u32 count, u32 timeout_ms, u32* out_ready) {
    *out_ready = 0;

    if (count > NYA_OS_SOCKET_WAIT_MAX) return NYA_OS_SOCKET_FAILED;

    WSAPOLLFD watched[NYA_OS_SOCKET_WAIT_MAX] = { 0 };

    for (u32 index = 0; index < count; index++) {
        watched[index].fd = _nya_os_socket_handle(sockets[index].socket);

        // No POLLIN here: WSAPoll refuses the flag outright, and POLLRDNORM is what it means by it.
        watched[index].events = (SHORT)((sockets[index].readable ? POLLRDNORM : 0) | (sockets[index].writable ? POLLWRNORM : 0));

        sockets[index].is_readable = false;
        sockets[index].is_writable = false;
        sockets[index].is_closed   = false;
    }

    // WSAPoll with no sockets returns at once rather than sleeping, which would spin an empty server; sleep here instead, which is what the caller asked for.
    if (count == 0) {
        Sleep(timeout_ms == NYA_OS_SOCKET_WAIT_FOREVER ? INFINITE : (DWORD)timeout_ms);
        return NYA_OS_SOCKET_OK;
    }

    s32 timeout = timeout_ms == NYA_OS_SOCKET_WAIT_FOREVER ? -1 : (s32)timeout_ms;
    s32 ready   = WSAPoll(watched, (ULONG)count, timeout);

    if (ready < 0) return _nya_os_socket_last();

    for (u32 index = 0; index < count; index++) {
        SHORT events = watched[index].revents;

        sockets[index].is_readable = (events & POLLRDNORM) != 0;
        sockets[index].is_writable = (events & POLLWRNORM) != 0;
        sockets[index].is_closed   = (events & (POLLHUP | POLLERR | POLLNVAL)) != 0;
    }

    *out_ready = (u32)ready;

    return NYA_OS_SOCKET_OK;
}

NYA_OsSocketStatus nya_os_socket_error(NYA_OsSocket socket) {
    SOCKET handle = _nya_os_socket_handle(socket);
    if (handle == INVALID_SOCKET) return NYA_OS_SOCKET_FAILED;

    s32 error = 0;
    s32 size  = sizeof(error);

    if (getsockopt(handle, SOL_SOCKET, SO_ERROR, (char*)&error, &size) != 0) return _nya_os_socket_last();

    return _nya_os_socket_status(error);
}

NYA_OsSocketStatus nya_os_socket_address(NYA_OsSocket socket, NYA_OsAddress* out_address) {
    memset(out_address, 0, sizeof(NYA_OsAddress));

    SOCKET handle = _nya_os_socket_handle(socket);
    if (handle == INVALID_SOCKET) return NYA_OS_SOCKET_FAILED;

    struct sockaddr_storage storage = { 0 };
    s32                     size    = sizeof(storage);

    if (getsockname(handle, (struct sockaddr*)&storage, &size) != 0) return _nya_os_socket_last();

    return _nya_os_address_from_host((const struct sockaddr*)&storage, out_address) ? NYA_OS_SOCKET_OK : NYA_OS_SOCKET_FAILED;
}

NYA_OsSocketStatus nya_os_socket_set_no_delay(NYA_OsSocket socket, b8 no_delay) {
    SOCKET handle = _nya_os_socket_handle(socket);
    if (handle == INVALID_SOCKET) return NYA_OS_SOCKET_FAILED;

    s32 on = no_delay ? 1 : 0;

    if (setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on)) != 0) return _nya_os_socket_last();

    return NYA_OS_SOCKET_OK;
}

// ADDRESSES

NYA_OsSocketStatus nya_os_address_resolve(NYA_ConstCString host, u16 port, NYA_OsAddressKind prefer, NYA_OsAddress* out_address) {
    memset(out_address, 0, sizeof(NYA_OsAddress));

    if (host == nullptr || host[0] == '\0') return NYA_OS_SOCKET_UNREACHABLE;

    struct addrinfo hints = { .ai_socktype = SOCK_STREAM };

    hints.ai_family = prefer == NYA_OS_ADDRESS_V4 ? AF_INET : (prefer == NYA_OS_ADDRESS_V6 ? AF_INET6 : AF_UNSPEC);

    struct addrinfo* found = nullptr;

    if (getaddrinfo(host, nullptr, &hints, &found) != 0 || found == nullptr) return NYA_OS_SOCKET_UNREACHABLE;

    b8 read = _nya_os_address_from_host(found->ai_addr, out_address);

    freeaddrinfo(found);

    if (!read) return NYA_OS_SOCKET_UNREACHABLE;

    out_address->port = port;

    return NYA_OS_SOCKET_OK;
}

NYA_OsAddress nya_os_address_any(NYA_OsAddressKind kind, u16 port) {
    return (NYA_OsAddress){ .kind = kind == NYA_OS_ADDRESS_V6 ? NYA_OS_ADDRESS_V6 : NYA_OS_ADDRESS_V4, .port = port };
}

b8 nya_os_address_equals(NYA_OsAddress a, NYA_OsAddress b) {
    if (a.kind != b.kind || a.port != b.port) return false;

    if (a.kind == NYA_OS_ADDRESS_NONE) return true;
    if (a.kind == NYA_OS_ADDRESS_V6 && a.scope != b.scope) return false;

    u64 size = a.kind == NYA_OS_ADDRESS_V6 ? 16 : 4;

    return memcmp(a.bytes, b.bytes, (size_t)size) == 0;
}

b8 nya_os_address_equals_host(NYA_OsAddress a, NYA_OsAddress b) {
    NYA_OsAddress without_ports_a = a;
    NYA_OsAddress without_ports_b = b;

    without_ports_a.port = 0;
    without_ports_b.port = 0;

    return nya_os_address_equals(without_ports_a, without_ports_b);
}

b8 nya_os_address_text(NYA_OsAddress address, b8 with_port, char* out_text, u64 capacity) {
    if (capacity == 0) return false;

    out_text[0] = '\0';

    char host[INET6_ADDRSTRLEN] = { 0 };

    if (address.kind == NYA_OS_ADDRESS_V4) {
        struct in_addr v4 = { 0 };
        memcpy(&v4, address.bytes, 4);

        if (inet_ntop(AF_INET, &v4, host, sizeof(host)) == nullptr) return false;

        s32 written = with_port ? snprintf(out_text, (size_t)capacity, "%s:%u", host, address.port) : snprintf(out_text, (size_t)capacity, "%s", host);

        return written > 0 && (u64)written < capacity;
    }

    if (address.kind == NYA_OS_ADDRESS_V6) {
        struct in6_addr v6 = { 0 };
        memcpy(&v6, address.bytes, 16);

        if (inet_ntop(AF_INET6, &v6, host, sizeof(host)) == nullptr) return false;

        s32 written = with_port ? snprintf(out_text, (size_t)capacity, "[%s]:%u", host, address.port) : snprintf(out_text, (size_t)capacity, "%s", host);

        return written > 0 && (u64)written < capacity;
    }

    return false;
}
