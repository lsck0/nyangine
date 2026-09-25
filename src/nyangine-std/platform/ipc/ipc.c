#include "nyangine-core/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * How many times nya_ipc_client_send retries a partial write before giving up.
 *
 * A local pipe takes at least one page per accepted write, so this covers a message of
 * NYA_IPC_BUFFER_BYTES many times over even if the reader only ever drains a byte at a time. It is a
 * bound rather than a timeout because a client is a tool and must not hang on a stuck listener.
 * */
#define _NYA_IPC_CLIENT_SEND_ATTEMPTS 4096

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The platform half. Everything above these is portable and lives in this file; ipc_linux.c and
 * ipc_windows.c define exactly these seven and nothing else, which is the whole surface a third
 * platform would have to write.
 *
 * A handle is an s64 in all of them: a file descriptor on Linux, a HANDLE on Windows, and -1 for
 * none on both.
 */

/** Expands a parsed name into the platform's endpoint. Fails when the runtime directory makes it too long. */
NYA_INTERNAL NYA_Error _nya_ipc_endpoint_resolve(const NYA_IpcName* name, OUT char* out_endpoint, u64 capacity) __attr_no_discard;

/** Binds `listener->endpoint` and starts accepting on it, filling `listener->handle`. */
NYA_INTERNAL NYA_Error _nya_ipc_listen(NYA_IpcListener* listener) __attr_no_discard;

/** Stops accepting and removes the endpoint from the namespace. Idempotent. */
NYA_INTERNAL void _nya_ipc_unlisten(NYA_IpcListener* listener);

/** One waiting connection, or -1 when none is waiting. Never blocks. */
NYA_INTERNAL s64 _nya_ipc_accept(NYA_IpcListener* listener) __attr_no_discard;

/** Connects to an endpoint. NYA_ERROR_NOT_FOUND when nothing is listening there. */
NYA_INTERNAL NYA_Error _nya_ipc_connect(NYA_ConstCString endpoint, OUT s64* out_handle) __attr_no_discard;

/** Bytes read, 0 for nothing waiting, -1 once the peer is gone. Never blocks. */
NYA_INTERNAL s64 _nya_ipc_read(s64 handle, OUT u8* out, u64 capacity) __attr_no_discard;

/** Bytes the kernel took, which may be fewer than `size` or zero, and -1 once the peer is gone. Never blocks. */
NYA_INTERNAL s64 _nya_ipc_write(s64 handle, const u8* data, u64 size) __attr_no_discard;

NYA_INTERNAL void _nya_ipc_close(s64 handle);

/*
 * The portable half's own internals.
 */

/** The live connection `peer` names, or null. The generation is what makes a stale id answer null. */
NYA_INTERNAL NYA_IpcConnection* _nya_ipc_connection_find(NYA_IpcListener* listener, NYA_IpcPeerId peer) __attr_no_discard;

/** Closes a connection and frees its slot. Safe on a slot that is already free. */
NYA_INTERNAL void _nya_ipc_connection_close(NYA_IpcConnection* connection);

/** Pushes what the kernel will take of the queued bytes. False once the peer is gone. */
NYA_INTERNAL b8 _nya_ipc_connection_flush(NYA_IpcConnection* connection) __attr_no_discard;

/** Takes whatever is waiting to be accepted, up to the connection limit, and refuses the rest. */
NYA_INTERNAL void _nya_ipc_listener_accept_pending(NYA_IpcListener* listener);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * NAMES
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_ipc_name_parse(NYA_ConstCString text, OUT NYA_IpcName* out_name) {
    nya_assert(out_name != nullptr);

    if (text == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an ipc name may not be null");

    u64 length = 0;
    while (length < NYA_IPC_MAX_NAME && text[length] != '\0') length++;

    if (length == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an ipc name may not be empty");

    if (text[length] != '\0') { return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an ipc name is at most %u characters", (u32)(NYA_IPC_MAX_NAME - 1)); }

    for (u64 i = 0; i < length; i++) {
        char character = text[i];

        b8 allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
                     character == '.' || character == '_' || character == '-';

        if (!allowed) { return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%c' at %u is not allowed in an ipc name", character, (u32)i); }
    }

    // "." and ".." are legal spellings under the rule above and would name the directory itself once a
    // suffix is appended on one platform and not on another. Refused outright rather than reasoned about.
    if (nya_string_equals(text, ".") || nya_string_equals(text, "..")) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a usable ipc name", text);
    }

    *out_name = (NYA_IpcName){ 0 };
    nya_memcpy(out_name->text, text, length);

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────
 * PEER IDS
 * ─────────────────────────────────────────────────────────
 */

b8 nya_ipc_peer_equals(NYA_IpcPeerId a, NYA_IpcPeerId b) {
    return a.index == b.index && a.generation == b.generation;
}

b8 nya_ipc_peer_is_set(NYA_IpcPeerId peer) {
    return peer.generation != 0;
}

/*
 * ─────────────────────────────────────────────────────────
 * LISTENER
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_ipc_listener_create(NYA_Arena* arena, NYA_IpcOptions options, OUT NYA_IpcListener** out_listener) {
    nya_assert(arena != nullptr);
    nya_assert(out_listener != nullptr);

    *out_listener = nullptr;

    // A zeroed name never came out of nya_ipc_name_parse, which is the only way to make one.
    if (options.name.text[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an ipc listener needs a parsed name");

    if (options.max_connections > NYA_IPC_MAX_CONNECTIONS) {
        return nya_error(
            NYA_ERROR_INVALID_ARGUMENT,
            "asked for %u connections; the limit is %u",
            options.max_connections,
            (u32)NYA_IPC_MAX_CONNECTIONS
        );
    }

    NYA_IpcListener* listener = nya_arena_alloc(arena, sizeof(NYA_IpcListener));
    if (listener == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for an ipc listener");

    *listener = (NYA_IpcListener){
        .allocator       = arena,
        .name            = options.name,
        .handle          = -1,
        .max_connections = options.max_connections == 0 ? (u32)NYA_IPC_MAX_CONNECTIONS : options.max_connections,
        // One, not zero: a zeroed NYA_IpcPeerId has to stay invalid, so no live peer may ever hold it.
        .next_generation = 1,
    };

    for (u32 i = 0; i < NYA_IPC_MAX_CONNECTIONS; i++) listener->connections[i].handle = -1;

    NYA_Error resolved = _nya_ipc_endpoint_resolve(&listener->name, listener->endpoint, sizeof(listener->endpoint));
    if (!resolved.ok) {
        nya_arena_free(arena, listener, sizeof(NYA_IpcListener));
        return resolved;
    }

    NYA_Error listening = _nya_ipc_listen(listener);
    if (!listening.ok) {
        nya_arena_free(arena, listener, sizeof(NYA_IpcListener));
        return listening;
    }

    nya_assert(listener->handle >= 0);

    *out_listener = listener;

    return NYA_OK;
}

void nya_ipc_listener_destroy(NYA_IpcListener* listener) {
    if (listener == nullptr) return;

    for (u32 i = 0; i < NYA_IPC_MAX_CONNECTIONS; i++) _nya_ipc_connection_close(&listener->connections[i]);

    _nya_ipc_unlisten(listener);

    NYA_Arena* arena = listener->allocator;
    nya_arena_free(arena, listener, sizeof(NYA_IpcListener));
}

b8 nya_ipc_listener_poll(NYA_IpcListener* listener, OUT NYA_IpcEvent* out_event) {
    nya_assert(listener != nullptr);
    nya_assert(out_event != nullptr);
    nya_assert(listener->max_connections >= 1 && listener->max_connections <= NYA_IPC_MAX_CONNECTIONS);

    *out_event = (NYA_IpcEvent){ 0 };

    _nya_ipc_listener_accept_pending(listener);

    for (u32 step = 0; step < listener->max_connections; step++) {
        u32 slot = listener->cursor;

        listener->cursor = (listener->cursor + 1) % listener->max_connections;

        NYA_IpcConnection* connection = &listener->connections[slot];
        if (!connection->occupied) continue;

        if (!connection->announced) {
            connection->announced = true;

            *out_event = (NYA_IpcEvent){ .kind = NYA_IPC_EVENT_CONNECTED, .peer = connection->id };
            return true;
        }

        /*
         * Queued output before new input, so a reply written during the previous poll leaves before
         * more work arrives. The other order lets a peer that talks faster than it reads keep its own
         * answers stuck in the buffer until the send that overflows it.
         */
        if (connection->pending_size > 0 && !_nya_ipc_connection_flush(connection)) {
            NYA_IpcPeerId gone = connection->id;
            _nya_ipc_connection_close(connection);

            *out_event = (NYA_IpcEvent){ .kind = NYA_IPC_EVENT_DISCONNECTED, .peer = gone };
            return true;
        }

        connection->received_size = 0;

        s64 got = _nya_ipc_read(connection->handle, connection->received, sizeof(connection->received));

        if (got < 0) {
            NYA_IpcPeerId gone = connection->id;
            _nya_ipc_connection_close(connection);

            *out_event = (NYA_IpcEvent){ .kind = NYA_IPC_EVENT_DISCONNECTED, .peer = gone };
            return true;
        }

        if (got == 0) continue;

        nya_assert((u64)got <= sizeof(connection->received));
        connection->received_size = (u64)got;

        *out_event = (NYA_IpcEvent){
            .kind = NYA_IPC_EVENT_DATA,
            .peer = connection->id,
            .data = connection->received,
            .size = connection->received_size,
        };

        return true;
    }

    return false;
}

NYA_Error nya_ipc_listener_send(NYA_IpcListener* listener, NYA_IpcPeerId peer, const u8* data, u64 size) {
    nya_assert(listener != nullptr);
    nya_assert(data != nullptr || size == 0);

    if (size == 0) return NYA_OK;

    NYA_IpcConnection* connection = _nya_ipc_connection_find(listener, peer);
    if (connection == nullptr) return nya_error(NYA_ERROR_NOT_FOUND, "no live ipc connection %u/%u", peer.index, peer.generation);

    // Drained first so a peer that is reading normally never sees the queue fill up at all.
    if (connection->pending_size > 0 && !_nya_ipc_connection_flush(connection)) {
        _nya_ipc_connection_close(connection);
        return nya_error(NYA_ERROR_IO, "ipc connection %u/%u is gone", peer.index, peer.generation);
    }

    nya_assert(connection->pending_size <= sizeof(connection->pending));

    u64 room = sizeof(connection->pending) - connection->pending_size;
    if (size > room) {
        return nya_error(
            NYA_ERROR_OUT_OF_MEMORY,
            "ipc peer %u/%u is not reading: %llu bytes queued, %llu more asked for",
            peer.index,
            peer.generation,
            (unsigned long long)connection->pending_size,
            (unsigned long long)size
        );
    }

    nya_memcpy(connection->pending + connection->pending_size, data, size);
    connection->pending_size += size;

    // Opportunistic: the common case is a small reply the kernel takes whole, so it leaves here rather
    // than waiting a frame for the next poll.
    if (!_nya_ipc_connection_flush(connection)) {
        _nya_ipc_connection_close(connection);
        return nya_error(NYA_ERROR_IO, "ipc connection %u/%u is gone", peer.index, peer.generation);
    }

    return NYA_OK;
}

void nya_ipc_listener_disconnect(NYA_IpcListener* listener, NYA_IpcPeerId peer) {
    nya_assert(listener != nullptr);

    NYA_IpcConnection* connection = _nya_ipc_connection_find(listener, peer);
    if (connection == nullptr) return;

    _nya_ipc_connection_close(connection);
}

u32 nya_ipc_listener_connection_count(const NYA_IpcListener* listener) {
    nya_assert(listener != nullptr);

    u32 count = 0;
    for (u32 i = 0; i < NYA_IPC_MAX_CONNECTIONS; i++) count += listener->connections[i].occupied ? 1 : 0;

    nya_assert(count <= listener->max_connections);

    return count;
}

NYA_ConstCString nya_ipc_listener_endpoint(const NYA_IpcListener* listener) {
    nya_assert(listener != nullptr);

    return listener->endpoint;
}

/*
 * ─────────────────────────────────────────────────────────
 * CLIENT
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_ipc_client_create(NYA_Arena* arena, NYA_IpcName name, OUT NYA_IpcClient** out_client) {
    nya_assert(arena != nullptr);
    nya_assert(out_client != nullptr);

    *out_client = nullptr;

    if (name.text[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an ipc client needs a parsed name");

    NYA_IpcClient* client = nya_arena_alloc(arena, sizeof(NYA_IpcClient));
    if (client == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for an ipc client");

    *client = (NYA_IpcClient){ .allocator = arena, .handle = -1 };

    NYA_Error resolved = _nya_ipc_endpoint_resolve(&name, client->endpoint, sizeof(client->endpoint));
    if (!resolved.ok) {
        nya_arena_free(arena, client, sizeof(NYA_IpcClient));
        return resolved;
    }

    NYA_Error connected = _nya_ipc_connect(client->endpoint, &client->handle);
    if (!connected.ok) {
        nya_arena_free(arena, client, sizeof(NYA_IpcClient));
        return connected;
    }

    nya_assert(client->handle >= 0);

    *out_client = client;

    return NYA_OK;
}

void nya_ipc_client_destroy(NYA_IpcClient* client) {
    if (client == nullptr) return;

    _nya_ipc_close(client->handle);
    client->handle = -1;

    NYA_Arena* arena = client->allocator;
    nya_arena_free(arena, client, sizeof(NYA_IpcClient));
}

NYA_Error nya_ipc_client_send(NYA_IpcClient* client, const u8* data, u64 size) {
    nya_assert(client != nullptr);
    nya_assert(data != nullptr || size == 0);

    if (size == 0) return NYA_OK;
    if (client->handle < 0) return nya_error(NYA_ERROR_IO, "the ipc connection is closed");

    u64 written = 0;

    /*
     * Bounded by attempts rather than by time: a local pipe drains as fast as the other side reads, and
     * a peer that has stopped reading entirely must not hold this loop forever. The count is generous
     * enough that only a genuinely stuck reader reaches it.
     */
    for (u32 attempt = 0; attempt < _NYA_IPC_CLIENT_SEND_ATTEMPTS && written < size; attempt++) {
        s64 took = _nya_ipc_write(client->handle, data + written, size - written);

        if (took < 0) {
            _nya_ipc_close(client->handle);
            client->handle = -1;

            return nya_error(NYA_ERROR_IO, "the ipc peer is gone after %llu of %llu bytes", (unsigned long long)written, (unsigned long long)size);
        }

        written += (u64)took;
    }

    if (written < size)
        return nya_error(
            NYA_ERROR_TIMEOUT,
            "the ipc peer stopped reading after %llu of %llu bytes",
            (unsigned long long)written,
            (unsigned long long)size
        );

    return NYA_OK;
}

NYA_Error nya_ipc_client_receive(NYA_IpcClient* client, OUT u8* buffer, u64 capacity, OUT u64* out_size) {
    nya_assert(client != nullptr);
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);
    nya_assert(out_size != nullptr);

    *out_size = 0;

    if (client->handle < 0) return nya_error(NYA_ERROR_IO, "the ipc connection is closed");

    s64 got = _nya_ipc_read(client->handle, buffer, capacity);

    if (got < 0) {
        _nya_ipc_close(client->handle);
        client->handle = -1;

        return nya_error(NYA_ERROR_IO, "the ipc peer is gone");
    }

    nya_assert((u64)got <= capacity);
    *out_size = (u64)got;

    return NYA_OK;
}

b8 nya_ipc_client_is_connected(const NYA_IpcClient* client) {
    nya_assert(client != nullptr);

    return client->handle >= 0;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_IpcConnection* _nya_ipc_connection_find(NYA_IpcListener* listener, NYA_IpcPeerId peer) {
    nya_assert(listener != nullptr);

    if (!nya_ipc_peer_is_set(peer)) return nullptr;
    if (peer.index >= NYA_IPC_MAX_CONNECTIONS) return nullptr;

    NYA_IpcConnection* connection = &listener->connections[peer.index];

    if (!connection->occupied) return nullptr;
    if (connection->id.generation != peer.generation) return nullptr;

    return connection;
}

void _nya_ipc_connection_close(NYA_IpcConnection* connection) {
    nya_assert(connection != nullptr);

    if (connection->handle >= 0) _nya_ipc_close(connection->handle);

    // Everything reset but the id, which nobody reads once occupied is false, and the generation of the
    // slot, which lives on the listener so a reused slot never repeats an id.
    *connection = (NYA_IpcConnection){ .handle = -1 };
}

b8 _nya_ipc_connection_flush(NYA_IpcConnection* connection) {
    nya_assert(connection != nullptr);
    nya_assert(connection->occupied);
    nya_assert(connection->pending_size <= sizeof(connection->pending));

    if (connection->pending_size == 0) return true;

    s64 took = _nya_ipc_write(connection->handle, connection->pending, connection->pending_size);
    if (took < 0) return false;

    nya_assert((u64)took <= connection->pending_size);

    if ((u64)took == connection->pending_size) {
        connection->pending_size = 0;
        return true;
    }

    connection->pending_size -= (u64)took;
    nya_memmove(connection->pending, connection->pending + took, connection->pending_size);

    return true;
}

void _nya_ipc_listener_accept_pending(NYA_IpcListener* listener) {
    nya_assert(listener != nullptr);

    /*
     * Bounded by the connection limit rather than run until the queue is empty: a process that loops on
     * connect can otherwise keep this function running, and a bound that holds per call is the only kind
     * a remote party cannot argue with. Whatever is left waits for the next poll.
     */
    for (u32 attempt = 0; attempt < listener->max_connections; attempt++) {
        s64 handle = _nya_ipc_accept(listener);
        if (handle < 0) return;

        u32 slot = listener->max_connections;
        for (u32 i = 0; i < listener->max_connections; i++) {
            if (!listener->connections[i].occupied) {
                slot = i;
                break;
            }
        }

        // Full. Accepted and dropped rather than left in the backlog, so the caller gets an immediate
        // end of file instead of a connection that looks open and never answers.
        if (slot == listener->max_connections) {
            _nya_ipc_close(handle);
            return;
        }

        // Wraps back to one rather than to zero: generation zero is the "no peer" id.
        listener->next_generation++;
        if (listener->next_generation == 0) listener->next_generation = 1;

        listener->connections[slot] = (NYA_IpcConnection){
            .id        = { .index = slot, .generation = listener->next_generation },
            .occupied  = true,
            .announced = false,
            .handle    = handle,
        };
    }
}
