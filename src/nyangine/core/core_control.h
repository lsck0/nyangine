/**
 * @file core_control.h
 *
 * The control surface: a local socket another process drives this one through.
 *
 * Off unless a program turns it on, which is one call. Nothing here allocates, opens a handle or
 * costs a cycle until then, and a shipping build that never calls init links the same as before.
 *
 * It exposes the engine's own primitives rather than a list of commands invented for it. There are
 * three things a controlling process can do, and each is a thing the engine already does:
 *
 * ```
 * the event bus        dispatch an NYA_Event, or subscribe to the ones this program raises
 * reflection           read and write any struct the program has exposed by name
 * the greeting         ask what this build is and what it exposes, so a tool can adapt
 * ```
 *
 * Nothing else is added as the engine grows: a new event type is controllable the day it exists, and
 * a program makes its own state controllable by handing over a reflection and a pointer.
 *
 * ```
 * nya_system_control_init       binds the socket and starts listening
 * nya_system_control_deinit     closes it; everything below is a no-op again
 * nya_system_control_tick       drains the socket. Registered on the frame for you; see below
 *
 * nya_control_expose            makes a live struct readable and writable by name
 * nya_control_hide              the pair. A hidden object is unreachable from the socket immediately
 * nya_control_expose_event      makes an event type's payload readable and writable, by reflection
 * nya_control_hide_event        the pair
 *
 * nya_control_is_running        whether the socket is up
 * nya_control_endpoint          where it is, for the line that tells a person how to connect
 * nya_control_connection_count  how many processes are attached
 * ```
 *
 * ```c
 * // the program's own state, annotated with @reflect so the generator describes it
 * static VtuberFace FACE = { 0 };
 *
 * NYA_IpcName name = { 0 };
 * NYA_EXPECT(nya_ipc_name_parse("vtuber", &name));
 *
 * NYA_EXPECT(nya_system_control_init((NYA_ControlConfig){
 *     .name        = name,
 *     .permissions = NYA_CONTROL_PERMISSION_WRITE | NYA_CONTROL_PERMISSION_DISPATCH,
 * }));
 * defer nya_system_control_deinit();
 *
 * NYA_EXPECT(nya_control_expose("face", nya_reflect_of(VtuberFace), &FACE));
 * ```
 *
 * and from anywhere else on the machine, one length prefixed document at a time:
 *
 * ```python
 * import json, socket, struct
 * s = socket.socket(socket.AF_UNIX); s.connect(os.environ["XDG_RUNTIME_DIR"] + "/vtuber.sock")
 * def call(message):
 *     body = json.dumps(message).encode()
 *     s.sendall(struct.pack("<I", len(body)) + body)
 *     size, = struct.unpack("<I", s.recv(4))
 *     return json.loads(s.recv(size))
 *
 * call({ "op": "hello" })
 * call({ "op": "object.set", "name": "face", "value": { "mouth_open": 0.8 } })
 * call({ "op": "event.dispatch", "type": "CONTROL_MESSAGE", "name": "wave" })
 * ```
 *
 * ── the protocol ──
 *
 * A message is a four byte little endian length followed by that many bytes of a serde document, in
 * either the json or the nya format; which one is detected from the bytes, and a reply comes back in
 * the format the request arrived in. Length prefixing rather than one document per line, because the
 * engine's own format is multi line, and because a delimiter means scanning an unbounded amount of a
 * hostile peer's input before deciding it is too long.
 *
 * A request is an object with `op`, and optionally `id`, which is echoed so a caller can have more
 * than one request outstanding. A reply is an object with the same `id`, `ok`, and either the verb's
 * own fields or `error`. An event pushed to a subscriber has no `id`, which is how a client tells the
 * two apart.
 *
 * ```
 * hello                                    -> protocol, engine, build, permissions, objects, events
 * object.list                              -> every exposed name and its type
 * object.get         name                  -> value
 * object.set         name, value           -> ok            (needs WRITE)
 * event.dispatch     type, name?, body?    -> ok            (needs DISPATCH)
 * event.subscribe    types[]               -> ok
 * event.unsubscribe  types[]               -> ok
 * ```
 *
 * `event.dispatch` names a type from NYA_EVENT_NAME_MAP. NYA_EVENT_CONTROL_MESSAGE is the one that
 * exists for this: it carries a free `name` and a free `body`, so a program handles external commands
 * with an ordinary event hook and never learns that a socket was involved. Any other type needs its
 * payload described through nya_control_expose_event first, and a type whose payload is undescribed
 * is dispatched empty.
 *
 * ── what a hostile peer may do ──
 *
 * Anyone who can open the socket can drive this program, so the socket is owner-only and the answer to
 * "what if the peer is malicious" is a bound rather than a check. Every message is at most
 * NYA_CONTROL_MAX_MESSAGE_BYTES and a longer one drops that connection. At most
 * NYA_CONTROL_MAX_MESSAGES_PER_TICK are handled per frame, so a peer cannot take the frame. Nothing a
 * peer sends reaches an assertion: an unknown verb, a missing field, a name that is not exposed, a
 * type that is not an event, a value of the wrong type and a truncated document are all ordinary
 * replies with `ok` false. Permissions are checked before the verb runs, not inside it.
 *
 * Thread safety: none. Everything here runs on the thread that called init.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_reflection.h"
#include "nyangine/base/base_types.h"
#include "nyangine/core/core_event.h"
#include "nyangine/platform/ipc/ipc.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The version a client checks against what `hello` reports. Bumped when a verb changes meaning, never
 * when one is added: a client that knows nothing of a new verb is unaffected by it.
 * */
#define NYA_CONTROL_PROTOCOL_VERSION 1

/** Bytes of the length prefix in front of every message. Little endian, so it is the same on both platforms. */
#define NYA_CONTROL_HEADER_BYTES 4

/**
 * Largest message in either direction.
 *
 * One less header than the transport can queue, because a reply has to fit the peer's send buffer
 * whole: a reply that would not fit is refused rather than half written. A reflected struct of a few
 * hundred fields serializes well inside this, and a peer that announces more is dropped without the
 * bytes ever being read.
 * */
#define NYA_CONTROL_MAX_MESSAGE_BYTES (NYA_IPC_BUFFER_BYTES - NYA_CONTROL_HEADER_BYTES)

/**
 * Messages handled in one tick, across all connections.
 *
 * The frame is the thing being protected: the control surface exists to be driven by a person's tools
 * at human speed, and sixty four messages a frame is well past anything a control panel produces
 * while still being a number a runaway script cannot exceed at this program's expense.
 * */
#define NYA_CONTROL_MAX_MESSAGES_PER_TICK 64

/**
 * Structs that can be exposed at once. A program exposes its handful of controllable subsystems, not
 * its whole state; thirty two is past every use this was designed for and keeps the table a linear
 * scan, which for this many is faster than anything with a hash in it.
 * */
#define NYA_CONTROL_MAX_EXPOSED 32

/** Longest exposed name, the terminator included. */
#define NYA_CONTROL_MAX_EXPOSED_NAME 32

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_ControlPermission NYA_ControlPermission;
typedef struct NYA_ControlConfig   NYA_ControlConfig;

/**
 * What a connected process is allowed to do. Reading is always allowed, since a process that can open
 * the socket at all is already trusted with the program's state; the two that change something are
 * granted one at a time.
 * */
enum NYA_ControlPermission {
    /** hello, object.list, object.get, event.subscribe. What every connection gets. */
    NYA_CONTROL_PERMISSION_READ = 0,

    /** object.set. */
    NYA_CONTROL_PERMISSION_WRITE = 1 << 0,

    /** event.dispatch. */
    NYA_CONTROL_PERMISSION_DISPATCH = 1 << 1,

    NYA_CONTROL_PERMISSION_ALL = NYA_CONTROL_PERMISSION_WRITE | NYA_CONTROL_PERMISSION_DISPATCH,
};

struct NYA_ControlConfig {
    /** Required. Parse it with nya_ipc_name_parse. */
    NYA_IpcName name;

    /** Connections at once, 1..NYA_IPC_MAX_CONNECTIONS. Zero means the transport's maximum. */
    u32 max_connections;

    /**
     * What a connection may do beyond reading. Zero is read only, which is the safe default and what a
     * metrics or debug client needs.
     * */
    NYA_ControlPermission permissions;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

/**
 * Binds the socket and starts listening, and hooks the drain onto NYA_EVENT_HANDLING_STARTED so a
 * program that runs the engine's frame loop needs no second call.
 *
 * NYA_ERROR_ALREADY_EXISTS when this program is already listening, or when another process holds the
 * name. Fails before anything is allocated, so a failure leaves the subsystem exactly as off as it
 * was.
 * */
NYA_API NYA_Error nya_system_control_init(NYA_ControlConfig config) __attr_no_discard;

/**
 * Closes every connection, unbinds the socket and drops every subscription. Idempotent, and a no-op
 * when init was never called or failed.
 *
 * Exposures survive it, because they describe the program rather than the socket: a program that
 * stops and restarts the control surface does not re-register what it already said.
 * */
NYA_API void nya_system_control_deinit(void);

/**
 * Accepts, reads, and answers, for at most NYA_CONTROL_MAX_MESSAGES_PER_TICK messages. Never blocks.
 *
 * Called for you once a frame. Public for the two cases that need it directly: a headless program with
 * no frame loop, and a test that wants to drive the exchange one step at a time. Calling it twice in
 * one frame drains more and is otherwise harmless.
 * */
NYA_API void nya_system_control_tick(void);

/*
 * ─────────────────────────────────────────────────────────
 * EXPOSING
 * ─────────────────────────────────────────────────────────
 */

/**
 * Makes `instance` readable as `name`, and writable when the connection has WRITE.
 *
 * `instance` must outlive the exposure, since nothing copies it: the point is that a controlling
 * process reads and writes the live struct. Exposing a name twice replaces the earlier entry rather
 * than failing, so a hot reloaded module can re-expose its state without knowing whether it already
 * did.
 *
 * NYA_ERROR_INVALID_ARGUMENT for a name that is empty or too long, NYA_ERROR_OUT_OF_MEMORY once
 * NYA_CONTROL_MAX_EXPOSED names are taken. Safe to call before init, and the exposure survives it.
 * */
NYA_API NYA_Error nya_control_expose(NYA_ConstCString name, const NYA_TypeReflection* type, void* instance) __attr_no_discard;

/** Removes an exposure. A name that was never exposed is a no-op. */
NYA_API void nya_control_hide(NYA_ConstCString name);

/**
 * Describes the payload of `type`, so a subscriber receives it and `event.dispatch` can fill it.
 *
 * The engine describes none of its own: an event payload is a union member and which member is live is
 * decided by the event type, which is knowledge this file will not guess at. A program that wants key
 * events on the wire says so, once, and names the reflection.
 *
 * NYA_EVENT_CONTROL_MESSAGE needs no description; its payload is an object already.
 * */
NYA_API NYA_Error nya_control_expose_event(NYA_EventType type, const NYA_TypeReflection* payload) __attr_no_discard;

/** The pair. An event type that was never described is a no-op. */
NYA_API void nya_control_hide_event(NYA_EventType type);

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

NYA_API b8 nya_control_is_running(void) __attr_no_discard;

/** Where the socket is, or null when it is not running. */
NYA_API NYA_ConstCString nya_control_endpoint(void) __attr_no_discard;

NYA_API u32 nya_control_connection_count(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * FRAMING
 * ─────────────────────────────────────────────────────────
 */

/*
 * The two halves of the wire format, public because a controlling process written in C is a real case
 * and should not be reimplementing the header by hand. Both are pure functions over bytes.
 */

/**
 * Writes the length prefix for a message of `size` bytes into `out_header`.
 *
 * NYA_ERROR_INVALID_ARGUMENT when `size` is zero or over NYA_CONTROL_MAX_MESSAGE_BYTES, which is the
 * one place that limit is enforced for outgoing messages.
 * */
NYA_API NYA_Error nya_control_frame_encode(u64 size, OUT u8 out_header[NYA_CONTROL_HEADER_BYTES]) __attr_no_discard;

/**
 * Reads the length prefix at the front of `data`.
 *
 * False when fewer than NYA_CONTROL_HEADER_BYTES are available, which is a message still arriving
 * rather than a bad one. A length past NYA_CONTROL_MAX_MESSAGE_BYTES is returned as it was read, so
 * the caller can tell an oversized message from an incomplete one and drop the connection; every
 * caller must check it.
 * */
NYA_API b8 nya_control_frame_decode(const u8* data, u64 size, OUT u64* out_length);
