#include "nyangine-core/nyangine.h"

#if defined(NYA_TESTING) && NYA_OS_FAULT_ENABLED

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

enum {
    /** Bytes of a UDP test message past its header and checksum. Past one datagram, so fragments are faulted too. */
    _NYA_FAULTS_UDP_PAYLOAD_MAX = 3000,

    /** Reliable messages in flight at once. The send action waits for the network once this many are unacknowledged. */
    _NYA_FAULTS_UDP_PENDING_MAX = 128,

    /** Pumps one ordinary UDP action takes, and how many a convergence may take before it is a failure. */
    _NYA_FAULTS_UDP_PUMPS          = 4,
    _NYA_FAULTS_UDP_CONVERGE_PUMPS = 3000,

    /** Highest fault rate a window draws, in parts per million: a third of the datagrams, past which nothing converges. */
    _NYA_FAULTS_PPM_MAX = 300'000,

    /** Bytes the answer route may be asked for. Half the response buffer, so the head always fits beside it. */
    _NYA_FAULTS_HTTP_BODY_MAX = NYA_HTTP_MAX_RESPONSE_BYTES / 2,

    /** Room a client keeps for a whole answer: the largest body and a generous head. */
    _NYA_FAULTS_HTTP_RESPONSE_MAX = _NYA_FAULTS_HTTP_BODY_MAX + 4096,

    /** Passes one exchange may take, each waiting up to _NYA_FAULTS_HTTP_WAIT_MS, before the server is said to hang. */
    _NYA_FAULTS_HTTP_PASSES  = 600,
    _NYA_FAULTS_HTTP_WAIT_MS = 5,

    /** Ticks a closed connection's slot has to be given back in. */
    _NYA_FAULTS_HTTP_RELEASE_TICKS = 32,

    /** Slowloris peers held at once: below NYA_HTTP_MAX_CONNECTIONS_PER_ADDRESS, so an honest request still has a slot. */
    _NYA_FAULTS_SLOW_MAX = 2,

    /** Bytes past the head bound a dribbled header may reach before the server holding it is a finding: two lines of slack. */
    _NYA_FAULTS_SLOW_SLACK = 256,

    /** Journal lines one run appends before it starts the file over, so it stays small enough to read back every step. */
    _NYA_FAULTS_JOURNAL_MAX = 256,
};

#define _NYA_FAULTS_HTTP_PATH    "/simulation/faults"
#define _NYA_FAULTS_PREFIX       "fault_"
#define _NYA_FAULTS_SAVE_FILE    _NYA_FAULTS_PREFIX "save.nya"
#define _NYA_FAULTS_JOURNAL_FILE _NYA_FAULTS_PREFIX "journal.log"

/** Salt the plan's seed is the run's seed under, so the plan's draws never coincide with the run's own. The ASCII of "osfaults". */
#define _NYA_FAULTS_SEED_SALT 0x6F736661756C7473ULL

/** What every monotonic read moves the simulated clock by, so a loop polling a deadline on it ends. */
#define _NYA_FAULTS_READ_STEP_NS 1'000ULL

/** Simulated time one UDP pump takes. */
#define _NYA_FAULTS_UDP_PUMP_NS (10ULL * 1'000'000ULL)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct {
    b8 initialized;

    NYA_OsFaultPlan plan;

    /** Paths and the journal's record, for the life of the set. */
    NYA_Arena* arena;

    /** Per action, emptied at the top of each. */
    NYA_Arena* scratch;

    // udp

    /** The transports of one session. Destroyed with it. */
    NYA_Arena*        udp_arena;
    NYA_NetTransport* udp_server;
    NYA_NetTransport* udp_client;
    NYA_NetPeerId     udp_server_peer;
    NYA_NetPeerId     udp_client_peer;
    b8                udp_server_connected;
    b8                udp_client_connected;

    /** Reliable messages the client sent and the server took, in this session. The oracle is that these meet once healed. */
    u32 udp_sent;
    u32 udp_received;

    /** The window's datagram rules, armed only while a UDP action runs. */
    NYA_OsFaultRule udp_send_to;
    NYA_OsFaultRule udp_receive_from;

    // http

    b8  http_available;
    u16 http_port;

    NYA_OsSocket slow[_NYA_FAULTS_SLOW_MAX];
    u64          slow_bytes[_NYA_FAULTS_SLOW_MAX];
    u32          slow_count;

    u8 response[_NYA_FAULTS_HTTP_RESPONSE_MAX];

    // disk

    b8          disk_available;
    NYA_CString prefix;
    NYA_CString journal_path;
    NYA_CString root;

    /** The window's file rules, armed only while a disk action runs. */
    NYA_OsFaultRule disk_rules[NYA_OS_FAULT_FILE_RENAME + 1];

    /** The last save a write acknowledged, and whether there is one. */
    b8  save_exists;
    u64 save_value;

    /** Which journal entries were acknowledged, in the order they were appended, and how many were tried. */
    u64 journal_acknowledged[_NYA_FAULTS_JOURNAL_MAX];
    u32 journal_acknowledged_count;
    u64 journal_attempts;
} _NYA_SimulationFaults;

NYA_INTERNAL _NYA_SimulationFaults _NYA_FAULTS = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * HELPERS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A rule drawn for a window: a rate up to _NYA_FAULTS_PPM_MAX over a nonempty subset of `kinds`. */
NYA_INTERNAL NYA_OsFaultRule _nya_faults_rule_draw(NYA_SimulationRun* run, u32 kinds) {
    // a draw that picks no kind falls back to all of them, so the loop is bounded rather than retried.
    u32 subset = kinds & (u32)nya_simulation_roll(run);
    if (subset == 0) subset = kinds;

    return (NYA_OsFaultRule){
        .probability_ppm = 1 + (u32)nya_simulation_below(run, _NYA_FAULTS_PPM_MAX),
        .kinds           = subset,
    };
}

/** What the answer route serves at offset `index`: printable, and different at every offset a short send could split. */
NYA_INTERNAL u8 _nya_faults_body_byte(u64 index) {
    return (u8)('a' + (index * 7 + index / 26) % 26);
}

/*
 * ─────────────────────────────────────────────────────────
 * UDP
 * ─────────────────────────────────────────────────────────
 */

/** Puts the UDP pair on simulated time and its window's rules on the wire. Paired with _nya_faults_udp_leave. */
NYA_INTERNAL void _nya_faults_udp_enter(void) {
    _NYA_FAULTS.plan.time_simulated                            = true;
    _NYA_FAULTS.plan.rules[NYA_OS_FAULT_SOCKET_SEND_TO]      = _NYA_FAULTS.udp_send_to;
    _NYA_FAULTS.plan.rules[NYA_OS_FAULT_SOCKET_RECEIVE_FROM] = _NYA_FAULTS.udp_receive_from;
}

NYA_INTERNAL void _nya_faults_udp_leave(void) {
    _NYA_FAULTS.plan.rules[NYA_OS_FAULT_SOCKET_SEND_TO]      = (NYA_OsFaultRule){ 0 };
    _NYA_FAULTS.plan.rules[NYA_OS_FAULT_SOCKET_RECEIVE_FROM] = (NYA_OsFaultRule){ 0 };
    _NYA_FAULTS.plan.time_simulated                            = false;
}

NYA_INTERNAL void _nya_faults_udp_close(void) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;
    if (faults->udp_arena == nullptr) return;

    if (faults->udp_client != nullptr) nya_net_transport_destroy(faults->udp_client);
    if (faults->udp_server != nullptr) nya_net_transport_destroy(faults->udp_server);
    nya_arena_destroy(faults->udp_arena);

    faults->udp_arena            = nullptr;
    faults->udp_client           = nullptr;
    faults->udp_server           = nullptr;
    faults->udp_server_connected = false;
    faults->udp_client_connected = false;
}

/** A fresh pair, listening and dialling. On an error nothing is left open, and the pair is down until the next restart. */
NYA_INTERNAL NYA_Error _nya_faults_udp_open(void) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;
    nya_assert(faults->udp_arena == nullptr, "a UDP pair is already open");

    faults->udp_arena    = nya_arena_create(.name = "simulation_faults_udp");
    faults->udp_sent     = 0;
    faults->udp_received = 0;

    NYA_Error opened = nya_net_transport_udp_create(faults->udp_arena, (NYA_NetUdpOptions){ 0 }, &faults->udp_server);
    if (opened.ok) opened = nya_net_transport_udp_create(faults->udp_arena, (NYA_NetUdpOptions){ 0 }, &faults->udp_client);
    if (opened.ok) opened = nya_net_transport_listen(faults->udp_server, 0);
    if (opened.ok) opened = nya_net_transport_connect(faults->udp_client, "127.0.0.1", nya_net_transport_port(faults->udp_server));

    if (!opened.ok) _nya_faults_udp_close();

    return opened;
}

/** One message the server took: checked whole, then in order. */
NYA_INTERNAL void _nya_faults_udp_receive(NYA_SimulationRun* run, const u8* data, u64 size) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;

    u32 sequence = 0;
    u32 length   = 0;
    u64 checksum = 0;

    if (size >= 16) {
        nya_memcpy(&sequence, data, sizeof(sequence));
        nya_memcpy(&length, data + 4, sizeof(length));
        nya_memcpy(&checksum, data + size - 8, sizeof(checksum));
    }

    // the transport's AEAD is the only thing between a flipped datagram and this check.
    if (size < 16 || length != size || checksum != nya_hash_wyhash(data, size - 8)) {
        nya_simulation_fail(run, "the UDP transport handed up a corrupted %llu byte message", (unsigned long long)size);
        return;
    }

    if (sequence != faults->udp_received) {
        nya_simulation_fail(run, "the UDP transport delivered reliable message %u where %u was next", sequence, faults->udp_received);
        return;
    }

    faults->udp_received++;
}

/** Drains both ends once and moves the simulated clock on by one pump. */
NYA_INTERNAL void _nya_faults_udp_pump(NYA_SimulationRun* run) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;
    NYA_NetTransportEvent  event  = { 0 };

    if (faults->udp_arena == nullptr) return;

    while (nya_net_transport_poll(faults->udp_server, &event)) {
        switch (event.kind) {
            case NYA_NET_TRANSPORT_EVENT_CONNECTED: {
                faults->udp_server_peer      = event.peer;
                faults->udp_server_connected = true;
            } break;

            case NYA_NET_TRANSPORT_EVENT_DISCONNECTED: faults->udp_server_connected = false; break;
            case NYA_NET_TRANSPORT_EVENT_MESSAGE:      _nya_faults_udp_receive(run, event.data, event.size); break;
            default:                                   break;
        }
    }

    while (nya_net_transport_poll(faults->udp_client, &event)) {
        if (event.kind == NYA_NET_TRANSPORT_EVENT_CONNECTED) {
            faults->udp_client_peer      = event.peer;
            faults->udp_client_connected = true;
        }

        if (event.kind == NYA_NET_TRANSPORT_EVENT_DISCONNECTED) faults->udp_client_connected = false;
    }

    nya_os_fault_time_advance(&faults->plan, _NYA_FAULTS_UDP_PUMP_NS);
}

NYA_INTERNAL b8 _nya_faults_udp_connected(void) {
    return _NYA_FAULTS.udp_server_connected && _NYA_FAULTS.udp_client_connected;
}

/** A fresh pair on a healthy random source. Failing then is the host's trouble, and the run cannot go on without one. */
NYA_INTERNAL void _nya_faults_udp_restart(void) {
    _nya_faults_udp_close();
    NYA_EXPECT(_nya_faults_udp_open(), "while opening the simulated UDP pair");
}

/*
 * ─────────────────────────────────────────────────────────
 * HTTP
 * ─────────────────────────────────────────────────────────
 */

/** Answers `size` bytes of the body pattern, through the fallible path, so a starved exchange is refused here too. */
NYA_INTERNAL NYA_HttpStatus _nya_faults_http_answer(NYA_HttpExchange* exchange) {
    char text[24] = { 0 };
    if (!nya_http_request_query_param(exchange->request, "size", text, sizeof(text))) {
        return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, "no size to answer with");
    }

    u64 size = strtoull(text, nullptr, 10);
    if (size == 0 || size > _NYA_FAULTS_HTTP_BODY_MAX) return nya_http_response_problem(exchange, NYA_HTTP_STATUS_BAD_REQUEST, "a size out of range");

    u8* body = nya_arena_try_alloc(exchange->arena, size);
    if (body == nullptr) return nya_http_response_problem(exchange, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, "no memory for the answer");

    for (u64 i = 0; i < size; i++) body[i] = _nya_faults_body_byte(i);

    NYA_Error written = nya_http_response_bytes(exchange->response, body, size, NYA_HTTP_MEDIA_TEXT);
    if (!written.ok) return nya_http_response_error(exchange, written);

    return NYA_HTTP_STATUS_OK;
}

NYA_INTERNAL const NYA_HttpRoute _NYA_FAULTS_HTTP_ROUTES[] = {
    {
     .method   = NYA_HTTP_METHOD_GET,
     .path     = _NYA_FAULTS_HTTP_PATH,
     .auth     = NYA_HTTP_AUTH_NONE,
     .handler  = _nya_faults_http_answer,
     .summary  = "Answers `size` bytes of a fixed pattern",
     .statuses = { NYA_HTTP_STATUS_OK, NYA_HTTP_STATUS_BAD_REQUEST, NYA_HTTP_STATUS_SERVICE_UNAVAILABLE, NYA_HTTP_STATUS_INTERNAL_ERROR },
     },
};

NYA_INTERNAL const NYA_HttpRouter _NYA_FAULTS_HTTP_ROUTER = {
    .name        = "simulation_faults",
    .routes      = _NYA_FAULTS_HTTP_ROUTES,
    .route_count = nya_carray_length(_NYA_FAULTS_HTTP_ROUTES),
};

/** A stream socket to the server, connected or under way. NYA_OS_SOCKET_NONE when the host refused outright. */
NYA_INTERNAL NYA_OsSocket _nya_faults_http_dial(void) {
    NYA_OsAddress address = { 0 };
    nya_assert(nya_os_address_resolve("127.0.0.1", _NYA_FAULTS.http_port, NYA_OS_ADDRESS_V4, &address) == NYA_OS_SOCKET_OK);

    NYA_OsSocket       socket    = NYA_OS_SOCKET_NONE;
    NYA_OsSocketStatus connected = nya_os_socket_connect(address, &socket);
    if (connected != NYA_OS_SOCKET_OK && connected != NYA_OS_SOCKET_WOULD_BLOCK) return NYA_OS_SOCKET_NONE;

    NYA_OsSocketWait writable = { .socket = socket, .writable = true };
    u32              ready    = 0;
    (void)nya_os_socket_wait(&writable, 1, 1000, &ready);

    return socket;
}

/** What one exchange came back with. `complete` is a status line, a head and every byte its Content-Length promised. */
typedef struct {
    b8  closed;
    b8  complete;
    u32 status;
    b8  body_intact;
    b8  retry_after;
} _NYA_FaultsHttpOutcome;

/** Reads what `filled` bytes of an answer say. */
NYA_INTERNAL _NYA_FaultsHttpOutcome _nya_faults_http_parse(const u8* bytes, u64 filled, u64 body_expected) {
    _NYA_FaultsHttpOutcome outcome = { 0 };

    const char* text  = (const char*)bytes;
    const char* blank = filled >= 4 ? strstr(text, "\r\n\r\n") : nullptr;
    if (blank == nullptr || filled < 12 || strncmp(text, "HTTP/1.1 ", 9) != 0) return outcome;

    outcome.status      = (u32)strtoul(text + 9, nullptr, 10);
    outcome.retry_after = strstr(text, "Retry-After: ") != nullptr && strstr(text, "Retry-After: ") < blank;

    const char* length = strstr(text, "Content-Length: ");
    if (length == nullptr || length > blank) return outcome;

    u64 head = (u64)(blank + 4 - text);
    u64 body = strtoull(length + 16, nullptr, 10);
    if (filled < head + body) return outcome;

    outcome.complete = true;

    if (outcome.status == NYA_HTTP_STATUS_OK) {
        outcome.body_intact = body == body_expected;
        for (u64 i = 0; i < body && outcome.body_intact; i++) outcome.body_intact = bytes[head + i] == _nya_faults_body_byte(i);
    }

    return outcome;
}

/** Sends a GET for `body_size` bytes over a fresh connection and reads until the server closes, ticking it throughout. */
NYA_INTERNAL _NYA_FaultsHttpOutcome _nya_faults_http_exchange(u64 body_size) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;

    char request[160] = { 0 };
    s32  length       = snprintf(request, sizeof(request), "GET " _NYA_FAULTS_HTTP_PATH "?size=%llu HTTP/1.1\r\nHost: simulation\r\nConnection: close\r\n\r\n",
                                 (unsigned long long)body_size);
    nya_assert(length > 0 && (u64)length < sizeof(request));

    NYA_OsSocket socket = _nya_faults_http_dial();
    defer nya_os_socket_close(socket);

    if (socket.handle == 0) return (_NYA_FaultsHttpOutcome){ .closed = true };

    u64 sent = 0;
    for (u32 pass = 0; pass < _NYA_FAULTS_HTTP_PASSES && sent < (u64)length; pass++) {
        nya_system_http_tick();

        u64                took   = 0;
        NYA_OsSocketStatus status = nya_os_socket_send(socket, (const u8*)request + sent, (u64)length - sent, &took);

        if (status == NYA_OS_SOCKET_OK) {
            sent += took;
        } else if (status == NYA_OS_SOCKET_WOULD_BLOCK) {
            NYA_OsSocketWait writable = { .socket = socket, .writable = true };
            u32              ready    = 0;
            (void)nya_os_socket_wait(&writable, 1, _NYA_FAULTS_HTTP_WAIT_MS, &ready);
        } else {
            break;
        }
    }

    nya_os_socket_close_send(socket);

    u64 filled = 0;
    b8  closed = false;

    for (u32 pass = 0; pass < _NYA_FAULTS_HTTP_PASSES && !closed && filled + 1 < sizeof(faults->response); pass++) {
        nya_system_http_tick();

        u64                read   = 0;
        NYA_OsSocketStatus status = nya_os_socket_receive(socket, faults->response + filled, sizeof(faults->response) - filled - 1, &read);

        if (status == NYA_OS_SOCKET_OK) {
            filled += read;
        } else if (status == NYA_OS_SOCKET_WOULD_BLOCK) {
            NYA_OsSocketWait readable = { .socket = socket, .readable = true };
            u32              ready    = 0;
            (void)nya_os_socket_wait(&readable, 1, _NYA_FAULTS_HTTP_WAIT_MS, &ready);
        } else {
            closed = true;
        }
    }

    faults->response[filled] = '\0';

    _NYA_FaultsHttpOutcome outcome = _nya_faults_http_parse(faults->response, filled, body_size);
    outcome.closed                 = closed;

    return outcome;
}

/** Ticks the server until it holds no connection, or says it never let go. */
NYA_INTERNAL void _nya_faults_http_released(NYA_SimulationRun* run, u32 held_on_purpose) {
    for (u32 tick = 0; tick < _NYA_FAULTS_HTTP_RELEASE_TICKS && nya_http_server_connection_count() > held_on_purpose; tick++) nya_system_http_tick();

    if (nya_http_server_connection_count() > held_on_purpose) {
        nya_simulation_fail(run, "the HTTP server holds %u connections after its peers closed, where %u are open", nya_http_server_connection_count(), held_on_purpose);
    }
}

NYA_INTERNAL void _nya_faults_slow_forget(u32 index) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;
    nya_assert(index < faults->slow_count);

    nya_os_socket_close(faults->slow[index]);

    faults->slow_count--;
    faults->slow[index]       = faults->slow[faults->slow_count];
    faults->slow_bytes[index] = faults->slow_bytes[faults->slow_count];
}

/*
 * ─────────────────────────────────────────────────────────
 * DISK
 * ─────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_faults_disk_enter(void) {
    for (u32 op = 0; op <= NYA_OS_FAULT_FILE_RENAME; op++) _NYA_FAULTS.plan.rules[op] = _NYA_FAULTS.disk_rules[op];
}

NYA_INTERNAL void _nya_faults_disk_leave(void) {
    for (u32 op = 0; op <= NYA_OS_FAULT_FILE_RENAME; op++) _NYA_FAULTS.plan.rules[op] = (NYA_OsFaultRule){ 0 };
}

NYA_INTERNAL b8 _nya_faults_disk_faulty(void) {
    for (u32 op = 0; op <= NYA_OS_FAULT_FILE_RENAME; op++) {
        if (_NYA_FAULTS.disk_rules[op].probability_ppm > 0) return true;
    }

    return false;
}

/** The value the save holds, read without faults. False when there is no save, or it does not read as one. */
NYA_INTERNAL b8 _nya_faults_save_value(OUT u64* out_value, OUT b8* out_exists) {
    NYA_Object* object = nullptr;
    NYA_Error   read   = nya_save_read(_NYA_FAULTS.scratch, _NYA_FAULTS_SAVE_FILE, NYA_SERDE_NONE, &object);

    *out_exists = read.kind != NYA_ERROR_NOT_FOUND;
    if (!read.ok) return false;

    NYA_Value* value = nya_object_get(object, "value");
    if (value == nullptr) return false;

    if (value->type == NYA_TYPE_U64) *out_value = value->as_u64;
    else if (value->type == NYA_TYPE_S64) *out_value = (u64)value->as_s64;
    else return false;

    return true;
}

/** A failed replace must take its temporary with it; one left beside the save is a leak a full disk fills with. */
NYA_INTERNAL void _nya_faults_save_no_temporaries(NYA_SimulationRun* run) {
    NYA_ArrayᐸNYA_DirectoryEntryᐳ* entries = nullptr;
    if (!nya_filesystem_list(_NYA_FAULTS.scratch, _NYA_FAULTS.root, &entries).ok) return;

    nya_array_foreach (entries, entry) {
        if (nya_string_starts_with(entry->name, _NYA_FAULTS_SAVE_FILE ".") && nya_string_ends_with(entry->name, ".tmp")) {
            nya_simulation_fail(run, "a failed save left its temporary %s behind", nya_string_to_cstring(_NYA_FAULTS.scratch, entry->name));
            (void)nya_filesystem_delete(nya_string_to_cstring(_NYA_FAULTS.scratch, nya_path_join(_NYA_FAULTS.scratch, _NYA_FAULTS.root, nya_string_to_cstring(_NYA_FAULTS.scratch, entry->name))));
        }
    }
}

/** The journal's line for entry `entry`: the number and a hash of it, so a torn or merged line cannot parse. */
NYA_INTERNAL s32 _nya_faults_journal_line(u64 entry, OUT char* out, u64 capacity) {
    return snprintf(out, capacity, "entry %llu %016llx\n", (unsigned long long)entry, (unsigned long long)nya_hash_wyhash(&entry, sizeof(entry)));
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ACTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_faults_do_udp_send(NYA_SimulationRun* run) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;

    _nya_faults_udp_enter();
    defer _nya_faults_udp_leave();

    if (_nya_faults_udp_connected() && faults->udp_sent - faults->udp_received < _NYA_FAULTS_UDP_PENDING_MAX) {
        u8  message[16 + _NYA_FAULTS_UDP_PAYLOAD_MAX];
        u32 size = 16 + (u32)nya_simulation_below(run, _NYA_FAULTS_UDP_PAYLOAD_MAX);

        for (u32 i = 8; i < size - 8; i++) message[i] = (u8)nya_simulation_roll(run);
        nya_memcpy(message, &faults->udp_sent, sizeof(u32));
        nya_memcpy(message + 4, &size, sizeof(u32));

        u64 checksum = nya_hash_wyhash(message, size - 8);
        nya_memcpy(message + size - 8, &checksum, sizeof(checksum));

        // an error is a peer the transport has given up on, which the pumps below hear about as a disconnect.
        if (nya_net_transport_send(faults->udp_client, faults->udp_client_peer, NYA_NET_CHANNEL_RELIABLE, message, size).ok) faults->udp_sent++;
    }

    for (u32 i = 0; i < _NYA_FAULTS_UDP_PUMPS; i++) _nya_faults_udp_pump(run);
}

NYA_INTERNAL void _nya_faults_do_udp_pump(NYA_SimulationRun* run) {
    _nya_faults_udp_enter();
    defer _nya_faults_udp_leave();

    for (u32 i = 0; i < _NYA_FAULTS_UDP_PUMPS; i++) _nya_faults_udp_pump(run);
}

NYA_INTERNAL void _nya_faults_do_udp_converge(NYA_SimulationRun* run) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;

    // healed first: what converges has to converge on a clean wire, with what the delay held arriving late.
    faults->udp_send_to      = (NYA_OsFaultRule){ 0 };
    faults->udp_receive_from = (NYA_OsFaultRule){ 0 };

    _nya_faults_udp_enter();
    defer _nya_faults_udp_leave();

    nya_os_fault_datagrams_release(&faults->plan);

    u32 pumps = 0;
    while (pumps < _NYA_FAULTS_UDP_CONVERGE_PUMPS && !(_nya_faults_udp_connected() && faults->udp_received == faults->udp_sent)) {
        _nya_faults_udp_pump(run);
        pumps++;

        // a session the faults timed out is a reason to start another, not a failure: the transport told both ends.
        if (!faults->udp_client_connected && pumps > _NYA_FAULTS_UDP_CONVERGE_PUMPS / 2) break;
    }

    nya_simulation_trace(run, faults->udp_received);

    if (_nya_faults_udp_connected() && faults->udp_received != faults->udp_sent) {
        nya_simulation_fail(run, "the UDP transport delivered %u of %u reliable messages once the network healed", faults->udp_received, faults->udp_sent);
    }

    if (!_nya_faults_udp_connected()) _nya_faults_udp_restart();
}

NYA_INTERNAL void _nya_faults_do_http(NYA_SimulationRun* run) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;

    b8  resets = nya_simulation_chance(run, 25);
    u32 stream = (1U << NYA_OS_FAULT_SHORT) | (1U << NYA_OS_FAULT_WOULD_BLOCK) | (resets ? 1U << NYA_OS_FAULT_RESET : 0U);
    u64 body   = 1 + nya_simulation_below(run, _NYA_FAULTS_HTTP_BODY_MAX);

    faults->plan.rules[NYA_OS_FAULT_SOCKET_SEND]    = _nya_faults_rule_draw(run, stream);
    faults->plan.rules[NYA_OS_FAULT_SOCKET_RECEIVE] = _nya_faults_rule_draw(run, stream);
    faults->plan.rules[NYA_OS_FAULT_SOCKET_ACCEPT]  = _nya_faults_rule_draw(run, (1U << NYA_OS_FAULT_FAIL) | (1U << NYA_OS_FAULT_WOULD_BLOCK));

    u64 injected_before = faults->plan.injected[NYA_OS_FAULT_SOCKET_SEND] + faults->plan.injected[NYA_OS_FAULT_SOCKET_RECEIVE];

    _NYA_FaultsHttpOutcome outcome = _nya_faults_http_exchange(body);

    b8 faulted = faults->plan.injected[NYA_OS_FAULT_SOCKET_SEND] + faults->plan.injected[NYA_OS_FAULT_SOCKET_RECEIVE] > injected_before;

    faults->plan.rules[NYA_OS_FAULT_SOCKET_SEND]    = (NYA_OsFaultRule){ 0 };
    faults->plan.rules[NYA_OS_FAULT_SOCKET_RECEIVE] = (NYA_OsFaultRule){ 0 };
    faults->plan.rules[NYA_OS_FAULT_SOCKET_ACCEPT]  = (NYA_OsFaultRule){ 0 };

    if (!outcome.closed) nya_simulation_fail(run, "the HTTP server neither finished nor closed an exchange under socket faults");

    if (outcome.complete && outcome.status >= 500) nya_simulation_fail(run, "the HTTP server answered %u under socket faults, which are the network's and not its own", outcome.status);
    if (outcome.complete && outcome.status == NYA_HTTP_STATUS_OK && !outcome.body_intact) {
        nya_simulation_fail(run, "the HTTP server's %llu byte answer arrived whole in length and wrong in content", (unsigned long long)body);
    }

    // without a reset nothing is lost: a short or refused call is the server's to retry, and the whole answer has to arrive.
    b8 reset_possible = resets && faulted;
    if (!reset_possible && !(outcome.complete && outcome.status == NYA_HTTP_STATUS_OK)) {
        nya_simulation_fail(run, "the HTTP server lost a %llu byte answer to short calls and EAGAIN alone (status %u, %s)", (unsigned long long)body, outcome.status,
                            outcome.complete ? "complete" : "incomplete");
    }

    nya_simulation_trace(run, reset_possible ? 0 : outcome.status);

    _nya_faults_http_released(run, 0);
}

NYA_INTERNAL void _nya_faults_do_http_starved(NYA_SimulationRun* run) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;

    // the next fallible allocation on this thread is the exchange's reserve, which runs before any handler.
    faults->plan.rules[NYA_OS_FAULT_ALLOCATION] = (NYA_OsFaultRule){ .fail_call = faults->plan.calls[NYA_OS_FAULT_ALLOCATION] + 1, .fail_kind = NYA_OS_FAULT_FAIL };

    u64                    refusals = nya_http_server_memory_refusal_count();
    _NYA_FaultsHttpOutcome outcome  = _nya_faults_http_exchange(1 + nya_simulation_below(run, 64));

    faults->plan.rules[NYA_OS_FAULT_ALLOCATION] = (NYA_OsFaultRule){ 0 };

    if (!outcome.complete || outcome.status != NYA_HTTP_STATUS_SERVICE_UNAVAILABLE || !outcome.retry_after) {
        nya_simulation_fail(run, "a starved exchange was answered %u%s rather than 503 with Retry-After", outcome.status, outcome.complete ? "" : " (incomplete)");
    }

    if (nya_http_server_memory_refusal_count() != refusals + 1) nya_simulation_fail(run, "a starved exchange was not counted as a memory refusal");

    nya_simulation_trace(run, outcome.status);

    _nya_faults_http_released(run, 0);
}

/**
 * Drips one header line onto slow peer `index` and listens. False once the peer is done with: refused, closed, or
 * held past the head bound, which is the finding.
 * */
NYA_INTERNAL b8 _nya_faults_slow_drip(NYA_SimulationRun* run, u32 index) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;

    char line[96] = { 0 };
    s32  length   = snprintf(line, sizeof(line), "X-Drip-%llu: %.48s\r\n", (unsigned long long)faults->slow_bytes[index], "dripdripdripdripdripdripdripdripdripdripdripdrip");
    nya_assert(length > 0 && (u64)length < sizeof(line));

    u64                sent   = 0;
    NYA_OsSocketStatus status = nya_os_socket_send(faults->slow[index], (const u8*)line, (u64)length, &sent);
    faults->slow_bytes[index] += sent;

    nya_system_http_tick();
    nya_system_http_tick();

    u8                 answer[512] = { 0 };
    u64                read        = 0;
    NYA_OsSocketStatus heard       = status == NYA_OS_SOCKET_OK ? nya_os_socket_receive(faults->slow[index], answer, sizeof(answer) - 1, &read) : status;

    if (heard == NYA_OS_SOCKET_OK) {
        u32 code = strncmp((const char*)answer, "HTTP/1.1 ", 9) == 0 ? (u32)strtoul((const char*)answer + 9, nullptr, 10) : 0;

        // the head never ended, so the only right answers are refusals of it.
        if (code < 400 || code >= 500) nya_simulation_fail(run, "the HTTP server answered an unfinished head of %llu bytes with %u", (unsigned long long)faults->slow_bytes[index], code);

        return false;
    }

    if (heard != NYA_OS_SOCKET_WOULD_BLOCK) return false;

    if (faults->slow_bytes[index] > NYA_HTTP_MAX_HEAD_BYTES + _NYA_FAULTS_SLOW_SLACK) {
        nya_simulation_fail(run, "the HTTP server still holds a head of %llu bytes, past its %d byte bound", (unsigned long long)faults->slow_bytes[index], NYA_HTTP_MAX_HEAD_BYTES);
        return false;
    }

    return true;
}

NYA_INTERNAL void _nya_faults_do_slowloris(NYA_SimulationRun* run) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;

    static const char START[] = "GET " _NYA_FAULTS_HTTP_PATH "?size=1 HTTP/1.1\r\nHost: simulation\r\n";

    u32 peers = 1 + (u32)nya_simulation_below(run, _NYA_FAULTS_SLOW_MAX);

    for (u32 i = 0; i < peers; i++) {
        NYA_OsSocket socket = _nya_faults_http_dial();
        if (socket.handle == 0) {
            nya_simulation_fail(run, "the host refused a connection to the HTTP server");
            break;
        }

        u64 sent = 0;
        (void)nya_os_socket_send(socket, (const u8*)START, sizeof(START) - 1, &sent);

        faults->slow[faults->slow_count]       = socket;
        faults->slow_bytes[faults->slow_count] = sent;
        faults->slow_count++;
    }

    // an honest request in the middle of the dribble, which is the liveness half: slow peers hold slots, not the server.
    u32 honest_at = (u32)nya_simulation_below(run, NYA_HTTP_MAX_HEAD_BYTES / 64);
    b8  answered  = false;

    for (u32 round = 0; faults->slow_count > 0; round++) {
        if (round == honest_at) {
            _NYA_FaultsHttpOutcome outcome = _nya_faults_http_exchange(1 + nya_simulation_below(run, 1024));
            answered                       = true;

            if (!(outcome.complete && outcome.status == NYA_HTTP_STATUS_OK && outcome.body_intact)) {
                nya_simulation_fail(run, "an honest request beside %u slow peers was answered %u%s", faults->slow_count, outcome.status, outcome.complete ? "" : " (incomplete)");
            }
        }

        for (u32 index = 0; index < faults->slow_count;) {
            if (_nya_faults_slow_drip(run, index)) {
                index++;
                continue;
            }

            _nya_faults_slow_forget(index);
        }
    }

    nya_simulation_trace(run, answered);

    _nya_faults_http_released(run, 0);
}

NYA_INTERNAL void _nya_faults_do_disk_save(NYA_SimulationRun* run) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;
    nya_arena_free_all(faults->scratch);

    b8  faulty = _nya_faults_disk_faulty();
    u64 value  = run->step + 1;

    NYA_Object* object = nya_object_create(faults->scratch);
    nya_object_add(object, "value", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = value });

    _nya_faults_disk_enter();
    NYA_Error written = nya_save_write(_NYA_FAULTS_SAVE_FILE, object, NYA_SERDE_NONE);
    _nya_faults_disk_leave();

    nya_simulation_trace(run, written.ok);

    if (!written.ok && !faulty) nya_simulation_fail(run, "a save failed on a healthy disk: %s", written.message);

    u64 found  = 0;
    b8  exists = false;
    b8  read   = _nya_faults_save_value(&found, &exists);

    if (written.ok) {
        if (!read || found != value) nya_simulation_fail(run, "an acknowledged save of %llu reads back as %s %llu", (unsigned long long)value, read ? "" : "unreadable", (unsigned long long)found);

        faults->save_exists = true;
        faults->save_value  = value;
    } else if (faults->save_exists) {
        if (!read || found != faults->save_value) {
            nya_simulation_fail(run, "a failed save replaced the previous one: %llu reads back as %s %llu", (unsigned long long)faults->save_value, read ? "" : "unreadable",
                                (unsigned long long)found);
        }
    } else if (exists) {
        nya_simulation_fail(run, "a failed first save left a file behind");
    }

    _nya_faults_save_no_temporaries(run);
}

NYA_INTERNAL void _nya_faults_do_disk_read(NYA_SimulationRun* run) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;
    if (!faults->save_exists) return;

    nya_arena_free_all(faults->scratch);

    faults->plan.rules[NYA_OS_FAULT_FILE_READ] = _nya_faults_rule_draw(run, (1U << NYA_OS_FAULT_IO) | (1U << NYA_OS_FAULT_SHORT) | (1U << NYA_OS_FAULT_FLIP));

    u64 found  = 0;
    b8  exists = false;
    b8  read   = _nya_faults_save_value(&found, &exists);

    faults->plan.rules[NYA_OS_FAULT_FILE_READ] = (NYA_OsFaultRule){ 0 };

    // a refusal is what a lying disk should get; a different value that parses is the save silently corrupted.
    if (read && found != faults->save_value) {
        nya_simulation_fail(run, "a save of %llu read back through bit rot as %llu, and nothing noticed", (unsigned long long)faults->save_value, (unsigned long long)found);
    }
}

NYA_INTERNAL void _nya_faults_do_disk_journal(NYA_SimulationRun* run) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;
    nya_arena_free_all(faults->scratch);

    if (faults->journal_attempts >= _NYA_FAULTS_JOURNAL_MAX) {
        (void)nya_filesystem_delete(faults->journal_path);
        faults->journal_attempts           = 0;
        faults->journal_acknowledged_count = 0;
    }

    b8  faulty = _nya_faults_disk_faulty();
    u64 entry  = faults->journal_attempts++;

    char line[64] = { 0 };
    nya_assert(_nya_faults_journal_line(entry, line, sizeof(line)) > 0);

    _nya_faults_disk_enter();
    NYA_Error appended = nya_file_append(faults->journal_path, (NYA_ConstCString)line);
    _nya_faults_disk_leave();

    nya_simulation_trace(run, appended.ok);

    if (appended.ok) faults->journal_acknowledged[faults->journal_acknowledged_count++] = entry;
    if (!appended.ok && !faulty) nya_simulation_fail(run, "an append failed on a healthy disk: %s", appended.message);

    // read back clean: every line whole, and every acknowledged entry there in the order it was acknowledged.
    NYA_String* text = nya_string_create(faults->scratch);
    if (!nya_file_read(faults->journal_path, text).ok) {
        if (faults->journal_acknowledged_count > 0) nya_simulation_fail(run, "the journal is gone with %u acknowledged entries", faults->journal_acknowledged_count);
        return;
    }

    NYA_ArrayᐸNYA_Stringᐳ* lines = nya_string_split_lines(faults->scratch, text);
    u32                    next  = 0;

    nya_array_foreach (lines, piece) {
        if (nya_string_is_empty(piece)) continue;

        unsigned long long number = 0;
        unsigned long long hash   = 0;
        NYA_CString        raw    = nya_string_to_cstring(faults->scratch, piece);
        char               expected[64] = { 0 };

        b8 parsed = sscanf(raw, "entry %llu %llx", &number, &hash) == 2 && _nya_faults_journal_line(number, expected, sizeof(expected)) > 0
                 && strncmp(expected, raw, strlen(expected) - 1) == 0 && strlen(raw) == strlen(expected) - 1;

        if (!parsed) {
            nya_simulation_fail(run, "the journal holds a torn line '%.40s' after a failed append", raw);
            return;
        }

        if (next < faults->journal_acknowledged_count && number == faults->journal_acknowledged[next]) next++;
    }

    if (next != faults->journal_acknowledged_count) {
        nya_simulation_fail(run, "the journal lost acknowledged entries: %u of %u found in order", next, faults->journal_acknowledged_count);
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FAULTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL void _nya_faults_fault_udp(NYA_SimulationRun* run) {
    u32 sending   = (1U << NYA_OS_FAULT_DROP) | (1U << NYA_OS_FAULT_DUPLICATE) | (1U << NYA_OS_FAULT_DELAY) | (1U << NYA_OS_FAULT_FLIP);
    u32 receiving = (1U << NYA_OS_FAULT_DROP) | (1U << NYA_OS_FAULT_FLIP);

    _NYA_FAULTS.udp_send_to      = _nya_faults_rule_draw(run, sending);
    _NYA_FAULTS.udp_receive_from = nya_simulation_chance(run, 50) ? _nya_faults_rule_draw(run, receiving) : (NYA_OsFaultRule){ 0 };
}

NYA_INTERNAL void _nya_faults_fault_udp_restart(NYA_SimulationRun* run) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;

    _nya_faults_udp_enter();
    defer _nya_faults_udp_leave();

    _nya_faults_udp_close();

    // sometimes the random source fails in the middle of making the pair: a cookie key, an identity. That is a refusal, never a crash.
    if (nya_simulation_chance(run, 40)) {
        faults->plan.rules[NYA_OS_FAULT_RANDOM] = (NYA_OsFaultRule){
            .fail_call = faults->plan.calls[NYA_OS_FAULT_RANDOM] + 1 + nya_simulation_below(run, 3),
            .fail_kind = NYA_OS_FAULT_FAIL,
        };
    }

    u64       injected = faults->plan.injected[NYA_OS_FAULT_RANDOM];
    NYA_Error opened   = _nya_faults_udp_open();

    faults->plan.rules[NYA_OS_FAULT_RANDOM] = (NYA_OsFaultRule){ 0 };

    b8 starved = faults->plan.injected[NYA_OS_FAULT_RANDOM] != injected;
    if (!opened.ok && !starved) nya_simulation_fail(run, "a UDP pair could not be made on a healthy host: %s", opened.message);

    nya_simulation_trace(run, opened.ok);

    if (!opened.ok) NYA_EXPECT(_nya_faults_udp_open(), "while opening the simulated UDP pair after a starved random source");
}

NYA_INTERNAL void _nya_faults_fault_disk(NYA_SimulationRun* run) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;

    // one or two operations at a time, so a run sees each failing on its own as well as together.
    for (u32 op = 0; op <= NYA_OS_FAULT_FILE_RENAME; op++) faults->disk_rules[op] = (NYA_OsFaultRule){ 0 };

    u32 count = 1 + (u32)nya_simulation_below(run, 2);
    for (u32 i = 0; i < count; i++) {
        static const NYA_OsFaultOp OPS[] = { NYA_OS_FAULT_FILE_OPEN, NYA_OS_FAULT_FILE_WRITE, NYA_OS_FAULT_FILE_SYNC, NYA_OS_FAULT_FILE_RENAME };
        NYA_OsFaultOp              op    = OPS[nya_simulation_below(run, nya_carray_length(OPS))];

        faults->disk_rules[op] = _nya_faults_rule_draw(run, U32_MAX);
    }
}

NYA_INTERNAL void _nya_faults_fault_disk_heal(NYA_SimulationRun* run) {
    nya_unused(run);

    for (u32 op = 0; op <= NYA_OS_FAULT_FILE_RENAME; op++) _NYA_FAULTS.disk_rules[op] = (NYA_OsFaultRule){ 0 };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_simulation_faults_add(NYA_SimulationRun* run) {
    nya_assert(run != nullptr);
    nya_assert(!_NYA_FAULTS.initialized, "the fault scenarios are already registered");

    _NYA_SimulationFaults* faults = &_NYA_FAULTS;

    // field by field: the state holds a response buffer, and a compound literal of it is a large stack temporary.
    nya_memset(faults, 0, sizeof(*faults));
    faults->initialized = true;
    faults->arena       = nya_arena_create(.name = "simulation_faults");
    faults->scratch     = nya_arena_create(.name = "simulation_faults_scratch");

    faults->plan.seed          = run->seed ^ _NYA_FAULTS_SEED_SALT;
    faults->plan.random_seeded = true;
    faults->plan.read_step_ns  = _NYA_FAULTS_READ_STEP_NS;
    faults->plan.sockets       = NYA_OS_FAULT_SOCKETS_DATAGRAM | NYA_OS_FAULT_SOCKETS_ACCEPTED;

    NYA_String* prefix = nya_save_path(faults->arena, _NYA_FAULTS_PREFIX);
    if (prefix != nullptr) {
        faults->disk_available = true;
        faults->prefix         = nya_string_to_cstring(faults->arena, prefix);
        faults->root           = nya_string_to_cstring(faults->arena, nya_path_dirname(faults->arena, faults->prefix));
        faults->journal_path   = nya_string_to_cstring(faults->arena, nya_save_path(faults->arena, _NYA_FAULTS_JOURNAL_FILE));
        faults->plan.path_prefix = faults->prefix;

        // what a crashed run left would replay differently.
        (void)nya_save_delete(_NYA_FAULTS_SAVE_FILE);
        (void)nya_save_delete(_NYA_FAULTS_JOURNAL_FILE);
    }

    nya_assert(nya_os_fault_plan_attach(&faults->plan), "another fault plan is attached to the simulation's thread");

    _nya_faults_udp_enter();
    NYA_EXPECT(_nya_faults_udp_open(), "while opening the simulated UDP pair");
    _nya_faults_udp_leave();

    nya_simulation_action_add(run, "udp_send", 20, _nya_faults_do_udp_send);
    nya_simulation_action_add(run, "udp_pump", 10, _nya_faults_do_udp_pump);
    nya_simulation_action_add(run, "udp_converge", 2, _nya_faults_do_udp_converge);
    nya_simulation_fault_add(run, "fault_udp_datagrams", 3, _nya_faults_fault_udp);
    nya_simulation_fault_add(run, "fault_udp_restart", 1, _nya_faults_fault_udp_restart);

    if (nya_http_server_is_running() && nya_http_server_merge(&_NYA_FAULTS_HTTP_ROUTER).ok) {
        faults->http_available = true;
        faults->http_port      = nya_http_server_port();

        nya_simulation_action_add(run, "http_socket_faults", 10, _nya_faults_do_http);
        nya_simulation_action_add(run, "http_slowloris", 1, _nya_faults_do_slowloris);
        nya_simulation_fault_add(run, "fault_http_starved", 2, _nya_faults_do_http_starved);
    }

    if (faults->disk_available) {
        nya_simulation_action_add(run, "disk_save", 10, _nya_faults_do_disk_save);
        nya_simulation_action_add(run, "disk_read", 3, _nya_faults_do_disk_read);
        nya_simulation_action_add(run, "disk_journal", 10, _nya_faults_do_disk_journal);
        nya_simulation_fault_add(run, "fault_disk", 5, _nya_faults_fault_disk);
        nya_simulation_fault_add(run, "fault_disk_heal", 3, _nya_faults_fault_disk_heal);
    }
}

void nya_simulation_faults_remove(void) {
    _NYA_SimulationFaults* faults = &_NYA_FAULTS;
    if (!faults->initialized) return;

    while (faults->slow_count > 0) _nya_faults_slow_forget(0);

    if (faults->http_available) nya_http_server_unmerge(&_NYA_FAULTS_HTTP_ROUTER);

    _nya_faults_udp_enter();
    _nya_faults_udp_close();
    _nya_faults_udp_leave();

    nya_os_fault_plan_detach();

    // the mix, so a run whose faults never fired is visible rather than reported as a pass.
    static const NYA_ConstCString NAMES[NYA_OS_FAULT_OP_COUNT] = {
        "open", "read", "write", "sync", "rename", "accept", "connect", "send", "receive", "send_to", "receive_from", "random", "commit", "alloc",
    };

    printf("  os faults injected:");
    for (u32 op = 0; op < NYA_OS_FAULT_OP_COUNT; op++) {
        if (faults->plan.injected[op] > 0) printf(" %s %llu/%llu", NAMES[op], (unsigned long long)faults->plan.injected[op], (unsigned long long)faults->plan.calls[op]);
    }
    printf("\n");

    if (faults->disk_available) {
        (void)nya_save_delete(_NYA_FAULTS_SAVE_FILE);
        (void)nya_save_delete(_NYA_FAULTS_JOURNAL_FILE);
    }

    nya_arena_destroy(faults->scratch);
    nya_arena_destroy(faults->arena);

    nya_memset(faults, 0, sizeof(*faults));
}

#endif // NYA_TESTING && NYA_OS_FAULT_ENABLED
