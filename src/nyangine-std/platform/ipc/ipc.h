/**
 * @file ipc.h
 *
 * A local control channel: a unix domain socket on Linux, a named pipe on Windows. One process
 * listens under a name, other processes on the same machine connect to that name, and bytes move
 * both ways. Nothing leaves the machine and no TCP port is bound.
 *
 * This is the transport only: it moves bytes and says who they came from. What those bytes mean is
 * core_control.h's problem, and a program that wants its own protocol over the same pipe can have
 * one without touching this file.
 *
 * ```
 * nya_ipc_name_parse            a name a listener may bind, or an error saying which rule failed
 *
 * nya_ipc_listener_create       binds the name and starts accepting; nothing is allocated before this
 * nya_ipc_listener_destroy      closes every connection, unlinks the endpoint
 * nya_ipc_listener_poll         one event: a peer arrived, left, or sent bytes. Never blocks
 * nya_ipc_listener_send         queues bytes to one peer. Never blocks
 * nya_ipc_listener_disconnect   drops one peer
 * nya_ipc_listener_connection_count
 * nya_ipc_listener_endpoint     the path or pipe name that was actually bound, for a log line
 *
 * nya_ipc_client_create         connects to a listening name
 * nya_ipc_client_destroy
 * nya_ipc_client_send
 * nya_ipc_client_receive        whatever has arrived, or nothing. Never blocks
 * nya_ipc_client_is_connected
 *
 * nya_ipc_peer_equals           whether two ids name the same connection
 * nya_ipc_peer_is_set           whether an id names a connection at all
 * ```
 *
 * ```c
 * NYA_IpcName name = { 0 };
 * NYA_TRY(nya_ipc_name_parse("nyangine", &name));
 *
 * NYA_IpcListener* listener = nullptr;
 * NYA_TRY(nya_ipc_listener_create(arena, (NYA_IpcOptions){ .name = name }, &listener));
 * defer nya_ipc_listener_destroy(listener);
 *
 * nya_log_info("control socket at %s", nya_ipc_listener_endpoint(listener));
 *
 * // once a frame. Bounded on purpose: see the note on nya_ipc_listener_poll.
 * NYA_IpcEvent event = { 0 };
 * for (u32 drained = 0; drained < 64 && nya_ipc_listener_poll(listener, &event); drained++) {
 *     if (event.kind == NYA_IPC_EVENT_DATA) handle(event.peer, event.data, event.size);
 * }
 * ```
 *
 * Everything here is untrusted. Any process the user can run can connect, so a peer is assumed to be
 * hostile: it may connect and never read, send a byte a second forever, send nothing at all, or
 * vanish mid message. None of that may cost this process more than the fixed buffers below, and none
 * of it is a reason to crash. The only assertions here are on the caller's own contract.
 *
 * The endpoint is a *name*, not a path. A name expands to `$XDG_RUNTIME_DIR/<name>.sock` on Linux and
 * `\\.\pipe\<name>` on Windows, so one configuration string works on both. Taking a path instead was
 * rejected twice over: it does not port, and it hands a config file the power to bind anywhere the
 * process can write, which is a traversal bug waiting for a badly merged settings file.
 *
 * Thread safety: none. One thread owns a listener or a client for its whole life.
 * */
#pragma once

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_error.h"
#include "nyangine-std/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Connections one listener holds at once. A control channel is driven by a person's own tools: a
 * control panel, a script and a spare terminal is three, so four leaves one and still keeps the fixed
 * cost of the whole feature under a quarter of a megabyte. The connection past the last is refused
 * and closed immediately rather than queued, so a process that spams connect cannot grow this.
 * */
#ifndef NYA_IPC_MAX_CONNECTIONS
#define NYA_IPC_MAX_CONNECTIONS 4
#endif

/**
 * Longest endpoint name, the terminator included. Chosen from the tightest platform limit rather than
 * a round number: `sun_path` is 108 bytes and has to hold a runtime directory, a separator, the name
 * and ".sock", which leaves this much for the name itself on any sane `$XDG_RUNTIME_DIR`.
 * */
#define NYA_IPC_MAX_NAME 48

/**
 * The expanded endpoint: `sun_path` on Linux, which is the larger of the two.
 * */
#define NYA_IPC_MAX_ENDPOINT 108

/**
 * Bytes held per connection in each direction. One read per poll fills at most this much, and a send
 * to a peer that is not reading fails once this much is already queued rather than growing.
 *
 * Sized from the largest thing the control surface sends, a reflected object of a few hundred fields,
 * with room to spare. Four connections cost 4 x 2 x this, which is the whole memory footprint of the
 * feature.
 * */
#define NYA_IPC_BUFFER_BYTES 16384

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_IpcName       NYA_IpcName;
typedef struct NYA_IpcPeerId     NYA_IpcPeerId;
typedef enum NYA_IpcEventKind    NYA_IpcEventKind;
typedef struct NYA_IpcEvent      NYA_IpcEvent;
typedef struct NYA_IpcOptions    NYA_IpcOptions;
typedef struct NYA_IpcConnection NYA_IpcConnection;
typedef struct NYA_IpcListener   NYA_IpcListener;
typedef struct NYA_IpcClient     NYA_IpcClient;

/**
 * A name that is known to be bindable, because the only way to make one is to parse it.
 *
 * Nothing downstream re-checks it and nothing downstream can be handed the raw string instead, which
 * is the point: the rules live in nya_ipc_name_parse and nowhere else.
 * */
struct NYA_IpcName {
    /** Null terminated, already checked against the rules in nya_ipc_name_parse. */
    char text[NYA_IPC_MAX_NAME];
};

/**
 * Identifies one connection for as long as it lives.
 *
 * The generation is what makes a stale id safe: a slot reused by the next peer does not answer to the
 * id of the one before it, so a send that races a disconnect is refused rather than delivered to a
 * stranger.
 * */
struct NYA_IpcPeerId {
    u32 index;
    u32 generation;
};

#define NYA_IPC_PEER_NONE ((NYA_IpcPeerId){ .index = 0, .generation = 0 })

enum NYA_IpcEventKind {
    NYA_IPC_EVENT_NONE = 0,

    /** A process connected. It has sent nothing yet. */
    NYA_IPC_EVENT_CONNECTED,

    /** A peer is gone, by its own hand or ours. Its id never resolves again. */
    NYA_IPC_EVENT_DISCONNECTED,

    /** Bytes arrived. See NYA_IpcEvent.data. */
    NYA_IPC_EVENT_DATA,

    NYA_IPC_EVENT_KIND_COUNT,
};

/** One thing that happened, drained by nya_ipc_listener_poll. */
struct NYA_IpcEvent {
    NYA_IpcEventKind kind;
    NYA_IpcPeerId    peer;

    /**
     * DATA only, and a stream rather than a message: whatever the kernel had, which may be half of
     * what the peer sent or several things at once. Framing is the caller's job.
     *
     * Points into the listener's own buffer and is valid until the next call on that listener. Never
     * null and never empty for a DATA event.
     * */
    const u8* data;
    u64       size;
};

struct NYA_IpcOptions {
    /** Required. Parse it with nya_ipc_name_parse; a zeroed name is rejected. */
    NYA_IpcName name;

    /**
     * Connections to hold at once, 1..NYA_IPC_MAX_CONNECTIONS. Zero means the maximum.
     * */
    u32 max_connections;
};

/**
 * One accepted connection. Transparent because it is data, with one exception marked below.
 * */
struct NYA_IpcConnection {
    NYA_IpcPeerId id;

    b8 occupied;

    /** Whether its CONNECTED event has been handed out yet. */
    b8 announced;

    /**
     * The platform's handle, and the one field here that is not portable data: a file descriptor on
     * Linux, a HANDLE on Windows, -1 when the slot is free. Read it and you have written code that
     * only builds on one of them.
     * */
    s64 handle;

    /** What the last read produced, handed out by the next poll. */
    u8  received[NYA_IPC_BUFFER_BYTES];
    u64 received_size;

    /** Queued by send, drained by poll as the peer reads. */
    u8  pending[NYA_IPC_BUFFER_BYTES];
    u64 pending_size;
};

struct NYA_IpcListener {
    NYA_Arena* allocator;

    NYA_IpcName name;

    /** What `name` expanded to. Null terminated. */
    char endpoint[NYA_IPC_MAX_ENDPOINT];

    /** The listening socket or the pipe instance waiting for a connection. See NYA_IpcConnection. */
    s64 handle;

    u32 max_connections;

    /**
     * Handed to the next accepted connection. Starts at one so a zeroed NYA_IpcPeerId is never valid.
     * */
    u32 next_generation;

    /** Where the next poll starts looking, so one chatty peer cannot starve the others. */
    u32 cursor;

    NYA_IpcConnection connections[NYA_IPC_MAX_CONNECTIONS];
};

struct NYA_IpcClient {
    NYA_Arena* allocator;

    /** See NYA_IpcConnection. -1 once the connection is gone. */
    s64 handle;

    char endpoint[NYA_IPC_MAX_ENDPOINT];
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * NAMES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Parses `text` into a name a listener may bind.
 *
 * The rules, and the error says which one failed: between 1 and NYA_IPC_MAX_NAME - 1 characters, and
 * every character one of `a-z A-Z 0-9 . _ -`. No separator of either platform survives that, so a
 * name can never climb out of the directory it expands into, and no name is legal on one platform and
 * not the other.
 * */
NYA_API NYA_Error nya_ipc_name_parse(NYA_ConstCString text, OUT NYA_IpcName* out_name) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * LISTENER
 * ─────────────────────────────────────────────────────────
 */

/**
 * Binds `options.name` and starts accepting.
 *
 * Fails rather than stealing the endpoint when another process is already listening under the same
 * name, so two copies of a program cannot silently fight over one control channel. A dead process's
 * leftover socket file is not another listener and is replaced.
 *
 * Allocates exactly one NYA_IpcListener from `arena` and nothing else, ever. Nothing in this module
 * allocates before this call, which is what makes the feature free when it is off.
 * */
NYA_API NYA_Error nya_ipc_listener_create(NYA_Arena* arena, NYA_IpcOptions options, OUT NYA_IpcListener** out_listener) __attr_no_discard;

/** Closes every connection and removes the endpoint. Null is a no-op, so this pairs with a failed create. */
NYA_API void nya_ipc_listener_destroy(NYA_IpcListener* listener);

/**
 * Hands out one event and returns true, or returns false when there is nothing to report. Never
 * blocks, and does at most one read and one write per connection.
 *
 * Bound your drain. A peer that sends without pause can keep this returning true for as long as it
 * likes, so the loop that calls it belongs inside a counter, as in the example at the top of this
 * file. That is the same contract nya_net_transport_poll carries and for the same reason.
 * */
NYA_API b8 nya_ipc_listener_poll(NYA_IpcListener* listener, OUT NYA_IpcEvent* out_event);

/**
 * Queues `size` bytes for `peer`. Never blocks: what the kernel takes now is written now and the rest
 * waits for the next poll.
 *
 * NYA_ERROR_NOT_FOUND when the id names no live connection, which is the ordinary answer to a send
 * that raced a disconnect. NYA_ERROR_OUT_OF_MEMORY when the peer already has NYA_IPC_BUFFER_BYTES
 * queued and is not reading; the connection is left up and the caller decides whether to drop it.
 * */
NYA_API NYA_Error nya_ipc_listener_send(NYA_IpcListener* listener, NYA_IpcPeerId peer, const u8* data, u64 size) __attr_no_discard;

/** Drops `peer`. Anything queued for it is discarded. An id that names nothing is a no-op. */
NYA_API void nya_ipc_listener_disconnect(NYA_IpcListener* listener, NYA_IpcPeerId peer);

NYA_API u32 nya_ipc_listener_connection_count(const NYA_IpcListener* listener) __attr_no_discard;

/** The path or pipe name that was bound, for the log line that tells a person where to connect. */
NYA_API NYA_ConstCString nya_ipc_listener_endpoint(const NYA_IpcListener* listener) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * CLIENT
 * ─────────────────────────────────────────────────────────
 */

/**
 * Connects to a process listening under `name`.
 *
 * NYA_ERROR_NOT_FOUND when nothing is listening, which is the normal answer when the program being
 * controlled is not running. Blocks only for as long as a local connect takes, which is a syscall.
 * */
NYA_API NYA_Error nya_ipc_client_create(NYA_Arena* arena, NYA_IpcName name, OUT NYA_IpcClient** out_client) __attr_no_discard;

/** Closes the connection. Null is a no-op. */
NYA_API void nya_ipc_client_destroy(NYA_IpcClient* client);

/**
 * Sends `size` bytes, all of them. Unlike the listener side this waits for a full buffer to drain,
 * because a client is a tool talking to one process and half a message is worse than a short wait.
 * */
NYA_API NYA_Error nya_ipc_client_send(NYA_IpcClient* client, const u8* data, u64 size) __attr_no_discard;

/**
 * Copies whatever has arrived into `buffer`, up to `capacity`, and writes how much into
 * `out_size`. Zero means nothing was waiting, which is not an error. Never blocks.
 *
 * NYA_ERROR_IO once the other end is gone; the client is closed by then and is_connected is false.
 * */
NYA_API NYA_Error nya_ipc_client_receive(NYA_IpcClient* client, OUT u8* buffer, u64 capacity, OUT u64* out_size) __attr_no_discard;

NYA_API b8 nya_ipc_client_is_connected(const NYA_IpcClient* client) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * PEER IDS
 * ─────────────────────────────────────────────────────────
 */

NYA_API b8 nya_ipc_peer_equals(NYA_IpcPeerId a, NYA_IpcPeerId b) __attr_no_discard;

/** Whether an id names a connection at all. Says nothing about whether that connection is still up. */
NYA_API b8 nya_ipc_peer_is_set(NYA_IpcPeerId peer) __attr_no_discard;
