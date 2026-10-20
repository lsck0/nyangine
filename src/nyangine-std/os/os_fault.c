#include "nyangine-std/os/os_fault.h"

#if NYA_OS_FAULT_ENABLED

#include "nyangine-std/os/os_file.h"
#include "nyangine-std/os/os_page.h"
#include "nyangine-std/os/os_random.h"
#include "nyangine-std/os/os_socket.h"
#include "nyangine-std/os/os_time.h"

/*
 * The public half of every faulted call. Each one asks the thread's plan, and with none attached is the
 * real call and a thread local load. Included after os_*_linux.c, which define the real bodies under
 * NYA_OS_SYSCALL names; see os_fault.h.
 */

// STATE

NYA_INTERNAL thread_local NYA_OsFaultPlan* _nya_os_fault_plan = nullptr;

/** A datagram the delay fault took off the wire, and how many datagram calls later it goes back on. */
typedef struct {
    NYA_OsSocket  socket;
    NYA_OsAddress to;
    u8            data[NYA_OS_FAULT_DATAGRAM_MAX];
    u64           size;
    u64           release_call;
} _NYA_OsFaultHeld;

/** The attached plan's held datagrams, counted by its `held_count`. */
NYA_INTERNAL thread_local _NYA_OsFaultHeld _nya_os_fault_held[NYA_OS_FAULT_HELD_MAX];

/** Which kinds each operation can take. A rule's `kinds` is masked by this, so a plan cannot ask a sync to come up short. */
#define _NYA_OS_FAULT_BIT(kind) (1U << (kind))

NYA_INTERNAL const u32 _NYA_OS_FAULT_KINDS[NYA_OS_FAULT_OP_COUNT] = {
    [NYA_OS_FAULT_FILE_OPEN]           = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_IO) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_NO_SPACE),
    [NYA_OS_FAULT_FILE_READ]           = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_IO) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_SHORT) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_FLIP),
    [NYA_OS_FAULT_FILE_WRITE]          = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_IO) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_NO_SPACE) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_SHORT),
    [NYA_OS_FAULT_FILE_SYNC]           = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_IO),
    [NYA_OS_FAULT_FILE_RENAME]         = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_IO) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_NO_SPACE),
    [NYA_OS_FAULT_SOCKET_ACCEPT]       = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_FAIL) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_WOULD_BLOCK),
    [NYA_OS_FAULT_SOCKET_CONNECT]      = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_REFUSED),
    [NYA_OS_FAULT_SOCKET_SEND]         = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_WOULD_BLOCK) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_SHORT) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_RESET),
    [NYA_OS_FAULT_SOCKET_RECEIVE]      = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_WOULD_BLOCK) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_SHORT) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_RESET),
    [NYA_OS_FAULT_SOCKET_SEND_TO]      = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_DROP) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_DUPLICATE) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_DELAY)
                                       | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_FLIP) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_WOULD_BLOCK),
    [NYA_OS_FAULT_SOCKET_RECEIVE_FROM] = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_DROP) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_FLIP) | _NYA_OS_FAULT_BIT(NYA_OS_FAULT_WOULD_BLOCK),
    [NYA_OS_FAULT_RANDOM]              = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_FAIL),
    [NYA_OS_FAULT_PAGE_COMMIT]         = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_FAIL),
    [NYA_OS_FAULT_ALLOCATION]          = _NYA_OS_FAULT_BIT(NYA_OS_FAULT_FAIL),
};

/** Salts for the draws that are not a fault decision, so they never land on a decision's coordinate. */
enum {
    _NYA_OS_FAULT_STREAM_KIND = NYA_OS_FAULT_OP_COUNT,
    _NYA_OS_FAULT_STREAM_SHAPE,
    _NYA_OS_FAULT_STREAM_RANDOM,
};

// INTERNAL

/**
 * splitmix64's finaliser over (seed, stream, index). Its constants are Steele, Lea and Flood's, from
 * "Fast splittable pseudorandom number generators" (2014); os sits below base_hash.h, so it carries its own.
 * */
__attr_no_sanitize("unsigned-integer-overflow") NYA_INTERNAL u64 _nya_os_fault_hash(u64 seed, u64 stream, u64 index) {
    u64 x = seed ^ (stream * 0x9E3779B97F4A7C15ULL) ^ (index * 0xD1B54A32D192ED03ULL);

    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;

    return x ^ (x >> 31);
}

NYA_INTERNAL b8 _nya_os_fault_bit_get(const u64* set, s64 descriptor) {
    if (descriptor < 0 || descriptor >= NYA_OS_FAULT_DESCRIPTOR_MAX) return false;

    return (set[descriptor / 64] >> (descriptor % 64)) & 1U;
}

NYA_INTERNAL void _nya_os_fault_bit_set(u64* set, s64 descriptor, b8 on) {
    if (descriptor < 0 || descriptor >= NYA_OS_FAULT_DESCRIPTOR_MAX) return;

    u64 bit = 1ULL << (descriptor % 64);
    set[descriptor / 64] = on ? (set[descriptor / 64] | bit) : (set[descriptor / 64] & ~bit);
}

/** Counts one call of `op` and picks its fault: the scripted one first, then the probability, then which kind. */
NYA_INTERNAL NYA_OsFaultKind _nya_os_fault_pick(NYA_OsFaultPlan* plan, NYA_OsFaultOp op) {
    const u64        call = ++plan->calls[op];
    NYA_OsFaultRule* rule = &plan->rules[op];

    NYA_OsFaultKind kind = NYA_OS_FAULT_NONE;

    if (rule->fail_call != 0 && rule->fail_call == call) {
        kind = rule->fail_kind;
    } else if (rule->probability_ppm > 0 && _nya_os_fault_hash(plan->seed, op, call) % NYA_OS_FAULT_PPM < rule->probability_ppm) {
        u32 allowed = _NYA_OS_FAULT_KINDS[op] & (rule->kinds != 0 ? rule->kinds : U32_MAX);
        u32 count   = (u32)__builtin_popcount(allowed);

        if (count > 0) {
            u32 nth = (u32)(_nya_os_fault_hash(plan->seed, _NYA_OS_FAULT_STREAM_KIND, call * NYA_OS_FAULT_OP_COUNT + op) % count);

            for (u32 bit = 0; bit < NYA_OS_FAULT_KIND_COUNT; bit++) {
                if (!(allowed & _NYA_OS_FAULT_BIT(bit))) continue;
                if (nth == 0) {
                    kind = (NYA_OsFaultKind)bit;
                    break;
                }
                nth--;
            }
        }
    }

    // a scripted kind the operation cannot take is the caller's mistake, and doing nothing is the honest answer to it.
    if (kind != NYA_OS_FAULT_NONE && !(_NYA_OS_FAULT_KINDS[op] & _NYA_OS_FAULT_BIT(kind))) kind = NYA_OS_FAULT_NONE;

    if (kind != NYA_OS_FAULT_NONE) {
        plan->injected[op]++;
        plan->digest = _nya_os_fault_hash(plan->digest, op, (call << 8) | (u64)kind);
    }

    return kind;
}

/** A draw that shapes a fault already decided: where the short call stops, which bit flips, how long a datagram is held. */
NYA_INTERNAL u64 _nya_os_fault_shape(NYA_OsFaultPlan* plan, NYA_OsFaultOp op, u64 limit) {
    if (limit == 0) return 0;

    return _nya_os_fault_hash(plan->seed, _NYA_OS_FAULT_STREAM_SHAPE, plan->calls[op] * NYA_OS_FAULT_OP_COUNT + op) % limit;
}

NYA_INTERNAL b8 _nya_os_fault_path_followed(const NYA_OsFaultPlan* plan, const char* path) {
    if (plan->path_prefix == nullptr || path == nullptr) return false;

    u64 length = strlen(plan->path_prefix);

    return length > 0 && strncmp(path, plan->path_prefix, length) == 0;
}

/** The plan to fault a file call on `file` with, or null. */
NYA_INTERNAL NYA_OsFaultPlan* _nya_os_fault_file(const NYA_OsFile* file) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan == nullptr || file == nullptr) return nullptr;

    return _nya_os_fault_bit_get(plan->files, file->descriptor) ? plan : nullptr;
}

/** The plan to fault a call on `socket` with, or null. */
NYA_INTERNAL NYA_OsFaultPlan* _nya_os_fault_socket(NYA_OsSocket socket) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan == nullptr) return nullptr;

    return _nya_os_fault_bit_get(plan->sockets_followed, _nya_os_socket_fd(socket)) ? plan : nullptr;
}

NYA_INTERNAL void _nya_os_fault_flip(NYA_OsFaultPlan* plan, NYA_OsFaultOp op, u8* bytes, u64 size) {
    if (size == 0) return;

    u64 bit = _nya_os_fault_shape(plan, op, size * 8);
    bytes[bit / 8] ^= (u8)(1U << (bit % 8));
}

/** Sends what the delay fault held whose time has come, or all of it. Ordered oldest first, so two due at once keep their order. */
NYA_INTERNAL void _nya_os_fault_held_release(NYA_OsFaultPlan* plan, b8 all) {
    const u64 now = plan->calls[NYA_OS_FAULT_SOCKET_SEND_TO] + plan->calls[NYA_OS_FAULT_SOCKET_RECEIVE_FROM];

    u32 kept = 0;

    for (u32 i = 0; i < plan->held_count; i++) {
        _NYA_OsFaultHeld* held = &_nya_os_fault_held[i];

        if (all || held->release_call <= now) {
            // the socket may have closed since, and a datagram that cannot go is one the network lost.
            (void)_nya_os_real_nya_os_socket_send_to(held->socket, held->to, held->data, held->size);
            continue;
        }

        if (kept != i) _nya_os_fault_held[kept] = *held;
        kept++;
    }

    plan->held_count = kept;
}

/** Forgets a descriptor's held datagrams, so a number the kernel hands out again does not send its predecessor's. */
NYA_INTERNAL void _nya_os_fault_held_forget(NYA_OsFaultPlan* plan, NYA_OsSocket socket) {
    u32 kept = 0;

    for (u32 i = 0; i < plan->held_count; i++) {
        if (_nya_os_fault_held[i].socket.handle == socket.handle) continue;
        if (kept != i) _nya_os_fault_held[kept] = _nya_os_fault_held[i];
        kept++;
    }

    plan->held_count = kept;
}

// LIFETIME

b8 nya_os_fault_plan_attach(NYA_OsFaultPlan* plan) {
    if (plan == nullptr || _nya_os_fault_plan != nullptr) return false;

    if (plan->monotonic_ns == 0) plan->monotonic_ns = NYA_OS_FAULT_MONOTONIC_ORIGIN_NS;

    // the ring is the thread's and starts empty, so whatever a plan said it held before is not on it.
    plan->held_count = 0;

    _nya_os_fault_plan = plan;
    return true;
}

void nya_os_fault_plan_detach(void) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan == nullptr) return;

    _nya_os_fault_held_release(plan, true);
    _nya_os_fault_plan = nullptr;
}

NYA_OsFaultPlan* nya_os_fault_plan_current(void) {
    return _nya_os_fault_plan;
}

void nya_os_fault_time_advance(NYA_OsFaultPlan* plan, u64 nanoseconds) {
    if (plan == nullptr) return;

    plan->monotonic_ns += nanoseconds;
}

void nya_os_fault_time_skew_wall(NYA_OsFaultPlan* plan, s64 nanoseconds) {
    if (plan == nullptr) return;

    plan->wall_skew_ns += nanoseconds;
}

void nya_os_fault_datagrams_flush(NYA_OsFaultPlan* plan) {
    if (plan == nullptr) return;

    _nya_os_fault_held_release(plan, true);
}

void nya_os_fault_heal(NYA_OsFaultPlan* plan) {
    if (plan == nullptr) return;

    memset(plan->rules, 0, sizeof(plan->rules));
}

NYA_OsFaultKind nya_os_fault_decide(NYA_OsFaultOp op) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan == nullptr || op >= NYA_OS_FAULT_OP_COUNT) return NYA_OS_FAULT_NONE;

    return _nya_os_fault_pick(plan, op);
}

// FILES

NYA_OsFileStatus nya_os_file_open(const char* path, u32 flags, OUT NYA_OsFile* out_file) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    b8               followed = plan != nullptr && _nya_os_fault_path_followed(plan, path);

    if (followed) {
        NYA_OsFaultKind kind = _nya_os_fault_pick(plan, NYA_OS_FAULT_FILE_OPEN);

        // a full disk only refuses an open that would make a file; opening one that exists costs no space.
        if (kind == NYA_OS_FAULT_NO_SPACE && (flags & NYA_OS_FILE_OPEN_CREATE)) return NYA_OS_FILE_STATUS_NO_SPACE;
        if (kind == NYA_OS_FAULT_IO) return NYA_OS_FILE_STATUS_IO;
    }

    NYA_OsFileStatus status = _nya_os_real_nya_os_file_open(path, flags, out_file);

    if (plan != nullptr && status == NYA_OS_FILE_STATUS_OK) _nya_os_fault_bit_set(plan->files, out_file->descriptor, followed);

    return status;
}

void nya_os_file_close(NYA_OsFile* file) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan != nullptr && file != nullptr) _nya_os_fault_bit_set(plan->files, file->descriptor, false);

    _nya_os_real_nya_os_file_close(file);
}

NYA_OsFileStatus nya_os_file_read(NYA_OsFile* file, OUT u8* buffer, u64 size, OUT u64* out_read) {
    NYA_OsFaultPlan* plan = _nya_os_fault_file(file);
    if (plan == nullptr) return _nya_os_real_nya_os_file_read(file, buffer, size, out_read);

    NYA_OsFaultKind kind = _nya_os_fault_pick(plan, NYA_OS_FAULT_FILE_READ);
    if (kind == NYA_OS_FAULT_IO) return NYA_OS_FILE_STATUS_IO;

    u64 asked = kind == NYA_OS_FAULT_SHORT && size > 1 ? 1 + _nya_os_fault_shape(plan, NYA_OS_FAULT_FILE_READ, size - 1) : size;

    NYA_OsFileStatus status = _nya_os_real_nya_os_file_read(file, buffer, asked, out_read);

    if (status == NYA_OS_FILE_STATUS_OK && kind == NYA_OS_FAULT_FLIP) _nya_os_fault_flip(plan, NYA_OS_FAULT_FILE_READ, buffer, *out_read);

    return status;
}

NYA_OsFileStatus nya_os_file_write(NYA_OsFile* file, const u8* buffer, u64 size, OUT u64* out_written) {
    NYA_OsFaultPlan* plan = _nya_os_fault_file(file);
    if (plan == nullptr) return _nya_os_real_nya_os_file_write(file, buffer, size, out_written);

    NYA_OsFaultKind kind = _nya_os_fault_pick(plan, NYA_OS_FAULT_FILE_WRITE);
    if (kind == NYA_OS_FAULT_IO) return NYA_OS_FILE_STATUS_IO;

    // a disk that fills mid write takes what fits and refuses the rest, so some bytes land before the refusal.
    if (kind == NYA_OS_FAULT_NO_SPACE) {
        u64 fits = _nya_os_fault_shape(plan, NYA_OS_FAULT_FILE_WRITE, size);
        if (fits > 0) (void)_nya_os_real_nya_os_file_write(file, buffer, fits, out_written);

        return NYA_OS_FILE_STATUS_NO_SPACE;
    }

    u64 asked = kind == NYA_OS_FAULT_SHORT && size > 1 ? 1 + _nya_os_fault_shape(plan, NYA_OS_FAULT_FILE_WRITE, size - 1) : size;

    return _nya_os_real_nya_os_file_write(file, buffer, asked, out_written);
}

NYA_OsFileStatus nya_os_file_sync(NYA_OsFile* file) {
    NYA_OsFaultPlan* plan = _nya_os_fault_file(file);
    if (plan != nullptr && _nya_os_fault_pick(plan, NYA_OS_FAULT_FILE_SYNC) == NYA_OS_FAULT_IO) return NYA_OS_FILE_STATUS_IO;

    return _nya_os_real_nya_os_file_sync(file);
}

/** The fault a rename between `source` and `destination` takes, which either end being followed is enough for. */
NYA_INTERNAL NYA_OsFileStatus _nya_os_fault_rename(const char* source, const char* destination) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan == nullptr) return NYA_OS_FILE_STATUS_OK;
    if (!_nya_os_fault_path_followed(plan, source) && !_nya_os_fault_path_followed(plan, destination)) return NYA_OS_FILE_STATUS_OK;

    switch (_nya_os_fault_pick(plan, NYA_OS_FAULT_FILE_RENAME)) {
        case NYA_OS_FAULT_IO:       return NYA_OS_FILE_STATUS_IO;
        case NYA_OS_FAULT_NO_SPACE: return NYA_OS_FILE_STATUS_NO_SPACE;
        default:                    return NYA_OS_FILE_STATUS_OK;
    }
}

NYA_OsFileStatus nya_os_file_rename(const char* source, const char* destination) {
    NYA_OsFileStatus fault = _nya_os_fault_rename(source, destination);
    if (fault != NYA_OS_FILE_STATUS_OK) return fault;

    return _nya_os_real_nya_os_file_rename(source, destination);
}

NYA_OsFileStatus nya_os_file_replace(const char* source, const char* destination) {
    NYA_OsFileStatus fault = _nya_os_fault_rename(source, destination);
    if (fault != NYA_OS_FILE_STATUS_OK) return fault;

    return _nya_os_real_nya_os_file_replace(source, destination);
}

// SOCKETS

NYA_OsSocketStatus nya_os_socket_open_at(NYA_OsSocketKind kind, NYA_OsAddress address, u32 backlog, OUT NYA_OsSocket* out_socket) {
    NYA_OsSocketStatus status = _nya_os_real_nya_os_socket_open_at(kind, address, backlog, out_socket);

    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan != nullptr && status == NYA_OS_SOCKET_OK) {
        b8 followed = kind == NYA_OS_SOCKET_DATAGRAM && (plan->sockets & NYA_OS_FAULT_SOCKETS_DATAGRAM);
        _nya_os_fault_bit_set(plan->sockets_followed, _nya_os_socket_fd(*out_socket), followed);
    }

    return status;
}

NYA_OsSocketStatus nya_os_socket_accept(NYA_OsSocket listener, OUT NYA_OsSocket* out_socket, OUT NYA_OsAddress* out_from) {
    NYA_OsFaultPlan* plan     = _nya_os_fault_plan;
    b8               followed = plan != nullptr && (plan->sockets & NYA_OS_FAULT_SOCKETS_ACCEPTED);

    if (followed) {
        NYA_OsFaultKind kind = _nya_os_fault_pick(plan, NYA_OS_FAULT_SOCKET_ACCEPT);

        if (kind != NYA_OS_FAULT_NONE) {
            *out_socket = NYA_OS_SOCKET_NONE;
            memset(out_from, 0, sizeof(NYA_OsAddress));

            // the connection stays in the backlog either way, and the next accept takes it.
            return kind == NYA_OS_FAULT_WOULD_BLOCK ? NYA_OS_SOCKET_WOULD_BLOCK : NYA_OS_SOCKET_FAILED;
        }
    }

    NYA_OsSocketStatus status = _nya_os_real_nya_os_socket_accept(listener, out_socket, out_from);

    if (plan != nullptr && status == NYA_OS_SOCKET_OK) _nya_os_fault_bit_set(plan->sockets_followed, _nya_os_socket_fd(*out_socket), followed);

    return status;
}

NYA_OsSocketStatus nya_os_socket_connect(NYA_OsAddress address, OUT NYA_OsSocket* out_socket) {
    NYA_OsFaultPlan* plan     = _nya_os_fault_plan;
    b8               followed = plan != nullptr && (plan->sockets & NYA_OS_FAULT_SOCKETS_CONNECTED);

    if (followed && _nya_os_fault_pick(plan, NYA_OS_FAULT_SOCKET_CONNECT) == NYA_OS_FAULT_REFUSED) {
        *out_socket = NYA_OS_SOCKET_NONE;
        return NYA_OS_SOCKET_REFUSED;
    }

    NYA_OsSocketStatus status = _nya_os_real_nya_os_socket_connect(address, out_socket);

    if (plan != nullptr && out_socket->handle != 0) _nya_os_fault_bit_set(plan->sockets_followed, _nya_os_socket_fd(*out_socket), followed);

    return status;
}

void nya_os_socket_close(NYA_OsSocket socket) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;

    if (plan != nullptr) {
        _nya_os_fault_bit_set(plan->sockets_followed, _nya_os_socket_fd(socket), false);
        _nya_os_fault_held_forget(plan, socket);
    }

    _nya_os_real_nya_os_socket_close(socket);
}

NYA_OsSocketStatus nya_os_socket_send(NYA_OsSocket socket, const u8* data, u64 size, OUT u64* out_sent) {
    NYA_OsFaultPlan* plan = _nya_os_fault_socket(socket);
    if (plan == nullptr) return _nya_os_real_nya_os_socket_send(socket, data, size, out_sent);

    NYA_OsFaultKind kind = _nya_os_fault_pick(plan, NYA_OS_FAULT_SOCKET_SEND);
    *out_sent            = 0;

    if (kind == NYA_OS_FAULT_WOULD_BLOCK) return NYA_OS_SOCKET_WOULD_BLOCK;

    if (kind == NYA_OS_FAULT_RESET) {
        _nya_os_socket_break(socket);
        return NYA_OS_SOCKET_CLOSED;
    }

    u64 asked = kind == NYA_OS_FAULT_SHORT && size > 1 ? 1 + _nya_os_fault_shape(plan, NYA_OS_FAULT_SOCKET_SEND, size - 1) : size;

    return _nya_os_real_nya_os_socket_send(socket, data, asked, out_sent);
}

NYA_OsSocketStatus nya_os_socket_receive(NYA_OsSocket socket, OUT u8* out_data, u64 capacity, OUT u64* out_read) {
    NYA_OsFaultPlan* plan = _nya_os_fault_socket(socket);
    if (plan == nullptr) return _nya_os_real_nya_os_socket_receive(socket, out_data, capacity, out_read);

    NYA_OsFaultKind kind = _nya_os_fault_pick(plan, NYA_OS_FAULT_SOCKET_RECEIVE);
    *out_read            = 0;

    if (kind == NYA_OS_FAULT_WOULD_BLOCK) return NYA_OS_SOCKET_WOULD_BLOCK;

    if (kind == NYA_OS_FAULT_RESET) {
        _nya_os_socket_break(socket);
        return NYA_OS_SOCKET_CLOSED;
    }

    u64 asked = kind == NYA_OS_FAULT_SHORT && capacity > 1 ? 1 + _nya_os_fault_shape(plan, NYA_OS_FAULT_SOCKET_RECEIVE, capacity - 1) : capacity;

    return _nya_os_real_nya_os_socket_receive(socket, out_data, asked, out_read);
}

NYA_OsSocketStatus nya_os_socket_send_to(NYA_OsSocket socket, NYA_OsAddress to, const u8* data, u64 size) {
    NYA_OsFaultPlan* plan = _nya_os_fault_socket(socket);
    if (plan == nullptr) return _nya_os_real_nya_os_socket_send_to(socket, to, data, size);

    NYA_OsFaultKind kind = _nya_os_fault_pick(plan, NYA_OS_FAULT_SOCKET_SEND_TO);
    _nya_os_fault_held_release(plan, false);

    switch (kind) {
        case NYA_OS_FAULT_WOULD_BLOCK: return NYA_OS_SOCKET_WOULD_BLOCK;

        // the host took it, and the network lost it: the sender cannot tell, which is the point.
        case NYA_OS_FAULT_DROP:        return NYA_OS_SOCKET_OK;

        case NYA_OS_FAULT_DUPLICATE: {
            (void)_nya_os_real_nya_os_socket_send_to(socket, to, data, size);
            return _nya_os_real_nya_os_socket_send_to(socket, to, data, size);
        }

        case NYA_OS_FAULT_FLIP: {
            if (size == 0 || size > NYA_OS_FAULT_DATAGRAM_MAX) break;

            u8 copy[NYA_OS_FAULT_DATAGRAM_MAX];
            memcpy(copy, data, size);
            _nya_os_fault_flip(plan, NYA_OS_FAULT_SOCKET_SEND_TO, copy, size);

            return _nya_os_real_nya_os_socket_send_to(socket, to, copy, size);
        }

        case NYA_OS_FAULT_DELAY: {
            if (size > NYA_OS_FAULT_DATAGRAM_MAX || plan->held_count >= NYA_OS_FAULT_HELD_MAX) break;

            _NYA_OsFaultHeld* held = &_nya_os_fault_held[plan->held_count++];
            held->socket          = socket;
            held->to              = to;
            held->size            = size;
            held->release_call    = plan->calls[NYA_OS_FAULT_SOCKET_SEND_TO] + plan->calls[NYA_OS_FAULT_SOCKET_RECEIVE_FROM] + 1
                               + _nya_os_fault_shape(plan, NYA_OS_FAULT_SOCKET_SEND_TO, NYA_OS_FAULT_HOLD_CALLS_MAX);
            memcpy(held->data, data, size);

            return NYA_OS_SOCKET_OK;
        }

        default: break;
    }

    return _nya_os_real_nya_os_socket_send_to(socket, to, data, size);
}

NYA_OsSocketStatus nya_os_socket_receive_from(NYA_OsSocket socket, OUT u8* out_data, u64 capacity, OUT u64* out_size, OUT NYA_OsAddress* out_from) {
    NYA_OsFaultPlan* plan = _nya_os_fault_socket(socket);
    if (plan == nullptr) return _nya_os_real_nya_os_socket_receive_from(socket, out_data, capacity, out_size, out_from);

    NYA_OsFaultKind kind = _nya_os_fault_pick(plan, NYA_OS_FAULT_SOCKET_RECEIVE_FROM);
    _nya_os_fault_held_release(plan, false);

    if (kind == NYA_OS_FAULT_WOULD_BLOCK) {
        *out_size = 0;
        memset(out_from, 0, sizeof(NYA_OsAddress));
        return NYA_OS_SOCKET_WOULD_BLOCK;
    }

    NYA_OsSocketStatus status = _nya_os_real_nya_os_socket_receive_from(socket, out_data, capacity, out_size, out_from);
    if (status != NYA_OS_SOCKET_OK) return status;

    // read and thrown away, so the next call finds the datagram after it, as if this one never arrived.
    if (kind == NYA_OS_FAULT_DROP) {
        *out_size = 0;
        memset(out_from, 0, sizeof(NYA_OsAddress));
        return NYA_OS_SOCKET_WOULD_BLOCK;
    }

    if (kind == NYA_OS_FAULT_FLIP) _nya_os_fault_flip(plan, NYA_OS_FAULT_SOCKET_RECEIVE_FROM, out_data, *out_size);

    return NYA_OS_SOCKET_OK;
}

// TIME

u64 nya_os_time_monotonic_ns(void) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan == nullptr || !plan->time_simulated) return _nya_os_real_nya_os_time_monotonic_ns();

    plan->monotonic_ns += plan->read_step_ns;
    return plan->monotonic_ns;
}

u64 nya_os_time_wall_ns(void) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan == nullptr || !plan->time_simulated) return _nya_os_real_nya_os_time_wall_ns();

    // the monotonic clock's distance from its origin, on the wall clock's: the two agree until something skews one.
    s64 wall = (s64)(NYA_OS_FAULT_WALL_ORIGIN_NS + (plan->monotonic_ns - NYA_OS_FAULT_MONOTONIC_ORIGIN_NS)) + plan->wall_skew_ns;

    return wall < 0 ? 0 : (u64)wall;
}

void nya_os_time_sleep_ms(u32 milliseconds) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan == nullptr || !plan->time_simulated) {
        _nya_os_real_nya_os_time_sleep_ms(milliseconds);
        return;
    }

    plan->monotonic_ns += (u64)milliseconds * 1'000'000ULL;
}

// RANDOM

b8 nya_os_random_bytes(OUT u8* out, u64 size) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan == nullptr) return _nya_os_real_nya_os_random_bytes(out, size);

    if (_nya_os_fault_pick(plan, NYA_OS_FAULT_RANDOM) == NYA_OS_FAULT_FAIL) return false;
    if (!plan->random_seeded) return _nya_os_real_nya_os_random_bytes(out, size);

    // the same refusals the real call makes, so a seeded run cannot hide a bad request.
    if (out == nullptr || size == 0 || size > NYA_OS_RANDOM_MAX_BYTES) return false;

    for (u64 at = 0; at < size; at += sizeof(u64)) {
        u64 word  = _nya_os_fault_hash(plan->seed, _NYA_OS_FAULT_STREAM_RANDOM, plan->random_draws++);
        u64 count = size - at < sizeof(u64) ? size - at : sizeof(u64);
        memcpy(out + at, &word, count);
    }

    return true;
}

// PAGES

b8 nya_os_page_commit(void* address, u64 size) {
    NYA_OsFaultPlan* plan = _nya_os_fault_plan;
    if (plan != nullptr && _nya_os_fault_pick(plan, NYA_OS_FAULT_PAGE_COMMIT) == NYA_OS_FAULT_FAIL) return false;

    return _nya_os_real_nya_os_page_commit(address, size);
}

#endif // NYA_OS_FAULT_ENABLED
