#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_socket.h"
#include "nyangine-std/base/base_thread.h"

// PRIVATE TYPES

/** Bytes a name may take, terminator included. A hostname is at most 253 by the standard. */
#define _NYA_RESOLVER_MAX_HOST 256

struct NYA_Resolver {
    NYA_Arena* arena;

    char host[_NYA_RESOLVER_MAX_HOST];
    u16  port;

    NYA_OsAddressKind prefer;

    /**
     * The thread's whole world.
     *
     * Written by the thread and read by the poll, which is safe without a lock for the one shape this
     * has: `done` is written last and read first, so a poll either sees a lookup that has not finished
     * or one whose answer was written before the flag that announces it. Anything more — cancelling,
     * restarting, several waiters — would need one, and none of those exist.
     * */
    NYA_OsAddress      address;
    NYA_OsSocketStatus status;
    b8                 done;

    NYA_Thread* thread;

    /** Set when the thread was abandoned, so a second destroy does not free what it still writes into. */
    b8 abandoned;
};

// PRIVATE API DECLARATION

/** The whole of the thread: one blocking lookup, then the flag that says so. */
NYA_INTERNAL void _nya_resolver_run(void* data);

/**
 * Whether `host` is an address already written out, which is a lookup nobody has to be asked about.
 *
 * By shape rather than by parsing it: a v6 literal is the only thing here that may hold a colon, and a
 * v4 one is digits and dots. Anything else goes to the resolver, so a name this gets wrong is a name
 * that takes a thread it did not need — never a name that is answered wrongly.
 * */
NYA_INTERNAL b8 _nya_resolver_is_literal(NYA_ConstCString host) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error nya_resolver_create(NYA_Arena* arena, NYA_ConstCString host, u16 port, NYA_OsAddressKind prefer, NYA_Resolver** out_resolver) {
    nya_assert(arena != nullptr && out_resolver != nullptr);

    *out_resolver = nullptr;

    if (host == nullptr || host[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a lookup needs a name");
    if (strlen(host) >= _NYA_RESOLVER_MAX_HOST) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is longer than a hostname may be", host);

    NYA_Resolver* resolver = nya_arena_alloc(arena, sizeof(NYA_Resolver));
    nya_memset(resolver, 0, sizeof(NYA_Resolver));

    resolver->arena  = arena;
    resolver->port   = port;
    resolver->prefer = prefer;

    (void)snprintf(resolver->host, sizeof(resolver->host), "%s", host);

    /* A literal is answered here on this thread, since turning "127.0.0.1" into four bytes asks nobody anything; the shape is checked before the call because a name goes to the host's resolver, which may block. */
    if (_nya_resolver_is_literal(resolver->host)) {
        resolver->status = nya_os_address_resolve(resolver->host, port, prefer, &resolver->address);
        resolver->done   = true;

        *out_resolver = resolver;

        return NYA_OK;
    }

    NYA_Error spawned = nya_thread_spawn(arena, _nya_resolver_run, resolver, "resolver", &resolver->thread);

    if (!spawned.ok) return spawned;

    *out_resolver = resolver;

    return NYA_OK;
}

NYA_ResolverStatus nya_resolver_poll(NYA_Resolver* resolver, NYA_OsAddress* out_address) {
    nya_assert(resolver != nullptr && out_address != nullptr);

    nya_memset(out_address, 0, sizeof(NYA_OsAddress));

    if (!resolver->done) return NYA_RESOLVER_PENDING;

    if (resolver->status != NYA_OS_SOCKET_OK) return NYA_RESOLVER_FAILED;

    *out_address = resolver->address;

    return NYA_RESOLVER_OK;
}

void nya_resolver_destroy(NYA_Resolver* resolver) {
    if (resolver == nullptr || resolver->abandoned) return;

    if (resolver->thread == nullptr) return;

    if (nya_thread_is_finished(resolver->thread)) {
        nya_thread_join(resolver->thread);
        resolver->thread = nullptr;

        return;
    }

    /* Still inside the host's resolver, a call nothing can cancel: waiting here would be the frame stall this file exists to avoid, so the thread is let go and its struct left alive until the arena takes it. */
    nya_thread_abandon(resolver->thread);

    resolver->thread    = nullptr;
    resolver->abandoned = true;
}

NYA_ConstCString nya_resolver_error(const NYA_Resolver* resolver) {
    nya_assert(resolver != nullptr);

    if (!resolver->done || resolver->status == NYA_OS_SOCKET_OK) return "";

    if (resolver->status == NYA_OS_SOCKET_UNREACHABLE) return "the name does not resolve";

    return "the host's resolver refused to answer";
}

// PRIVATE API IMPLEMENTATION

b8 _nya_resolver_is_literal(NYA_ConstCString host) {
    b8 digits_and_dots = true;

    for (u64 index = 0; host[index] != '\0'; index++) {
        if (host[index] == ':') return true;

        if ((host[index] < '0' || host[index] > '9') && host[index] != '.') digits_and_dots = false;
    }

    return digits_and_dots;
}

void _nya_resolver_run(void* data) {
    NYA_Resolver* resolver = (NYA_Resolver*)data;

    NYA_OsAddress      address = { 0 };
    NYA_OsSocketStatus status  = nya_os_address_resolve(resolver->host, resolver->port, resolver->prefer, &address);

    resolver->address = address;
    resolver->status  = status;

    // Last, and on purpose: a poll reads this first, so a reader either sees a lookup that has not finished or one whose answer was already written.
    resolver->done = true;
}
