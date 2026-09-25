#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Connections the kernel holds for us between two polls. Small on purpose: a control channel is
 * connected to by hand or by one script, so anything past a couple at once is a process in a loop,
 * and a shallow backlog is what refuses it at the kernel rather than in this file.
 * */
#define _NYA_IPC_BACKLOG 4

/**
 * Where the socket lives when `$XDG_RUNTIME_DIR` is not set, which is what a bare ssh session and
 * most containers look like.
 * */
#define _NYA_IPC_FALLBACK_DIRECTORY "/tmp"

/** Appended to the name, so a socket is recognisable in a directory listing. */
#define _NYA_IPC_SUFFIX ".sock"

/**
 * Owner read and write, nothing else. A control channel takes commands, so anyone who can open it can
 * drive the program.
 * */
#define _NYA_IPC_SOCKET_MODE 0o600

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Whether a process is still listening on `endpoint`, as opposed to it being a file a crash left behind. */
NYA_INTERNAL b8 _nya_ipc_endpoint_is_live(NYA_ConstCString endpoint) __attr_no_discard;

/** Fills `out` with the address for `endpoint`. False when the path does not fit `sun_path`. */
NYA_INTERNAL b8 _nya_ipc_address_from_endpoint(NYA_ConstCString endpoint, OUT struct sockaddr_un* out_address) __attr_no_discard;

b8 _nya_ipc_address_from_endpoint(NYA_ConstCString endpoint, OUT struct sockaddr_un* out_address) {
    nya_assert(endpoint != nullptr);
    nya_assert(out_address != nullptr);

    *out_address = (struct sockaddr_un){ .sun_family = AF_UNIX };

    u64 length = strlen(endpoint);
    if (length == 0 || length >= sizeof(out_address->sun_path)) return false;

    nya_memcpy(out_address->sun_path, endpoint, length);

    return true;
}

b8 _nya_ipc_endpoint_is_live(NYA_ConstCString endpoint) {
    struct sockaddr_un address = { 0 };
    if (!_nya_ipc_address_from_endpoint(endpoint, &address)) return false;

    s32 handle = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (handle < 0) return false;

    /*
     * Three answers, not two. A connect that succeeds means somebody is listening. ENOENT means there is
     * no file at all, and ECONNREFUSED means the file outlived the process that made it, which is what a
     * crash leaves behind; both may be bound over. Everything else, a permission problem included, is
     * treated as a live owner, because stealing a name another process may still be using is worse than
     * failing to start.
     */
    b8  connected = connect(handle, (const struct sockaddr*)&address, sizeof(address)) == 0;
    s32 reason    = errno;

    b8 live = connected || (reason != ECONNREFUSED && reason != ENOENT);

    (void)close(handle);

    return live;
}

NYA_Error _nya_ipc_endpoint_resolve(const NYA_IpcName* name, OUT char* out_endpoint, u64 capacity) {
    nya_assert(name != nullptr);
    nya_assert(out_endpoint != nullptr);
    nya_assert(capacity >= NYA_IPC_MAX_ENDPOINT);

    NYA_ConstCString directory = getenv("XDG_RUNTIME_DIR");
    if (directory == nullptr || directory[0] == '\0') directory = _NYA_IPC_FALLBACK_DIRECTORY;

    s32 written = snprintf(out_endpoint, capacity, "%s/%s" _NYA_IPC_SUFFIX, directory, name->text);

    if (written < 0 || (u64)written >= capacity) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s/%s" _NYA_IPC_SUFFIX "' does not fit a unix socket path", directory, name->text);
    }

    return NYA_OK;
}

NYA_Error _nya_ipc_listen(NYA_IpcListener* listener) {
    nya_assert(listener != nullptr);
    nya_assert(listener->handle < 0);

    struct sockaddr_un address = { 0 };
    if (!_nya_ipc_address_from_endpoint(listener->endpoint, &address)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' does not fit a unix socket path", listener->endpoint);
    }

    if (_nya_ipc_endpoint_is_live(listener->endpoint)) {
        return nya_error(NYA_ERROR_ALREADY_EXISTS, "another process is already listening on '%s'", listener->endpoint);
    }

    // Established above to be nobody's, so removing it is not taking anything from anyone. ENOENT is
    // the ordinary case of a first run.
    if (unlink(listener->endpoint) != 0 && errno != ENOENT) {
        return nya_error(NYA_ERROR_PERMISSION_DENIED, "could not remove the stale socket '%s'", listener->endpoint);
    }

    s32 handle = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (handle < 0) return nya_error(NYA_ERROR_IO, "could not create a unix socket");

    /*
     * The mode is set through the umask rather than with a chmod after the bind, because the bind is
     * what publishes the name: between a permissive bind and a later chmod there is a window in which
     * any process on the machine can connect and drive this one. The umask is restored immediately, and
     * this runs at subsystem init where no other thread is creating files.
     */
    mode_t previous   = umask(0o777 & ~(mode_t)_NYA_IPC_SOCKET_MODE);
    s32    bound      = bind(handle, (const struct sockaddr*)&address, sizeof(address));
    s32    bind_errno = errno;
    (void)umask(previous);

    if (bound != 0) {
        (void)close(handle);
        return nya_error(NYA_ERROR_IO, "could not bind '%s': %s", listener->endpoint, strerror(bind_errno));
    }

    if (listen(handle, _NYA_IPC_BACKLOG) != 0) {
        (void)close(handle);
        (void)unlink(listener->endpoint);

        return nya_error(NYA_ERROR_IO, "could not listen on '%s'", listener->endpoint);
    }

    listener->handle = handle;

    return NYA_OK;
}

void _nya_ipc_unlisten(NYA_IpcListener* listener) {
    nya_assert(listener != nullptr);

    if (listener->handle >= 0) (void)close((s32)listener->handle);
    listener->handle = -1;

    if (listener->endpoint[0] != '\0') (void)unlink(listener->endpoint);
}

s64 _nya_ipc_accept(NYA_IpcListener* listener) {
    nya_assert(listener != nullptr);

    if (listener->handle < 0) return -1;

    // accept4 would set both flags in the same syscall, but it needs _GNU_SOURCE and the engine compiles
    // to the POSIX 2008 feature set. Two more syscalls once per connection is not worth widening that.
    s32 handle = accept((s32)listener->handle, nullptr, nullptr);
    if (handle < 0) return -1;

    s32 flags = fcntl(handle, F_GETFL, 0);

    if (flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(handle, F_SETFD, FD_CLOEXEC) < 0) {
        (void)close(handle);
        return -1;
    }

    return handle;
}

NYA_Error _nya_ipc_connect(NYA_ConstCString endpoint, OUT s64* out_handle) {
    nya_assert(endpoint != nullptr);
    nya_assert(out_handle != nullptr);

    *out_handle = -1;

    struct sockaddr_un address = { 0 };
    if (!_nya_ipc_address_from_endpoint(endpoint, &address)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' does not fit a unix socket path", endpoint);
    }

    s32 handle = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (handle < 0) return nya_error(NYA_ERROR_IO, "could not create a unix socket");

    // Blocking for the connect itself, which for a unix socket is a single syscall that either finds a
    // listener or does not, then non-blocking for everything after it.
    if (connect(handle, (const struct sockaddr*)&address, sizeof(address)) != 0) {
        s32 reason = errno;
        (void)close(handle);

        if (reason == ENOENT || reason == ECONNREFUSED) return nya_error(NYA_ERROR_NOT_FOUND, "nothing is listening on '%s'", endpoint);
        if (reason == EACCES || reason == EPERM) return nya_error(NYA_ERROR_PERMISSION_DENIED, "not allowed to connect to '%s'", endpoint);

        return nya_error(NYA_ERROR_IO, "could not connect to '%s': %s", endpoint, strerror(reason));
    }

    s32 flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0) {
        (void)close(handle);
        return nya_error(NYA_ERROR_IO, "could not put '%s' into non-blocking mode", endpoint);
    }

    *out_handle = handle;

    return NYA_OK;
}

s64 _nya_ipc_read(s64 handle, OUT u8* out, u64 capacity) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 0);

    if (handle < 0) return -1;

    ssize_t got = recv((s32)handle, out, capacity, 0);

    if (got < 0) {
        // Nothing waiting, or a signal landed mid call. Neither says anything about the peer.
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }

    // Zero from a stream socket is end of file; "nothing right now" is EAGAIN, handled above.
    if (got == 0) return -1;

    nya_assert((u64)got <= capacity);

    return (s64)got;
}

s64 _nya_ipc_write(s64 handle, const u8* data, u64 size) {
    nya_assert(data != nullptr);

    if (handle < 0) return -1;
    if (size == 0) return 0;

    // MSG_NOSIGNAL: without it a peer that closes mid write raises SIGPIPE, whose default action kills
    // the process, so quitting the control panel would take the program with it.
    ssize_t took = send((s32)handle, data, size, MSG_NOSIGNAL);

    if (took < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }

    nya_assert((u64)took <= size);

    return (s64)took;
}

void _nya_ipc_close(s64 handle) {
    if (handle >= 0) (void)close((s32)handle);
}
