#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct _NYA_ControlConnection _NYA_ControlConnection;
typedef struct _NYA_ControlExposure   _NYA_ControlExposure;
typedef struct _NYA_ControlState      _NYA_ControlState;

/** Bytes of subscription bitset, one bit per NYA_EventType. */
#define _NYA_CONTROL_SUBSCRIPTION_BYTES ((NYA_EVENT_COUNT + 7) / 8)

/**
 * How deep a document may be walked while checking an object.set for string targets. The json parser
 * already refuses anything deeper than NYA_SERDE_JSON_DEPTH_MAX, and a reflected struct nests nowhere
 * near this far, so reaching it means a cycle and the walk stops rather than recurring forever.
 * */
#define _NYA_CONTROL_MAX_DEPTH 32

struct _NYA_ControlConnection {
    b8 occupied;

    NYA_IpcPeerId peer;

    /** The format this peer last spoke, so a reply comes back in the same one. */
    NYA_SerdeFormat format;

    /** One bit per event type; set means this connection wants it pushed. */
    u8 subscriptions[_NYA_CONTROL_SUBSCRIPTION_BYTES];

    /**
     * Bytes received and not yet a whole message. Bounded by construction: the length prefix is checked
     * against NYA_CONTROL_MAX_MESSAGE_BYTES before a single byte of the body is kept.
     * */
    u8  assembly[NYA_CONTROL_HEADER_BYTES + NYA_CONTROL_MAX_MESSAGE_BYTES];
    u64 assembly_size;
};

struct _NYA_ControlExposure {
    char name[NYA_CONTROL_MAX_EXPOSED_NAME];

    const NYA_TypeReflection* type;
    void*                     instance;
};

struct _NYA_ControlState {
    NYA_Arena* allocator;

    /**
     * Everything one tick parses and builds. Reset at the *start* of a tick rather than the end, so a
     * CONTROL_MESSAGE dispatched during the tick still has its name and body when the deferred half of
     * the event queue is drained later in the same frame.
     * */
    NYA_Arena* scratch;

    NYA_ControlConfig config;

    NYA_IpcListener* listener;

    b8 running;

    _NYA_ControlConnection connections[NYA_IPC_MAX_CONNECTIONS];

    /**
     * Taken once, at init. A hot reloading build hands out a fresh handle for every nya_callback, so a
     * hook unregistered with a second call would not match the one that was registered.
     * */
    NYA_CallbackHandle frame_hook;
    NYA_CallbackHandle event_hook;

    /** Connections subscribed to each type, so the engine hook is registered exactly while wanted. */
    u32 subscriber_count[NYA_EVENT_COUNT];
};

/**
 * Null until init. Allocated rather than a file scope struct so that a program which never turns the
 * control surface on carries none of this, not even zeroed pages.
 * */
NYA_INTERNAL _NYA_ControlState* _NYA_CONTROL = nullptr;

/*
 * Exposures outlive a socket on purpose: they describe the program, not the connection, so deinit and
 * a later init do not make a program re-register what it already said. They live here for that reason.
 */
NYA_INTERNAL _NYA_ControlExposure      _NYA_CONTROL_EXPOSED[NYA_CONTROL_MAX_EXPOSED];
NYA_INTERNAL u32                       _NYA_CONTROL_EXPOSED_COUNT = 0;
NYA_INTERNAL const NYA_TypeReflection* _NYA_CONTROL_EVENT_PAYLOAD[NYA_EVENT_COUNT];

/* ── connections ── */

NYA_INTERNAL _NYA_ControlConnection* _nya_control_connection_find(NYA_IpcPeerId peer) __attr_no_discard;
NYA_INTERNAL void                    _nya_control_connection_open(NYA_IpcPeerId peer);
NYA_INTERNAL void                    _nya_control_connection_close(NYA_IpcPeerId peer);

/** Drops a peer for breaking the protocol, saying why in the log. */
NYA_INTERNAL void _nya_control_connection_reject(NYA_IpcPeerId peer, NYA_ConstCString reason);

/** Appends `size` bytes and runs whatever whole messages that completed, up to `budget`. */
NYA_INTERNAL void _nya_control_connection_receive(NYA_IpcPeerId peer, const u8* data, u64 size, u32* budget);

/* ── messages ── */

/** Serializes `message` in the peer's format, frames it, and queues it. Drops the peer on failure. */
NYA_INTERNAL void _nya_control_send(NYA_IpcPeerId peer, const NYA_Object* message);

/** Builds `{ id, ok: false, error }` and sends it. */
NYA_INTERNAL void _nya_control_send_error(NYA_IpcPeerId peer, const NYA_Value* id, NYA_ConstCString format, ...) __attr_fmt_printf(3, 4);

/** Runs one parsed request. Never fails: every refusal is a reply. */
NYA_INTERNAL void _nya_control_handle(NYA_IpcPeerId peer, const NYA_Object* request);

NYA_INTERNAL void _nya_control_handle_hello(NYA_IpcPeerId peer, const NYA_Object* request, NYA_Object* reply);
NYA_INTERNAL void _nya_control_handle_object_list(NYA_Object* reply);
NYA_INTERNAL void _nya_control_handle_object_get(NYA_IpcPeerId peer, const NYA_Object* request, NYA_Object* reply, OUT b8* out_ok);
NYA_INTERNAL void _nya_control_handle_object_set(NYA_IpcPeerId peer, const NYA_Object* request, OUT b8* out_ok);
NYA_INTERNAL void _nya_control_handle_event_dispatch(NYA_IpcPeerId peer, const NYA_Object* request, OUT b8* out_ok);
NYA_INTERNAL void _nya_control_handle_event_subscription(NYA_IpcPeerId peer, const NYA_Object* request, b8 subscribe, OUT b8* out_ok);

/* ── helpers ── */

/** A string field, or null when it is missing or is not a string. */
NYA_INTERNAL NYA_ConstCString _nya_control_field_string(const NYA_Object* object, NYA_ConstCString key) __attr_no_discard;

/** The exposure called `name`, or null. */
NYA_INTERNAL _NYA_ControlExposure* _nya_control_exposure_find(NYA_ConstCString name) __attr_no_discard;

/** The event type spelled `name` in NYA_EVENT_NAME_MAP, or NYA_EVENT_COUNT when there is no such name. */
NYA_INTERNAL NYA_EventType _nya_control_event_type_from_name(NYA_ConstCString name) __attr_no_discard;

/**
 * Whether any value in `object` would land in a field the reflection describes as a string.
 *
 * Such a write would store the caller's pointer into the live struct, and that pointer belongs to the
 * scratch arena this tick is about to reuse. Refused rather than copied: see the note at the call site.
 * */
NYA_INTERNAL b8 _nya_control_writes_a_string(const NYA_TypeReflection* type, const NYA_Object* object, u32 depth, OUT NYA_ConstCString* out_field)
    __attr_no_discard;

/** Pushes `event` to every connection subscribed to its type. Registered as an engine hook. */
NYA_INTERNAL void _nya_control_on_engine_event(NYA_Event* event);

/** Keeps the engine hook for `type` registered exactly while at least one connection wants it. */
NYA_INTERNAL void _nya_control_subscription_set(_NYA_ControlConnection* connection, NYA_EventType type, b8 wanted);

/** The tick, as the frame calls it. */
NYA_INTERNAL void _nya_control_on_frame(NYA_Event* event);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * SYSTEM FUNCTIONS
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_system_control_init(NYA_ControlConfig config) {
    if (_NYA_CONTROL != nullptr) return nya_error(NYA_ERROR_ALREADY_EXISTS, "the control surface is already running");

    NYA_Arena* arena = nya_arena_create(.name = "control");
    if (arena == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the control surface");

    NYA_IpcListener* listener = nullptr;

    NYA_Error listening =
        nya_ipc_listener_create(arena, (NYA_IpcOptions){ .name = config.name, .max_connections = config.max_connections }, &listener);

    if (!listening.ok) {
        nya_arena_destroy(arena);
        return listening;
    }

    _NYA_ControlState* state = nya_arena_alloc(arena, sizeof(_NYA_ControlState));
    if (state == nullptr) {
        nya_ipc_listener_destroy(listener);
        nya_arena_destroy(arena);

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the control surface");
    }

    *state = (_NYA_ControlState){
        .allocator = arena,
        .scratch   = nya_arena_create(.name = "control_tick"),
        .config    = config,
        .listener  = listener,
        .running   = true,
    };

    state->frame_hook = nya_callback(_nya_control_on_frame);
    state->event_hook = nya_callback(_nya_control_on_engine_event);

    if (state->scratch == nullptr) {
        nya_ipc_listener_destroy(listener);
        nya_arena_destroy(arena);

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "no room for the control surface's scratch");
    }

    _NYA_CONTROL = state;

    /*
     * Drained where input is drained. A command that arrives over the socket is input like a keypress,
     * so it lands at the same point in the frame and a handler cannot tell the two apart by timing.
     * Immediate rather than deferred, because a deferred hook runs while the queue it would add to is
     * being walked.
     */
    nya_event_hook_register((NYA_EventHook){
        .event_type = NYA_EVENT_HANDLING_STARTED,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = state->frame_hook,
    });

    nya_log_info("Control surface listening on %s", nya_ipc_listener_endpoint(listener));

    return NYA_OK;
}

void nya_system_control_deinit(void) {
    if (_NYA_CONTROL == nullptr) return;

    nya_event_hook_unregister((NYA_EventHook){
        .event_type = NYA_EVENT_HANDLING_STARTED,
        .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
        .fn         = _NYA_CONTROL->frame_hook,
    });

    // Every subscription, so no hook survives the socket that fed it.
    for (u32 type = 0; type < NYA_EVENT_COUNT; type++) {
        if (_NYA_CONTROL->subscriber_count[type] == 0) continue;

        nya_event_hook_unregister((NYA_EventHook){
            .event_type = (NYA_EventType)type,
            .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
            .fn         = _NYA_CONTROL->event_hook,
        });

        _NYA_CONTROL->subscriber_count[type] = 0;
    }

    nya_ipc_listener_destroy(_NYA_CONTROL->listener);
    nya_arena_destroy(_NYA_CONTROL->scratch);

    NYA_Arena* arena = _NYA_CONTROL->allocator;
    _NYA_CONTROL     = nullptr;

    nya_arena_destroy(arena);
}

void nya_system_control_tick(void) {
    if (_NYA_CONTROL == nullptr || !_NYA_CONTROL->running) return;

    // At the start: everything this tick allocates has to outlive the tick itself, because a dispatched
    // event is read later in the same frame. See the field's note.
    nya_arena_free_all(_NYA_CONTROL->scratch);

    u32 budget = NYA_CONTROL_MAX_MESSAGES_PER_TICK;

    /*
     * Two bounds, both of them per tick: how many events are drained and how many messages are run. A
     * peer that never stops sending gets exactly this much of the frame and no more.
     */
    for (u32 step = 0; step < NYA_CONTROL_MAX_MESSAGES_PER_TICK && budget > 0; step++) {
        NYA_IpcEvent event = { 0 };
        if (!nya_ipc_listener_poll(_NYA_CONTROL->listener, &event)) break;

        switch (event.kind) {
            case NYA_IPC_EVENT_CONNECTED:    _nya_control_connection_open(event.peer); break;
            case NYA_IPC_EVENT_DISCONNECTED: _nya_control_connection_close(event.peer); break;
            case NYA_IPC_EVENT_DATA:         _nya_control_connection_receive(event.peer, event.data, event.size, &budget); break;

            case NYA_IPC_EVENT_NONE:
            case NYA_IPC_EVENT_KIND_COUNT:
            default:                         nya_unreachable();
        }
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * EXPOSING
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_control_expose(NYA_ConstCString name, const NYA_TypeReflection* type, void* instance) {
    if (name == nullptr || name[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an exposed object needs a name");
    if (type == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' needs a reflection", name);
    if (instance == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' needs an instance", name);

    if (type->kind != NYA_REFLECT_STRUCT && type->kind != NYA_REFLECT_UNION) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is a %s, and only a struct can be exposed", name, type->name);
    }

    u64 length = strlen(name);
    if (length >= NYA_CONTROL_MAX_EXPOSED_NAME) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is longer than %u characters", name, (u32)(NYA_CONTROL_MAX_EXPOSED_NAME - 1));
    }

    _NYA_ControlExposure* existing = _nya_control_exposure_find(name);

    if (existing == nullptr) {
        if (_NYA_CONTROL_EXPOSED_COUNT >= NYA_CONTROL_MAX_EXPOSED) {
            return nya_error(NYA_ERROR_OUT_OF_MEMORY, "%u objects are already exposed", (u32)NYA_CONTROL_MAX_EXPOSED);
        }

        existing = &_NYA_CONTROL_EXPOSED[_NYA_CONTROL_EXPOSED_COUNT++];
    }

    *existing = (_NYA_ControlExposure){ .type = type, .instance = instance };
    nya_memcpy(existing->name, name, length);

    return NYA_OK;
}

void nya_control_hide(NYA_ConstCString name) {
    if (name == nullptr) return;

    _NYA_ControlExposure* found = _nya_control_exposure_find(name);
    if (found == nullptr) return;

    // Order does not matter here, so the last entry fills the hole and the scan stays contiguous.
    u32 index = (u32)(found - _NYA_CONTROL_EXPOSED);
    nya_assert(index < _NYA_CONTROL_EXPOSED_COUNT);

    _NYA_CONTROL_EXPOSED[index] = _NYA_CONTROL_EXPOSED[_NYA_CONTROL_EXPOSED_COUNT - 1];
    _NYA_CONTROL_EXPOSED_COUNT--;
    _NYA_CONTROL_EXPOSED[_NYA_CONTROL_EXPOSED_COUNT] = (_NYA_ControlExposure){ 0 };
}

NYA_Error nya_control_expose_event(NYA_EventType type, const NYA_TypeReflection* payload) {
    if (type <= NYA_EVENT_INVALID || type >= NYA_EVENT_COUNT) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "%d is not an event type", (s32)type);
    if (payload == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an event payload needs a reflection");

    if (payload->kind != NYA_REFLECT_STRUCT) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not a struct, so it cannot be an event payload", payload->name);
    }

    // The payload has to fit the union it is read out of, or reading it walks past the event.
    if (payload->size > sizeof(NYA_Event)) { return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is larger than an NYA_Event", payload->name); }

    _NYA_CONTROL_EVENT_PAYLOAD[type] = payload;

    return NYA_OK;
}

void nya_control_hide_event(NYA_EventType type) {
    if (type <= NYA_EVENT_INVALID || type >= NYA_EVENT_COUNT) return;

    _NYA_CONTROL_EVENT_PAYLOAD[type] = nullptr;
}

/*
 * ─────────────────────────────────────────────────────────
 * INTROSPECTION
 * ─────────────────────────────────────────────────────────
 */

b8 nya_control_is_running(void) {
    return _NYA_CONTROL != nullptr && _NYA_CONTROL->running;
}

NYA_ConstCString nya_control_endpoint(void) {
    if (_NYA_CONTROL == nullptr) return nullptr;

    return nya_ipc_listener_endpoint(_NYA_CONTROL->listener);
}

u32 nya_control_connection_count(void) {
    if (_NYA_CONTROL == nullptr) return 0;

    return nya_ipc_listener_connection_count(_NYA_CONTROL->listener);
}

/*
 * ─────────────────────────────────────────────────────────
 * FRAMING
 * ─────────────────────────────────────────────────────────
 */

NYA_Error nya_control_frame_encode(u64 size, OUT u8 out_header[NYA_CONTROL_HEADER_BYTES]) {
    nya_assert(out_header != nullptr);

    if (size == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an empty control message");

    if (size > NYA_CONTROL_MAX_MESSAGE_BYTES) {
        return nya_error(
            NYA_ERROR_INVALID_ARGUMENT,
            "a control message of %llu bytes; the limit is %llu",
            (unsigned long long)size,
            (unsigned long long)NYA_CONTROL_MAX_MESSAGE_BYTES
        );
    }

    out_header[0] = (u8)(size & 0xFFU);
    out_header[1] = (u8)((size >> 8) & 0xFFU);
    out_header[2] = (u8)((size >> 16) & 0xFFU);
    out_header[3] = (u8)((size >> 24) & 0xFFU);

    return NYA_OK;
}

b8 nya_control_frame_decode(const u8* data, u64 size, OUT u64* out_length) {
    nya_assert(out_length != nullptr);

    *out_length = 0;

    if (data == nullptr || size < NYA_CONTROL_HEADER_BYTES) return false;

    *out_length = (u64)data[0] | ((u64)data[1] << 8) | ((u64)data[2] << 16) | ((u64)data[3] << 24);

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * HELPERS
 * ─────────────────────────────────────────────────────────
 */

NYA_ConstCString _nya_control_field_string(const NYA_Object* object, NYA_ConstCString key) {
    if (object == nullptr || key == nullptr) return nullptr;

    NYA_Value* value = nya_object_get(object, (NYA_CString)key);
    if (value == nullptr || value->type != NYA_TYPE_STRING || value->as_string == nullptr) return nullptr;

    return value->as_string;
}

_NYA_ControlExposure* _nya_control_exposure_find(NYA_ConstCString name) {
    if (name == nullptr) return nullptr;

    for (u32 i = 0; i < _NYA_CONTROL_EXPOSED_COUNT; i++) {
        if (nya_string_equals((NYA_ConstCString)_NYA_CONTROL_EXPOSED[i].name, name)) return &_NYA_CONTROL_EXPOSED[i];
    }

    return nullptr;
}

NYA_EventType _nya_control_event_type_from_name(NYA_ConstCString name) {
    if (name == nullptr) return NYA_EVENT_COUNT;

    for (u32 type = 0; type < NYA_EVENT_COUNT; type++) {
        if (NYA_EVENT_NAME_MAP[type] == nullptr) continue;
        if (nya_string_equals(NYA_EVENT_NAME_MAP[type], name)) return (NYA_EventType)type;
    }

    return NYA_EVENT_COUNT;
}

b8 _nya_control_writes_a_string(const NYA_TypeReflection* type, const NYA_Object* object, u32 depth, OUT NYA_ConstCString* out_field) {
    nya_assert(out_field != nullptr);

    if (type == nullptr || object == nullptr) return false;
    if (depth >= _NYA_CONTROL_MAX_DEPTH) return false;

    for (u32 i = 0; i < type->field_count; i++) {
        const NYA_ReflectField*   field      = &type->fields[i];
        const NYA_TypeReflection* field_type = field->type;

        if (field_type == nullptr) continue;

        NYA_Value* value = nya_object_get(object, (NYA_CString)field->name);
        if (value == nullptr) continue;

        if (field_type->kind == NYA_REFLECT_PRIMITIVE && field_type->primitive == NYA_TYPE_STRING) {
            *out_field = field->name;
            return true;
        }

        b8 nested_kind = field_type->kind == NYA_REFLECT_STRUCT || field_type->kind == NYA_REFLECT_UNION;

        if (nested_kind && value->type == NYA_TYPE_OBJECT && _nya_control_writes_a_string(field_type, &value->as_object, depth + 1, out_field)) {
            return true;
        }
    }

    return false;
}

/*
 * ─────────────────────────────────────────────────────────
 * CONNECTIONS
 * ─────────────────────────────────────────────────────────
 */

_NYA_ControlConnection* _nya_control_connection_find(NYA_IpcPeerId peer) {
    if (_NYA_CONTROL == nullptr) return nullptr;
    if (!nya_ipc_peer_is_set(peer)) return nullptr;
    if (peer.index >= NYA_IPC_MAX_CONNECTIONS) return nullptr;

    _NYA_ControlConnection* connection = &_NYA_CONTROL->connections[peer.index];

    if (!connection->occupied) return nullptr;
    if (!nya_ipc_peer_equals(connection->peer, peer)) return nullptr;

    return connection;
}

void _nya_control_connection_open(NYA_IpcPeerId peer) {
    nya_assert(_NYA_CONTROL != nullptr);
    nya_assert(peer.index < NYA_IPC_MAX_CONNECTIONS);

    _NYA_ControlConnection* connection = &_NYA_CONTROL->connections[peer.index];

    // A slot the transport has just handed out cannot still be ours; if it is, the previous peer's
    // subscriptions have to go before the new one inherits them.
    if (connection->occupied) _nya_control_connection_close(connection->peer);

    *connection = (_NYA_ControlConnection){
        .occupied = true,
        .peer     = peer,
        // Until the peer says otherwise. json, because a tool written in any other language has one.
        .format = NYA_SERDE_FORMAT_JSON,
    };

    nya_log_debug("Control surface: peer %u/%u connected", peer.index, peer.generation);
}

void _nya_control_connection_close(NYA_IpcPeerId peer) {
    _NYA_ControlConnection* connection = _nya_control_connection_find(peer);
    if (connection == nullptr) return;

    for (u32 type = 0; type < NYA_EVENT_COUNT; type++) _nya_control_subscription_set(connection, (NYA_EventType)type, false);

    *connection = (_NYA_ControlConnection){ 0 };

    nya_log_debug("Control surface: peer %u/%u disconnected", peer.index, peer.generation);
}

void _nya_control_connection_reject(NYA_IpcPeerId peer, NYA_ConstCString reason) {
    nya_assert(_NYA_CONTROL != nullptr);
    nya_assert(reason != nullptr);

    nya_log_warn("Control surface: dropping peer %u/%u: %s", peer.index, peer.generation, reason);

    _nya_control_connection_close(peer);
    nya_ipc_listener_disconnect(_NYA_CONTROL->listener, peer);
}

void _nya_control_connection_receive(NYA_IpcPeerId peer, const u8* data, u64 size, u32* budget) {
    nya_assert(_NYA_CONTROL != nullptr);
    nya_assert(data != nullptr);
    nya_assert(budget != nullptr);

    _NYA_ControlConnection* connection = _nya_control_connection_find(peer);
    if (connection == nullptr) return;

    nya_assert(connection->assembly_size <= sizeof(connection->assembly));

    if (size > sizeof(connection->assembly) - connection->assembly_size) {
        _nya_control_connection_reject(peer, "sent more than one message may be");
        return;
    }

    nya_memcpy(connection->assembly + connection->assembly_size, data, size);
    connection->assembly_size += size;

    while (*budget > 0) {
        u64 length = 0;
        if (!nya_control_frame_decode(connection->assembly, connection->assembly_size, &length)) return;

        if (length == 0 || length > NYA_CONTROL_MAX_MESSAGE_BYTES) {
            _nya_control_connection_reject(peer, "announced a message length outside the limit");
            return;
        }

        u64 whole = NYA_CONTROL_HEADER_BYTES + length;
        if (connection->assembly_size < whole) return;

        const u8* body = connection->assembly + NYA_CONTROL_HEADER_BYTES;

        /*
         * Detected before anything is parsed, and checked here rather than inside nya_deserialize:
         * that one panics on a format it does not know, which is correct for a caller that picked the
         * format itself and wrong for bytes a stranger chose. The connection's own format is only
         * updated once the answer is one this build can speak, so a reply always goes out in something
         * rather than in the peer's nonsense.
         */
        NYA_SerdeFormat format = nya_serde_detect_format(body, length);

        if (format >= NYA_SERDE_FORMAT_COUNT) {
            // Checked here rather than left to nya_deserialize, which panics on a format it does not
            // know. That is the right answer for a caller that picked the format itself and the wrong
            // one for bytes a stranger chose. The connection keeps whatever format it last spoke, so the
            // refusal below still goes out in something the peer can read.
            _nya_control_send_error(peer, nullptr, "the message is neither json nor the nya format");
        } else {
            connection->format = format;

            NYA_Object* request = nullptr;

            // NYA_SERDE_NO_CHECKSUM: the nya format carries one and a document typed by hand or emitted
            // by a script has no way to produce it. A control message is not a save file; its integrity
            // comes from the socket being owner-only, not from a hash the sender computes itself.
            NYA_Error parsed = nya_deserialize(_NYA_CONTROL->scratch, body, length, format, NYA_SERDE_NO_CHECKSUM, &request);

            if (!parsed.ok || request == nullptr) {
                _nya_control_send_error(peer, nullptr, "could not parse the message: %s", (NYA_ConstCString)parsed.message);
            } else {
                _nya_control_handle(peer, request);
            }
        }

        // The peer may have been dropped by the handler, in which case its assembly buffer is gone.
        if (_nya_control_connection_find(peer) == nullptr) return;

        nya_assert(connection->assembly_size >= whole);
        connection->assembly_size -= whole;
        nya_memmove(connection->assembly, connection->assembly + whole, connection->assembly_size);

        (*budget)--;
    }
}

/*
 * ─────────────────────────────────────────────────────────
 * SENDING
 * ─────────────────────────────────────────────────────────
 */

void _nya_control_send(NYA_IpcPeerId peer, const NYA_Object* message) {
    nya_assert(_NYA_CONTROL != nullptr);
    nya_assert(message != nullptr);

    _NYA_ControlConnection* connection = _nya_control_connection_find(peer);
    if (connection == nullptr) return;

    NYA_String* text = nya_serialize(_NYA_CONTROL->scratch, message, connection->format, NYA_SERDE_NONE);

    if (text == nullptr) {
        _nya_control_connection_reject(peer, "a reply could not be serialized");
        return;
    }

    u8 header[NYA_CONTROL_HEADER_BYTES] = { 0 };

    NYA_Error framed = nya_control_frame_encode(text->length, header);
    if (!framed.ok) {
        _nya_control_connection_reject(peer, "a reply was larger than a message may be");
        return;
    }

    /*
     * Header and body separately, which is safe because the transport queues rather than writes: either
     * both land in the peer's buffer or the first one fails and the peer is dropped, so no reader ever
     * sees a header without its body.
     */
    NYA_Error sent = nya_ipc_listener_send(_NYA_CONTROL->listener, peer, header, sizeof(header));
    if (sent.ok) sent = nya_ipc_listener_send(_NYA_CONTROL->listener, peer, text->items, text->length);

    if (!sent.ok) _nya_control_connection_reject(peer, "is not reading its replies");
}

void _nya_control_send_error(NYA_IpcPeerId peer, const NYA_Value* id, NYA_ConstCString format, ...) {
    nya_assert(_NYA_CONTROL != nullptr);
    nya_assert(format != nullptr);

    char    message[NYA_ERROR_MESSAGE_MAX_LENGTH] = { 0 };
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    NYA_Object* reply = nya_object_create(_NYA_CONTROL->scratch);

    if (id != nullptr) nya_object_set(reply, "id", *id);
    nya_object_set(reply, "ok", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = false });
    nya_object_set(
        reply,
        "error",
        (NYA_Value){ .type      = NYA_TYPE_STRING,
                     .as_string = nya_string_to_cstring(_NYA_CONTROL->scratch, nya_string_from(_NYA_CONTROL->scratch, message)) }
    );

    _nya_control_send(peer, reply);
}

/*
 * ─────────────────────────────────────────────────────────
 * VERBS
 * ─────────────────────────────────────────────────────────
 */

void _nya_control_handle(NYA_IpcPeerId peer, const NYA_Object* request) {
    nya_assert(_NYA_CONTROL != nullptr);
    nya_assert(request != nullptr);

    NYA_Value* id = nya_object_get(request, "id");

    NYA_ConstCString op = _nya_control_field_string(request, "op");
    if (op == nullptr) {
        _nya_control_send_error(peer, id, "a request needs a string 'op'");
        return;
    }

    NYA_Object* reply = nya_object_create(_NYA_CONTROL->scratch);
    if (id != nullptr) nya_object_set(reply, "id", *id);

    b8 ok = true;

    /*
     * Reject by default: the chain ends in an error rather than in a fallthrough, so a verb that is not
     * listed here cannot be reached by spelling it differently.
     */
    if (nya_string_equals(op, "hello")) {
        _nya_control_handle_hello(peer, request, reply);
    } else if (nya_string_equals(op, "object.list")) {
        _nya_control_handle_object_list(reply);
    } else if (nya_string_equals(op, "object.get")) {
        _nya_control_handle_object_get(peer, request, reply, &ok);
    } else if (nya_string_equals(op, "object.set")) {
        _nya_control_handle_object_set(peer, request, &ok);
    } else if (nya_string_equals(op, "event.dispatch")) {
        _nya_control_handle_event_dispatch(peer, request, &ok);
    } else if (nya_string_equals(op, "event.subscribe")) {
        _nya_control_handle_event_subscription(peer, request, true, &ok);
    } else if (nya_string_equals(op, "event.unsubscribe")) {
        _nya_control_handle_event_subscription(peer, request, false, &ok);
    } else {
        _nya_control_send_error(peer, id, "'%s' is not a verb this build knows", op);
        return;
    }

    // A verb that failed has already sent its own error, which says which rule it was.
    if (!ok) return;

    nya_object_set(reply, "ok", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });
    _nya_control_send(peer, reply);
}

void _nya_control_handle_hello(NYA_IpcPeerId peer, const NYA_Object* request, NYA_Object* reply) {
    nya_unused(peer, request);

    NYA_Arena* scratch = _NYA_CONTROL->scratch;

    nya_object_set(reply, "protocol", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = NYA_CONTROL_PROTOCOL_VERSION });
    nya_object_set(reply, "engine", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "nyangine" });
    nya_object_set(reply, "version", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)NYA_VERSION });

    NYA_ArrayᐸNYA_Valueᐳ* permissions = nya_array_create(scratch, NYA_Value);

    if ((_NYA_CONTROL->config.permissions & NYA_CONTROL_PERMISSION_WRITE) != 0) {
        nya_array_push_back(permissions, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "write" }));
    }

    if ((_NYA_CONTROL->config.permissions & NYA_CONTROL_PERMISSION_DISPATCH) != 0) {
        nya_array_push_back(permissions, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "dispatch" }));
    }

    nya_object_set(reply, "permissions", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *permissions });

    NYA_ArrayᐸNYA_Valueᐳ* objects = nya_array_create(scratch, NYA_Value);

    for (u32 i = 0; i < _NYA_CONTROL_EXPOSED_COUNT; i++) {
        nya_array_push_back(objects, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = _NYA_CONTROL_EXPOSED[i].name }));
    }

    nya_object_set(reply, "objects", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *objects });

    NYA_ArrayᐸNYA_Valueᐳ* events = nya_array_create(scratch, NYA_Value);

    for (u32 type = 0; type < NYA_EVENT_COUNT; type++) {
        if (NYA_EVENT_NAME_MAP[type] == nullptr) continue;

        nya_array_push_back(events, ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)NYA_EVENT_NAME_MAP[type] }));
    }

    nya_object_set(reply, "events", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *events });
}

void _nya_control_handle_object_list(NYA_Object* reply) {
    NYA_Arena* scratch = _NYA_CONTROL->scratch;

    NYA_ArrayᐸNYA_Valueᐳ* objects = nya_array_create(scratch, NYA_Value);

    for (u32 i = 0; i < _NYA_CONTROL_EXPOSED_COUNT; i++) {
        NYA_Object* entry = nya_object_create(scratch);

        nya_object_set(entry, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = _NYA_CONTROL_EXPOSED[i].name });
        nya_object_set(entry, "type", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)_NYA_CONTROL_EXPOSED[i].type->name });

        nya_array_push_back(objects, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *entry }));
    }

    nya_object_set(reply, "objects", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *objects });
}

void _nya_control_handle_object_get(NYA_IpcPeerId peer, const NYA_Object* request, NYA_Object* reply, OUT b8* out_ok) {
    *out_ok = false;

    NYA_Value*       id   = nya_object_get(request, "id");
    NYA_ConstCString name = _nya_control_field_string(request, "name");

    if (name == nullptr) {
        _nya_control_send_error(peer, id, "object.get needs a string 'name'");
        return;
    }

    _NYA_ControlExposure* exposure = _nya_control_exposure_find(name);
    if (exposure == nullptr) {
        _nya_control_send_error(peer, id, "'%s' is not exposed", name);
        return;
    }

    NYA_Object* value = nya_reflect_to_object(_NYA_CONTROL->scratch, exposure->type, exposure->instance);
    if (value == nullptr) {
        _nya_control_send_error(peer, id, "'%s' could not be read", name);
        return;
    }

    nya_object_set(reply, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = exposure->name });
    nya_object_set(reply, "value", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *value });

    *out_ok = true;
}

void _nya_control_handle_object_set(NYA_IpcPeerId peer, const NYA_Object* request, OUT b8* out_ok) {
    *out_ok = false;

    NYA_Value* id = nya_object_get(request, "id");

    if ((_NYA_CONTROL->config.permissions & NYA_CONTROL_PERMISSION_WRITE) == 0) {
        _nya_control_send_error(peer, id, "this control surface is read only");
        return;
    }

    NYA_ConstCString name = _nya_control_field_string(request, "name");
    if (name == nullptr) {
        _nya_control_send_error(peer, id, "object.set needs a string 'name'");
        return;
    }

    NYA_Value* value = nya_object_get(request, "value");
    if (value == nullptr || value->type != NYA_TYPE_OBJECT) {
        _nya_control_send_error(peer, id, "object.set needs an object 'value'");
        return;
    }

    _NYA_ControlExposure* exposure = _nya_control_exposure_find(name);
    if (exposure == nullptr) {
        _nya_control_send_error(peer, id, "'%s' is not exposed", name);
        return;
    }

    /*
     * Refused rather than copied. nya_reflect_from_object stores a string field as the pointer it was
     * handed, and that pointer belongs to the scratch arena the next tick reuses, so accepting one would
     * leave the live struct holding memory that is about to be something else. Copying instead would mean
     * an arena that only grows, which a peer could drive.
     */
    NYA_ConstCString offending = nullptr;
    if (_nya_control_writes_a_string(exposure->type, &value->as_object, 0, &offending)) {
        _nya_control_send_error(peer, id, "'%s.%s' is a string, and strings cannot be written over the control socket", name, offending);
        return;
    }

    NYA_Error written = nya_reflect_from_object(exposure->type, exposure->instance, &value->as_object);
    if (!written.ok) {
        _nya_control_send_error(peer, id, "'%s' could not be written: %s", name, (NYA_ConstCString)written.message);
        return;
    }

    *out_ok = true;
}

void _nya_control_handle_event_dispatch(NYA_IpcPeerId peer, const NYA_Object* request, OUT b8* out_ok) {
    *out_ok = false;

    NYA_Value* id = nya_object_get(request, "id");

    if ((_NYA_CONTROL->config.permissions & NYA_CONTROL_PERMISSION_DISPATCH) == 0) {
        _nya_control_send_error(peer, id, "this control surface does not accept dispatched events");
        return;
    }

    NYA_ConstCString type_name = _nya_control_field_string(request, "type");
    if (type_name == nullptr) {
        _nya_control_send_error(peer, id, "event.dispatch needs a string 'type'");
        return;
    }

    NYA_EventType type = _nya_control_event_type_from_name(type_name);

    if (type == NYA_EVENT_COUNT || type == NYA_EVENT_INVALID) {
        _nya_control_send_error(peer, id, "'%s' is not an event type", type_name);
        return;
    }

    // The lifecycle markers bound a range rather than naming an event, and dispatching one would put a
    // value in the queue that every consumer's switch treats as impossible.
    if (type == NYA_EVENT_LIFECYCLE_EVENTS_BEGIN || type == NYA_EVENT_LIFECYCLE_EVENTS_END) {
        _nya_control_send_error(peer, id, "'%s' marks a range and is not dispatchable", type_name);
        return;
    }

    NYA_Event event = { .type = type };

    if (type == NYA_EVENT_CONTROL_MESSAGE) {
        NYA_ConstCString message_name = _nya_control_field_string(request, "name");

        if (message_name == nullptr || message_name[0] == '\0') {
            _nya_control_send_error(peer, id, "a CONTROL_MESSAGE needs a non-empty string 'name'");
            return;
        }

        NYA_Value* body = nya_object_get(request, "body");

        event.as_control_message_event = (NYA_ControlMessageEvent){
            .name   = message_name,
            .body   = (body != nullptr && body->type == NYA_TYPE_OBJECT) ? &body->as_object : nullptr,
            .sender = peer,
        };
    } else if (_NYA_CONTROL_EVENT_PAYLOAD[type] != nullptr) {
        NYA_Value* payload = nya_object_get(request, "payload");

        if (payload != nullptr && payload->type == NYA_TYPE_OBJECT) {
            // Every union member starts at the same address, so which one this is does not change where
            // it is written; the reflection the program registered says how far it reaches.
            NYA_Error written = nya_reflect_from_object(_NYA_CONTROL_EVENT_PAYLOAD[type], &event.as_asset_event, &payload->as_object);

            if (!written.ok) {
                _nya_control_send_error(peer, id, "the payload for '%s' could not be written: %s", type_name, (NYA_ConstCString)written.message);
                return;
            }
        }
    }

    nya_event_dispatch(event);

    *out_ok = true;
}

void _nya_control_handle_event_subscription(NYA_IpcPeerId peer, const NYA_Object* request, b8 subscribe, OUT b8* out_ok) {
    *out_ok = false;

    NYA_Value* id = nya_object_get(request, "id");

    _NYA_ControlConnection* connection = _nya_control_connection_find(peer);
    if (connection == nullptr) return;

    NYA_Value* types = nya_object_get(request, "types");
    if (types == nullptr || types->type != NYA_TYPE_ARRAY) {
        _nya_control_send_error(peer, id, "a subscription needs an array 'types'");
        return;
    }

    // Bounded by the number of event types rather than by what the peer sent: a list with ten thousand
    // entries costs this loop exactly as much as a list with all of them.
    if (types->as_array.length > NYA_EVENT_COUNT) {
        _nya_control_send_error(peer, id, "a subscription may name at most %u types", (u32)NYA_EVENT_COUNT);
        return;
    }

    nya_array_foreach (&types->as_array, entry) {
        if (entry->type != NYA_TYPE_STRING || entry->as_string == nullptr) {
            _nya_control_send_error(peer, id, "a subscription names event types as strings");
            return;
        }

        NYA_EventType type = _nya_control_event_type_from_name(entry->as_string);

        if (type == NYA_EVENT_COUNT || type == NYA_EVENT_INVALID) {
            _nya_control_send_error(peer, id, "'%s' is not an event type", entry->as_string);
            return;
        }

        _nya_control_subscription_set(connection, type, subscribe);
    }

    *out_ok = true;
}

/*
 * ─────────────────────────────────────────────────────────
 * SUBSCRIPTIONS
 * ─────────────────────────────────────────────────────────
 */

void _nya_control_subscription_set(_NYA_ControlConnection* connection, NYA_EventType type, b8 wanted) {
    nya_assert(_NYA_CONTROL != nullptr);
    nya_assert(connection != nullptr);
    nya_assert(type < NYA_EVENT_COUNT);

    u32 byte = (u32)type / 8;
    u8  bit  = (u8)(1U << ((u32)type % 8));

    nya_assert(byte < _NYA_CONTROL_SUBSCRIPTION_BYTES);

    b8 held = (connection->subscriptions[byte] & bit) != 0;
    if (held == wanted) return;

    if (wanted) {
        connection->subscriptions[byte] |= bit;

        // Registered on the first subscriber and never again, so the engine walks one hook per type no
        // matter how many connections want it.
        if (_NYA_CONTROL->subscriber_count[type]++ == 0) {
            nya_event_hook_register((NYA_EventHook){
                .event_type = type,
                .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
                .fn         = _NYA_CONTROL->event_hook,
            });
        }

        return;
    }

    connection->subscriptions[byte] &= (u8)~bit;

    nya_assert(_NYA_CONTROL->subscriber_count[type] > 0);

    if (--_NYA_CONTROL->subscriber_count[type] == 0) {
        nya_event_hook_unregister((NYA_EventHook){
            .event_type = type,
            .hook_type  = NYA_EVENT_HOOK_TYPE_IMMEDIATE,
            .fn         = _NYA_CONTROL->event_hook,
        });
    }
}

void _nya_control_on_engine_event(NYA_Event* event) {
    if (_NYA_CONTROL == nullptr || !_NYA_CONTROL->running) return;

    nya_assert(event != nullptr);
    if (event->type >= NYA_EVENT_COUNT) return;

    u32 byte = (u32)event->type / 8;
    u8  bit  = (u8)(1U << ((u32)event->type % 8));

    NYA_Arena* scratch = _NYA_CONTROL->scratch;

    for (u32 i = 0; i < NYA_IPC_MAX_CONNECTIONS; i++) {
        _NYA_ControlConnection* connection = &_NYA_CONTROL->connections[i];

        if (!connection->occupied) continue;
        if ((connection->subscriptions[byte] & bit) == 0) continue;

        NYA_Object* push = nya_object_create(scratch);

        nya_object_set(push, "op", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString) "event" });
        nya_object_set(push, "type", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)NYA_EVENT_NAME_MAP[event->type] });
        nya_object_set(push, "timestamp", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = event->timestamp });

        if (event->type == NYA_EVENT_CONTROL_MESSAGE && event->as_control_message_event.name != nullptr) {
            nya_object_set(push, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (NYA_CString)event->as_control_message_event.name });
        } else if (_NYA_CONTROL_EVENT_PAYLOAD[event->type] != nullptr) {
            NYA_Object* payload = nya_reflect_to_object(scratch, _NYA_CONTROL_EVENT_PAYLOAD[event->type], &event->as_asset_event);

            if (payload != nullptr) nya_object_set(push, "payload", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *payload });
        }

        _nya_control_send(connection->peer, push);
    }
}

void _nya_control_on_frame(NYA_Event* event) {
    nya_unused(event);

    nya_system_control_tick();
}
