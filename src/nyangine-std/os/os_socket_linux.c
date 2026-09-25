#include "nyangine-std/os/os_socket.h"

// After the engine's own header: base_basic.h asks for POSIX 2008, and these declare what it wants only once they have seen that.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// INTERNAL

/**
 * A descriptor as a handle and back.
 *
 * Zero is no socket in NYA_OsSocket, and zero is also a perfectly good descriptor — stdin's. So the
 * descriptor is stored one higher, which costs an addition and makes a zeroed struct mean what the
 * header says it means.
 * */
NYA_INTERNAL s32 _nya_os_socket_fd(NYA_OsSocket socket) {
    return socket.handle == 0 ? -1 : (s32)(socket.handle - 1);
}

NYA_INTERNAL NYA_OsSocket _nya_os_socket_of(s32 descriptor) {
    return (NYA_OsSocket){ .handle = descriptor < 0 ? 0 : (u64)descriptor + 1 };
}

/** What the host's `errno` means to a caller here. */
NYA_INTERNAL NYA_OsSocketStatus _nya_os_socket_status(s32 error) {
    switch (error) {
        case 0:            return NYA_OS_SOCKET_OK;
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EINPROGRESS:  return NYA_OS_SOCKET_WOULD_BLOCK;

        case EPIPE:
        case ECONNRESET:
        case ENOTCONN:
        case ESHUTDOWN:    return NYA_OS_SOCKET_CLOSED;

        case ECONNREFUSED: return NYA_OS_SOCKET_REFUSED;

        case EADDRINUSE:
        case EACCES:       return NYA_OS_SOCKET_IN_USE;

        case ENETUNREACH:
        case EHOSTUNREACH:
        case EAFNOSUPPORT: return NYA_OS_SOCKET_UNREACHABLE;

        default:           return NYA_OS_SOCKET_FAILED;
    }
}

/** Turns a fresh descriptor into the kind of socket this module promises: non-blocking, and not inherited. */
NYA_INTERNAL b8 _nya_os_socket_prepare(s32 descriptor) {
    s32 flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0) return false;

    if (fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) return false;

    // A socket a child inherits is a port that stays bound after the server is gone, which is what SO_REUSEADDR is usually blamed for.
    s32 descriptor_flags = fcntl(descriptor, F_GETFD, 0);
    if (descriptor_flags >= 0) (void)fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC);

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

/** And back. An address of neither family answers false rather than a zeroed address that looks real. */
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

        // A v4 address on a dual-stack socket comes back mapped as ::ffff:1.2.3.4; unmapped here once so everything above sees the family actually on the wire.
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
NYA_INTERNAL NYA_OsSocketStatus _nya_os_socket_bind_one(s32 type, NYA_OsAddress address, OUT s32* out_descriptor) {
    *out_descriptor = -1;

    s32 descriptor = socket(address.kind == NYA_OS_ADDRESS_V6 ? AF_INET6 : AF_INET, type, 0);
    if (descriptor < 0) return _nya_os_socket_status(errno);

    if (!_nya_os_socket_prepare(descriptor)) {
        (void)close(descriptor);
        return NYA_OS_SOCKET_FAILED;
    }

    s32 on = 1;
    (void)setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_storage storage = { 0 };
    u64                     size    = _nya_os_address_to_host(address, &storage);

    if (bind(descriptor, (const struct sockaddr*)&storage, (socklen_t)size) != 0) {
        NYA_OsSocketStatus status = _nya_os_socket_status(errno);

        (void)close(descriptor);

        return status;
    }

    *out_descriptor = descriptor;

    return NYA_OS_SOCKET_OK;
}

/**
 * Opens a socket of `type` bound to `port`, dual stack where the host allows it.
 *
 * One function for both kinds because the difference between a datagram socket and a listening one is
 * three lines at the end, and the twenty before them — the family, the two options, the bind — are
 * exactly the part worth writing once.
 * */
NYA_INTERNAL NYA_OsSocketStatus _nya_os_socket_bind(s32 type, NYA_OsAddress address, OUT s32* out_descriptor) {
    *out_descriptor = -1;

    // An address the caller named is bound in its own family only: dual stack is about answering on every interface, which is what having no address to bind means.
    if (address.kind != NYA_OS_ADDRESS_NONE) return _nya_os_socket_bind_one(type, address, out_descriptor);

    u16 port = address.port;

    b8  dual       = true;
    s32 descriptor = socket(AF_INET6, type, 0);

    // A host with IPv6 off still has to be served, so the family is what it can get rather than what it wants.
    if (descriptor < 0) {
        dual       = false;
        descriptor = socket(AF_INET, type, 0);
    }

    if (descriptor < 0) return _nya_os_socket_status(errno);

    if (!_nya_os_socket_prepare(descriptor)) {
        (void)close(descriptor);
        return NYA_OS_SOCKET_FAILED;
    }

    s32 on = 1;

    // So a server that has just restarted binds rather than waiting out its own TIME_WAIT sockets.
    (void)setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if (dual) {
        s32 off = 0;

        // One socket for both families; a host that refuses becomes a v6-only socket, worse than v4, so a refusal starts over on v4.
        if (setsockopt(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) != 0) {
            (void)close(descriptor);

            descriptor = socket(AF_INET, type, 0);
            if (descriptor < 0) return _nya_os_socket_status(errno);

            if (!_nya_os_socket_prepare(descriptor)) {
                (void)close(descriptor);
                return NYA_OS_SOCKET_FAILED;
            }

            (void)setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
            dual = false;
        }
    }

    struct sockaddr_storage storage = { 0 };
    u64                     size    = _nya_os_address_to_host(nya_os_address_any(dual ? NYA_OS_ADDRESS_V6 : NYA_OS_ADDRESS_V4, port), &storage);

    if (bind(descriptor, (const struct sockaddr*)&storage, (socklen_t)size) != 0) {
        NYA_OsSocketStatus status = _nya_os_socket_status(errno);

        (void)close(descriptor);

        return status;
    }

    *out_descriptor = descriptor;

    return NYA_OS_SOCKET_OK;
}

// THE LIBRARY

NYA_OsSocketStatus nya_os_socket_start(void) {
    // Nothing to start: sockets are syscalls here. The call exists for Windows, where a program that skipped it would work everywhere except where it shipped.
    return NYA_OS_SOCKET_OK;
}

s64 nya_os_socket_descriptor(NYA_OsSocket socket) {
    return _nya_os_socket_fd(socket);
}

void nya_os_socket_stop(void) {}

// OPENING

NYA_OsSocketStatus nya_os_socket_open(NYA_OsSocketKind kind, u16 port, u32 backlog, NYA_OsSocket* out_socket) {
    return nya_os_socket_open_at(kind, (NYA_OsAddress){ .port = port }, backlog, out_socket);
}

NYA_OsSocketStatus nya_os_socket_open_at(NYA_OsSocketKind kind, NYA_OsAddress address, u32 backlog, NYA_OsSocket* out_socket) {
    *out_socket = NYA_OS_SOCKET_NONE;

    if (kind >= NYA_OS_SOCKET_KIND_COUNT) return NYA_OS_SOCKET_FAILED;

    b8 listener = kind == NYA_OS_SOCKET_LISTENER;

    s32                descriptor = -1;
    NYA_OsSocketStatus status     = _nya_os_socket_bind(listener ? SOCK_STREAM : SOCK_DGRAM, address, &descriptor);

    if (status != NYA_OS_SOCKET_OK) return status;

    if (listener && listen(descriptor, (s32)(backlog > 0 ? backlog : NYA_OS_SOCKET_BACKLOG)) != 0) {
        status = _nya_os_socket_status(errno);

        (void)close(descriptor);

        return status;
    }

    *out_socket = _nya_os_socket_of(descriptor);

    return NYA_OS_SOCKET_OK;
}

NYA_OsSocketStatus nya_os_socket_accept(NYA_OsSocket listener, NYA_OsSocket* out_socket, NYA_OsAddress* out_from) {
    *out_socket = NYA_OS_SOCKET_NONE;
    memset(out_from, 0, sizeof(NYA_OsAddress));

    s32 descriptor = _nya_os_socket_fd(listener);
    if (descriptor < 0) return NYA_OS_SOCKET_FAILED;

    struct sockaddr_storage storage = { 0 };
    socklen_t               size    = sizeof(storage);

    s32 accepted = accept(descriptor, (struct sockaddr*)&storage, &size);

    if (accepted < 0) return _nya_os_socket_status(errno);

    if (!_nya_os_socket_prepare(accepted)) {
        (void)close(accepted);
        return NYA_OS_SOCKET_FAILED;
    }

    (void)_nya_os_address_from_host((const struct sockaddr*)&storage, out_from);

    *out_socket = _nya_os_socket_of(accepted);

    return NYA_OS_SOCKET_OK;
}

NYA_OsSocketStatus nya_os_socket_connect(NYA_OsAddress address, NYA_OsSocket* out_socket) {
    *out_socket = NYA_OS_SOCKET_NONE;

    if (address.kind == NYA_OS_ADDRESS_NONE) return NYA_OS_SOCKET_UNREACHABLE;

    s32 descriptor = socket(address.kind == NYA_OS_ADDRESS_V6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) return _nya_os_socket_status(errno);

    if (!_nya_os_socket_prepare(descriptor)) {
        (void)close(descriptor);
        return NYA_OS_SOCKET_FAILED;
    }

    struct sockaddr_storage storage = { 0 };
    u64                     size    = _nya_os_address_to_host(address, &storage);

    s32                connected = connect(descriptor, (const struct sockaddr*)&storage, (socklen_t)size);
    NYA_OsSocketStatus status    = connected == 0 ? NYA_OS_SOCKET_OK : _nya_os_socket_status(errno);

    // Under way is not failed: the caller waits for writability then asks what came of it; anything else is over before it started and the descriptor goes with it.
    if (status != NYA_OS_SOCKET_OK && status != NYA_OS_SOCKET_WOULD_BLOCK) {
        (void)close(descriptor);
        return status;
    }

    *out_socket = _nya_os_socket_of(descriptor);

    return status;
}

void nya_os_socket_close(NYA_OsSocket socket) {
    s32 descriptor = _nya_os_socket_fd(socket);

    if (descriptor < 0) return;

    (void)close(descriptor);
}

// MOVING BYTES

NYA_OsSocketStatus nya_os_socket_send_to(NYA_OsSocket socket, NYA_OsAddress to, const u8* data, u64 size) {
    s32 descriptor = _nya_os_socket_fd(socket);
    if (descriptor < 0) return NYA_OS_SOCKET_FAILED;

    struct sockaddr_storage storage = { 0 };
    u64                     length  = _nya_os_address_to_host(to, &storage);

    if (length == 0) return NYA_OS_SOCKET_UNREACHABLE;

    ssize_t sent = sendto(descriptor, data, (size_t)size, MSG_NOSIGNAL, (const struct sockaddr*)&storage, (socklen_t)length);

    if (sent < 0) return _nya_os_socket_status(errno);

    // A datagram the host took only part of cannot be reassembled, so it is reported as not taken at all rather than a success that lost bytes.
    return (u64)sent == size ? NYA_OS_SOCKET_OK : NYA_OS_SOCKET_FAILED;
}

NYA_OsSocketStatus nya_os_socket_receive_from(NYA_OsSocket socket, u8* out_data, u64 capacity, u64* out_size, NYA_OsAddress* out_from) {
    *out_size = 0;
    memset(out_from, 0, sizeof(NYA_OsAddress));

    s32 descriptor = _nya_os_socket_fd(socket);
    if (descriptor < 0) return NYA_OS_SOCKET_FAILED;

    struct sockaddr_storage storage = { 0 };
    socklen_t               size    = sizeof(storage);

    ssize_t read = recvfrom(descriptor, out_data, (size_t)capacity, 0, (struct sockaddr*)&storage, &size);

    if (read < 0) return _nya_os_socket_status(errno);

    (void)_nya_os_address_from_host((const struct sockaddr*)&storage, out_from);

    *out_size = (u64)read;

    return NYA_OS_SOCKET_OK;
}

NYA_OsSocketStatus nya_os_socket_send(NYA_OsSocket socket, const u8* data, u64 size, u64* out_sent) {
    *out_sent = 0;

    s32 descriptor = _nya_os_socket_fd(socket);
    if (descriptor < 0) return NYA_OS_SOCKET_FAILED;

    // MSG_NOSIGNAL, because writing to a closed socket otherwise kills the process with SIGPIPE, and a peer that hung up is an ordinary event in a server.
    ssize_t sent = send(descriptor, data, (size_t)size, MSG_NOSIGNAL);

    if (sent < 0) return _nya_os_socket_status(errno);

    *out_sent = (u64)sent;

    return NYA_OS_SOCKET_OK;
}

NYA_OsSocketStatus nya_os_socket_receive(NYA_OsSocket socket, u8* out_data, u64 capacity, u64* out_read) {
    *out_read = 0;

    s32 descriptor = _nya_os_socket_fd(socket);
    if (descriptor < 0) return NYA_OS_SOCKET_FAILED;

    ssize_t read = recv(descriptor, out_data, (size_t)capacity, 0);

    if (read < 0) return _nya_os_socket_status(errno);

    // Zero bytes on a stream is its end and nothing else, which is why it is never OK: a caller that read it as a quiet moment would ask again forever.
    if (read == 0) return NYA_OS_SOCKET_CLOSED;

    *out_read = (u64)read;

    return NYA_OS_SOCKET_OK;
}

// WAITING, AND ASKING

NYA_OsSocketStatus nya_os_socket_wait(NYA_OsSocketWait* sockets, u32 count, u32 timeout_ms, u32* out_ready) {
    *out_ready = 0;

    if (count > NYA_OS_SOCKET_WAIT_MAX) return NYA_OS_SOCKET_FAILED;

    struct pollfd watched[NYA_OS_SOCKET_WAIT_MAX] = { 0 };

    for (u32 index = 0; index < count; index++) {
        watched[index].fd     = _nya_os_socket_fd(sockets[index].socket);
        watched[index].events = (s16)((sockets[index].readable ? POLLIN : 0) | (sockets[index].writable ? POLLOUT : 0));

        sockets[index].is_readable = false;
        sockets[index].is_writable = false;
        sockets[index].is_closed   = false;
    }

    s32 timeout = timeout_ms == NYA_OS_SOCKET_WAIT_FOREVER ? -1 : (s32)timeout_ms;
    s32 ready   = poll(watched, (nfds_t)count, timeout);

    // Interrupted by a signal is a timeout that happened early, not a failure: a profiler's timer must not take a server down.
    if (ready < 0) return errno == EINTR ? NYA_OS_SOCKET_OK : _nya_os_socket_status(errno);

    for (u32 index = 0; index < count; index++) {
        s16 events = (s16)watched[index].revents;

        sockets[index].is_readable = (events & POLLIN) != 0;
        sockets[index].is_writable = (events & POLLOUT) != 0;
        sockets[index].is_closed   = (events & (POLLHUP | POLLERR | POLLNVAL)) != 0;
    }

    *out_ready = (u32)ready;

    return NYA_OS_SOCKET_OK;
}

NYA_OsSocketStatus nya_os_socket_error(NYA_OsSocket socket) {
    s32 descriptor = _nya_os_socket_fd(socket);
    if (descriptor < 0) return NYA_OS_SOCKET_FAILED;

    s32       error = 0;
    socklen_t size  = sizeof(error);

    if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &size) != 0) return _nya_os_socket_status(errno);

    return _nya_os_socket_status(error);
}

NYA_OsSocketStatus nya_os_socket_address(NYA_OsSocket socket, NYA_OsAddress* out_address) {
    memset(out_address, 0, sizeof(NYA_OsAddress));

    s32 descriptor = _nya_os_socket_fd(socket);
    if (descriptor < 0) return NYA_OS_SOCKET_FAILED;

    struct sockaddr_storage storage = { 0 };
    socklen_t               size    = sizeof(storage);

    if (getsockname(descriptor, (struct sockaddr*)&storage, &size) != 0) return _nya_os_socket_status(errno);

    return _nya_os_address_from_host((const struct sockaddr*)&storage, out_address) ? NYA_OS_SOCKET_OK : NYA_OS_SOCKET_FAILED;
}

NYA_OsSocketStatus nya_os_socket_set_no_delay(NYA_OsSocket socket, b8 no_delay) {
    s32 descriptor = _nya_os_socket_fd(socket);
    if (descriptor < 0) return NYA_OS_SOCKET_FAILED;

    s32 on = no_delay ? 1 : 0;

    if (setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) return _nya_os_socket_status(errno);

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

    // The port is the caller's, not the resolver's: nothing above asks a name server which port it meant, and asking by service name is a second API nothing wants.
    out_address->port = port;

    return NYA_OS_SOCKET_OK;
}

NYA_OsAddress nya_os_address_any(NYA_OsAddressKind kind, u16 port) {
    // Zeroed bytes are how both families spell "every interface", so there is nothing to fill in but the family and the port.
    return (NYA_OsAddress){ .kind = kind == NYA_OS_ADDRESS_V6 ? NYA_OS_ADDRESS_V6 : NYA_OS_ADDRESS_V4, .port = port };
}

b8 nya_os_address_equals(NYA_OsAddress a, NYA_OsAddress b) {
    if (a.kind != b.kind || a.port != b.port) return false;

    u64 size = a.kind == NYA_OS_ADDRESS_V6 ? 16 : 4;

    if (a.kind == NYA_OS_ADDRESS_NONE) return true;
    if (a.kind == NYA_OS_ADDRESS_V6 && a.scope != b.scope) return false;

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
        if (inet_ntop(AF_INET, address.bytes, host, sizeof(host)) == nullptr) return false;

        s32 written = with_port ? snprintf(out_text, (size_t)capacity, "%s:%u", host, address.port) : snprintf(out_text, (size_t)capacity, "%s", host);

        return written > 0 && (u64)written < capacity;
    }

    if (address.kind == NYA_OS_ADDRESS_V6) {
        if (inet_ntop(AF_INET6, address.bytes, host, sizeof(host)) == nullptr) return false;

        // The brackets are not decoration: `::1:80` is a valid address on its own, so a v6 address with a port must say where the address ends.
        s32 written = with_port ? snprintf(out_text, (size_t)capacity, "[%s]:%u", host, address.port) : snprintf(out_text, (size_t)capacity, "%s", host);

        return written > 0 && (u64)written < capacity;
    }

    return false;
}
