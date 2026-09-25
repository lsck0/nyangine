#include <windows.h>

#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The only namespace a named pipe on this machine can live in. */
#define _NYA_IPC_PIPE_PREFIX "\\\\.\\pipe\\"

/**
 * What the kernel buffers per direction per instance. The same number as NYA_IPC_BUFFER_BYTES, so a
 * message that fits this module's own buffer is one the kernel will take in a single write.
 * */
#define _NYA_IPC_PIPE_BUFFER_BYTES NYA_IPC_BUFFER_BYTES

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * One instance of the pipe, waiting for a client.
 *
 * `first` asks for FILE_FLAG_FIRST_PIPE_INSTANCE, which is how this module refuses to start when
 * another process already owns the name: without it the second listener would quietly become another
 * instance of the same pipe and the two would take turns answering.
 * */
NYA_INTERNAL HANDLE _nya_ipc_instance_create(NYA_ConstCString endpoint, b8 first) __attr_no_discard;

/**
 * Puts a pipe handle into the mode this module expects: bytes rather than messages, and returning
 * rather than waiting.
 *
 * PIPE_NOWAIT is the documented-as-legacy way to get a non-blocking pipe, and it is used here in place
 * of overlapped I/O on purpose. Overlapped is the modern answer, and it needs an OVERLAPPED and an
 * in-flight flag per connection plus one for the pending accept, all of it state this project cannot
 * exercise: the engine builds and tests on Linux, so the Windows path would be three times the code
 * with none of it ever run. The behaviour this module needs out of a pipe is exactly what PIPE_NOWAIT
 * gives: ConnectNamedPipe answers ERROR_PIPE_LISTENING instead of waiting, ReadFile answers
 * ERROR_NO_DATA instead of waiting, and WriteFile takes what fits and reports it.
 *
 * Replace this with overlapped I/O the day there is a Windows machine in the test matrix.
 * */
NYA_INTERNAL b8 _nya_ipc_handle_set_nowait(HANDLE handle) __attr_no_discard;

/** A HANDLE as this module's portable handle, with INVALID_HANDLE_VALUE mapped to -1. */
NYA_INTERNAL s64 _nya_ipc_handle_to_s64(HANDLE handle) __attr_no_discard;

s64 _nya_ipc_handle_to_s64(HANDLE handle) {
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return -1;

    return (s64)(intptr_t)handle;
}

b8 _nya_ipc_handle_set_nowait(HANDLE handle) {
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;

    return SetNamedPipeHandleState(handle, &mode, nullptr, nullptr) != 0;
}

HANDLE _nya_ipc_instance_create(NYA_ConstCString endpoint, b8 first) {
    nya_assert(endpoint != nullptr);

    DWORD open_mode = PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0U);

    /*
     * No security descriptor is passed, which gives the pipe the default: the creating user and the
     * system, and nobody else. That is exactly the rule the unix side gets from mode 0600, and writing
     * a descriptor by hand here would be a chance to get it wrong in a direction that matters.
     */
    HANDLE handle = CreateNamedPipeA(
        endpoint,
        open_mode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
        NYA_IPC_MAX_CONNECTIONS,
        _NYA_IPC_PIPE_BUFFER_BYTES,
        _NYA_IPC_PIPE_BUFFER_BYTES,
        0,
        nullptr
    );

    return handle;
}

NYA_Error _nya_ipc_endpoint_resolve(const NYA_IpcName* name, OUT char* out_endpoint, u64 capacity) {
    nya_assert(name != nullptr);
    nya_assert(out_endpoint != nullptr);
    nya_assert(capacity >= NYA_IPC_MAX_ENDPOINT);

    s32 written = snprintf(out_endpoint, capacity, _NYA_IPC_PIPE_PREFIX "%s", name->text);

    if (written < 0 || (u64)written >= capacity) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'" _NYA_IPC_PIPE_PREFIX "%s' does not fit a pipe name", name->text);
    }

    return NYA_OK;
}

NYA_Error _nya_ipc_listen(NYA_IpcListener* listener) {
    nya_assert(listener != nullptr);
    nya_assert(listener->handle < 0);

    HANDLE handle = _nya_ipc_instance_create(listener->endpoint, true);

    if (handle == INVALID_HANDLE_VALUE) {
        DWORD reason = GetLastError();

        // What FILE_FLAG_FIRST_PIPE_INSTANCE reports when the name is taken, which is the case worth
        // naming: a second copy of the program, not a permissions problem.
        if (reason == ERROR_ACCESS_DENIED) {
            return nya_error(NYA_ERROR_ALREADY_EXISTS, "another process is already listening on '%s'", listener->endpoint);
        }

        return nya_error(NYA_ERROR_IO, "could not create '%s': error %lu", listener->endpoint, (unsigned long)reason);
    }

    listener->handle = _nya_ipc_handle_to_s64(handle);

    return NYA_OK;
}

void _nya_ipc_unlisten(NYA_IpcListener* listener) {
    nya_assert(listener != nullptr);

    // Nothing to unlink: a pipe's name exists only while an instance is open.
    if (listener->handle >= 0) (void)CloseHandle((HANDLE)(intptr_t)listener->handle);
    listener->handle = -1;
}

s64 _nya_ipc_accept(NYA_IpcListener* listener) {
    nya_assert(listener != nullptr);

    if (listener->handle < 0) return -1;

    HANDLE waiting = (HANDLE)(intptr_t)listener->handle;

    if (ConnectNamedPipe(waiting, nullptr) == 0) {
        DWORD reason = GetLastError();

        // Nobody has connected yet. The ordinary answer, and not a condition to report.
        if (reason == ERROR_PIPE_LISTENING) return -1;

        // The client connected in the window between the instance being created and this call. Its
        // connection is complete, so this is a success spelled as a failure.
        if (reason != ERROR_PIPE_CONNECTED) return -1;
    }

    if (!_nya_ipc_handle_set_nowait(waiting)) {
        (void)CloseHandle(waiting);
        listener->handle = -1;

        return -1;
    }

    /*
     * The accepted instance leaves as the connection, so a fresh one has to take its place before the
     * next accept. Failing that leaves the listener with no instance: connections already up keep
     * working and no new ones arrive, which is the honest degradation for running out of handles.
     */
    HANDLE next = _nya_ipc_instance_create(listener->endpoint, false);

    listener->handle = _nya_ipc_handle_to_s64(next);
    if (listener->handle < 0) nya_log_warn("ipc: could not open another pipe instance for '%s'; no further connections", listener->endpoint);

    return _nya_ipc_handle_to_s64(waiting);
}

NYA_Error _nya_ipc_connect(NYA_ConstCString endpoint, OUT s64* out_handle) {
    nya_assert(endpoint != nullptr);
    nya_assert(out_handle != nullptr);

    *out_handle = -1;

    HANDLE handle = CreateFileA(endpoint, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        DWORD reason = GetLastError();

        if (reason == ERROR_FILE_NOT_FOUND) return nya_error(NYA_ERROR_NOT_FOUND, "nothing is listening on '%s'", endpoint);
        if (reason == ERROR_ACCESS_DENIED) return nya_error(NYA_ERROR_PERMISSION_DENIED, "not allowed to connect to '%s'", endpoint);

        return nya_error(NYA_ERROR_IO, "could not connect to '%s': error %lu", endpoint, (unsigned long)reason);
    }

    if (!_nya_ipc_handle_set_nowait(handle)) {
        (void)CloseHandle(handle);
        return nya_error(NYA_ERROR_IO, "could not put '%s' into non-blocking mode", endpoint);
    }

    *out_handle = _nya_ipc_handle_to_s64(handle);

    return NYA_OK;
}

s64 _nya_ipc_read(s64 handle, OUT u8* out, u64 capacity) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 0);

    if (handle < 0) return -1;

    // ReadFile takes a DWORD, and the buffers here are far inside it; the clamp is what makes that true
    // rather than assumed.
    DWORD wanted = (DWORD)nya_min(capacity, (u64)_NYA_IPC_PIPE_BUFFER_BYTES);
    DWORD got    = 0;

    if (ReadFile((HANDLE)(intptr_t)handle, out, wanted, &got, nullptr) == 0) {
        DWORD reason = GetLastError();

        // Nothing waiting on a PIPE_NOWAIT handle.
        if (reason == ERROR_NO_DATA) return 0;

        return -1;
    }

    // A nowait pipe reports an empty read as a success with zero bytes as well as through ERROR_NO_DATA,
    // depending on the timing. Neither means end of file; that arrives as ERROR_BROKEN_PIPE above.
    nya_assert((u64)got <= capacity);

    return (s64)got;
}

s64 _nya_ipc_write(s64 handle, const u8* data, u64 size) {
    nya_assert(data != nullptr);

    if (handle < 0) return -1;
    if (size == 0) return 0;

    DWORD wanted = (DWORD)nya_min(size, (u64)_NYA_IPC_PIPE_BUFFER_BYTES);
    DWORD took   = 0;

    if (WriteFile((HANDLE)(intptr_t)handle, data, wanted, &took, nullptr) == 0) return -1;

    nya_assert((u64)took <= size);

    return (s64)took;
}

void _nya_ipc_close(s64 handle) {
    if (handle < 0) return;

    HANDLE closing = (HANDLE)(intptr_t)handle;

    // Disconnect before close so a client reading at that moment sees end of file rather than a handle
    // that stays half alive until the last reference in the system goes away. Fails harmlessly on the
    // client side of a pipe, which is not a server end.
    (void)DisconnectNamedPipe(closing);
    (void)CloseHandle(closing);
}
