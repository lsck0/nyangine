/**
 * @file base_socket.h
 *
 * What sits on top of os_socket.h: a name lookup that does not stop the frame, and the errors the rest
 * of the engine speaks.
 *
 * The sockets themselves need nothing added — `os` already hands over non-blocking ones with a status
 * per call — so this is deliberately small. It holds the one thing a program cannot do without an
 * allocator and a thread: resolving a name.
 *
 * ```c
 * NYA_Resolver* resolver = nullptr;
 * NYA_TRY(nya_resolver_create(arena, "example.com", 443, NYA_OS_ADDRESS_NONE, &resolver));
 *
 * // once a frame, until it is not pending
 * NYA_OsAddress address = { 0 };
 *
 * switch (nya_resolver_poll(resolver, &address)) {
 *     case NYA_RESOLVER_PENDING: break;
 *     case NYA_RESOLVER_OK:      connect_to(address); nya_resolver_destroy(resolver); break;
 *     case NYA_RESOLVER_FAILED:  give_up();           nya_resolver_destroy(resolver); break;
 * }
 * ```
 *
 * ── why a thread ──
 *
 * `getaddrinfo` blocks, and there is no portable non-blocking version of it: the host's own resolver
 * may go to a file, a daemon, or a name server across the network, and a name that does not resolve
 * can take seconds to say so. A frame cannot wait for that, so the call is made on a thread of its own
 * and the answer is picked up whenever the program next asks. One thread per lookup, which is right
 * because a program makes one or two of these at startup and then not again — a pool would be
 * machinery for a rate nothing here has.
 *
 * A resolver whose program stops caring is destroyed, and destroying one whose thread is still inside
 * the host's resolver abandons the thread rather than waiting for it: it writes into memory the
 * resolver owns, which is therefore kept until the thread is done with it. See nya_resolver_destroy.
 *
 * Thread safety: a resolver is polled from one thread, the one that started it.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/os/os_socket.h"

// TYPES

typedef enum NYA_ResolverStatus NYA_ResolverStatus;
typedef struct NYA_Resolver     NYA_Resolver;

/** Where a lookup has got to. */
enum NYA_ResolverStatus {
    /** Still asking. The answer most polls get, and not a failure. */
    NYA_RESOLVER_PENDING = 0,

    /** There is an address, and it is in the out parameter. */
    NYA_RESOLVER_OK,

    /** The name does not resolve, or the host's resolver refused to answer. */
    NYA_RESOLVER_FAILED,

    NYA_RESOLVER_STATUS_COUNT,
};

// FUNCTIONS

/**
 * Starts looking `host` up, with `port` carried through to the answer.
 *
 * A literal — "127.0.0.1", "::1" — is answered without a thread and without asking anybody, so the
 * first poll already has it: the common case in a game costs nothing.
 *
 * `prefer` picks which family to take when a name has both; NYA_OS_ADDRESS_NONE takes whichever the
 * host's own policy puts first.
 * */
NYA_API NYA_Error nya_resolver_create(NYA_Arena* arena, NYA_ConstCString host, u16 port, NYA_OsAddressKind prefer, OUT NYA_Resolver** out_resolver)
    __attr_no_discard;

/** Where the lookup is, and the address once there is one. */
NYA_API NYA_ResolverStatus nya_resolver_poll(NYA_Resolver* resolver, OUT NYA_OsAddress* out_address) __attr_no_discard;

/**
 * Lets a resolver go. Null is a no-op, and calling it while the lookup is still running is allowed.
 *
 * A lookup that has finished takes its thread with it. One that has not is abandoned: the host's
 * resolver is inside a call nothing can cancel, and the memory it writes into belongs to the resolver,
 * so the resolver outlives this call and is freed with the arena it was made from. That is a bounded
 * leak until the arena goes, and the alternative is a frame that waits for a name server.
 * */
NYA_API void nya_resolver_destroy(NYA_Resolver* resolver);

/** What the host said, in words, once a lookup failed. Empty while it is pending or succeeded. */
NYA_API NYA_ConstCString nya_resolver_error(const NYA_Resolver* resolver) __attr_no_discard;
