/**
 * @file os_socket.h
 *
 * Sockets as the operating system hands them over: a datagram socket, a listening stream socket, the
 * ones it accepts, and a wait that ends when any of them has something. The caller owns the storage, a
 * status says whether a call worked, and nothing here allocates or asserts.
 *
 * ```c
 * NYA_OsSocket listener;
 * if (nya_os_socket_open(NYA_OS_SOCKET_LISTENER, 8080, 0, &listener) != NYA_OS_SOCKET_OK) return false;
 *
 * NYA_OsSocket  client;
 * NYA_OsAddress from;
 *
 * // NYA_OS_SOCKET_WOULD_BLOCK is the answer most of the time, and it is not a failure.
 * if (nya_os_socket_accept(listener, &client, &from) == NYA_OS_SOCKET_OK) { ... }
 * ```
 *
 * Everything the engine actually serves with — a connection table, a read buffer, a write queue, a
 * transport that fragments and retries — is above this, in `net` and `http`.
 *
 * ── everything is non-blocking, always ──
 *
 * Every socket made here is non-blocking from the moment it exists, and every call that could wait
 * answers NYA_OS_SOCKET_WOULD_BLOCK instead. That is not a convenience: a blocking socket is a socket
 * that stops a frame, and a program that wants to wait says so once, in nya_os_socket_wait, where the
 * waiting is bounded and covers every socket at once.
 *
 * The one consequence to keep in mind is that a send is allowed to take part of what it was given. The
 * bytes it took are reported and the rest is the caller's to send later, because the alternative —
 * looping in here until the kernel takes it all — is a blocking send with extra steps. Whoever owns
 * the connection owns the queue.
 *
 * ── addresses ──
 *
 * NYA_OsAddress is a family, sixteen bytes and a port: v4 and v6 in one struct, by value, comparable
 * with nya_os_address_equals and printable with nya_os_address_text. Nothing here holds a name.
 * Resolution is a blocking call the host makes on its own schedule, so nya_os_address_resolve is the
 * one function here that may take a while, and `base` is where it gets a thread to be slow on.
 *
 * ── what this deliberately does not do ──
 *
 * No TLS, no name caching, no retry and no framing. A datagram is a datagram and a stream is bytes;
 * what they mean is decided above. TLS in particular is its own file when it lands, because a
 * handshake is a protocol rather than a syscall.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A socket that was never opened, and what a closed one is set to. Zero, so a zeroed struct holds none. */
#define NYA_OS_SOCKET_NONE ((NYA_OsSocket){ .handle = 0 })

/** Bytes an address's text form needs, terminator included: an IPv6 literal in brackets with a port. */
#define NYA_OS_ADDRESS_TEXT_MAX 64

/** How many connections may be waiting to be accepted before the host refuses one. */
#define NYA_OS_SOCKET_BACKLOG 64

/**
 * Sockets one wait may watch.
 *
 * The HTTP server's connection table and a game's handful of transports both sit far inside this, and
 * a program that wants more than this many at once wants an event port rather than a list: that is a
 * different call and it will be a different function when something needs it.
 * */
#define NYA_OS_SOCKET_WAIT_MAX 128

/** A wait that only ends when a socket is ready. The same spelling os_thread.h and os_process.h take. */
#define NYA_OS_SOCKET_WAIT_FOREVER ((u32)0xFFFFFFFF)

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_OsSocketKind   NYA_OsSocketKind;
typedef enum NYA_OsSocketStatus NYA_OsSocketStatus;
typedef enum NYA_OsAddressKind  NYA_OsAddressKind;
typedef struct NYA_OsSocket     NYA_OsSocket;
typedef struct NYA_OsAddress    NYA_OsAddress;
typedef struct NYA_OsSocketWait NYA_OsSocketWait;

/** How a call here ended. One shape for every function, since none of them knows what a failure means. */
enum NYA_OsSocketStatus {
    /** It worked, and whatever it was asked for is in the out parameters. */
    NYA_OS_SOCKET_OK,

    /**
     * Nothing to read, no room to write, or nobody waiting to be accepted.
     *
     * Not a failure and the ordinary answer: every socket here is non-blocking, so this is what "ask
     * again later" looks like.
     * */
    NYA_OS_SOCKET_WOULD_BLOCK,

    /** The peer closed the connection, or a send found it already gone. The socket is finished. */
    NYA_OS_SOCKET_CLOSED,

    /** Nobody is listening there. A connect's own answer, and a datagram's when the host bothers to say. */
    NYA_OS_SOCKET_REFUSED,

    /** That port is already taken, which is the one bind failure a program can do something about. */
    NYA_OS_SOCKET_IN_USE,

    /** There is no route, or the name does not resolve. */
    NYA_OS_SOCKET_UNREACHABLE,

    /** The operating system refused for some other reason, which is for the caller above to make sense of. */
    NYA_OS_SOCKET_FAILED,

    NYA_OS_SOCKET_STATUS_COUNT,
};

/** Which kind of socket to open. */
enum NYA_OsSocketKind {
    /** Datagrams: bound to a port, and every send says where it goes. */
    NYA_OS_SOCKET_DATAGRAM = 0,

    /** A stream listener: bound, listening, and answering nya_os_socket_accept. */
    NYA_OS_SOCKET_LISTENER,

    NYA_OS_SOCKET_KIND_COUNT,
};

/** Which kind of address, which is also how many of its bytes mean anything. */
enum NYA_OsAddressKind {
    /** None. The zero, so a zeroed address is not an address rather than being 0.0.0.0. */
    NYA_OS_ADDRESS_NONE = 0,

    /** IPv4: the first four bytes. */
    NYA_OS_ADDRESS_V4,

    /** IPv6: all sixteen. */
    NYA_OS_ADDRESS_V6,

    NYA_OS_ADDRESS_KIND_COUNT,
};

/**
 * One socket, as the host names it.
 *
 * A struct rather than a bare integer because the two hosts disagree about what a socket is — a small
 * file descriptor on Linux, a pointer sized handle on Windows — and because a bare integer is a thing
 * that gets passed where a length was meant. Zero is no socket on both, which is what makes
 * NYA_OS_SOCKET_NONE a zeroed struct.
 * */
struct NYA_OsSocket {
    u64 handle;
};

/** An address and a port, by value. Both families in one struct; see the header. */
struct NYA_OsAddress {
    NYA_OsAddressKind kind;

    /** Network order, as the host's own structures hold them: four bytes for v4, sixteen for v6. */
    u8 bytes[16];

    /** Host order, because every caller writes it as a number. */
    u16 port;

    /** The v6 scope, for a link local address that would otherwise be ambiguous. Zero for everything else. */
    u32 scope;
};

/** One socket to watch, and what it turned out to be ready for. */
struct NYA_OsSocketWait {
    NYA_OsSocket socket;

    /** What the caller wants to hear about. Both may be set; at least one must be. */
    b8 readable;
    b8 writable;

    /** What the wait found. Written by every wait, so a caller need not clear them between calls. */
    b8 is_readable;
    b8 is_writable;

    /**
     * The connection ended or broke while the wait was on it.
     *
     * Reported apart from readability because a socket at end of stream is readable forever: a caller
     * that only checked `is_readable` would spin on a peer that hung up.
     * */
    b8 is_closed;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Starts the host's socket library, once per process, and refers to it until the matching stop.
 *
 * A no-op on Linux and WSAStartup on Windows. Counted rather than idempotent, so two subsystems can
 * each say they need sockets and the second one stopping is what actually stops it.
 * */
NYA_API NYA_OsSocketStatus nya_os_socket_start(void) __attr_no_discard;

/** The pair. Releases the host's socket library when the last caller lets go. */
NYA_API void nya_os_socket_stop(void);

/**
 * The host's own name for this socket — a file descriptor on Linux, a SOCKET on Windows — or -1 for
 * NYA_OS_SOCKET_NONE.
 *
 * For a library that has to do its own reading and writing on a socket this module opened, which today
 * means OpenSSL and nothing else; see tls.h. Not for a caller that only wants to read or write, since
 * every host's differences are what the calls below are for.
 * */
NYA_API s64 nya_os_socket_descriptor(NYA_OsSocket socket) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * OPENING
 * ─────────────────────────────────────────────────────────
 */

/**
 * Opens a socket of `kind`, bound to `port` or to whatever the host picks when `port` is zero.
 *
 * `backlog` is how many connections may be waiting on a listener before the host refuses one, and zero
 * means NYA_OS_SOCKET_BACKLOG; a datagram socket has no such queue and ignores it.
 *
 * Dual stack where the host allows it: the socket is v6 with v4 mapped addresses turned on, so one
 * socket answers both families and a caller never opens two. A host that refuses that falls back to
 * v4, which is the answer nya_os_socket_address gives back. A listener also asks not to wait out the
 * previous process's sockets, so a server that was just restarted binds rather than refusing to.
 * */
NYA_API NYA_OsSocketStatus nya_os_socket_open(NYA_OsSocketKind kind, u16 port, u32 backlog, OUT NYA_OsSocket* out_socket) __attr_no_discard;

/**
 * The same, bound to one address rather than to every interface.
 *
 * What a server that should answer on loopback and nowhere else is opened with, which is the safe
 * default for anything a machine runs for itself. An address of NYA_OS_ADDRESS_NONE means every
 * interface and is exactly what nya_os_socket_open does.
 * */
NYA_API NYA_OsSocketStatus nya_os_socket_open_at(NYA_OsSocketKind kind, NYA_OsAddress address, u32 backlog, OUT NYA_OsSocket* out_socket)
    __attr_no_discard;

/**
 * Takes the next waiting connection, non-blocking.
 *
 * NYA_OS_SOCKET_WOULD_BLOCK when nobody is waiting, which is most of the time. The accepted socket is
 * non-blocking like every other one here, and `out_from` says who it is.
 * */
NYA_API NYA_OsSocketStatus nya_os_socket_accept(NYA_OsSocket listener, OUT NYA_OsSocket* out_socket, OUT NYA_OsAddress* out_from) __attr_no_discard;

/**
 * Starts connecting a stream socket to `address`.
 *
 * Non-blocking, so NYA_OS_SOCKET_WOULD_BLOCK means it is under way rather than that it failed: wait
 * for the socket to be writable and then ask nya_os_socket_error what came of it. NYA_OS_SOCKET_OK
 * means it connected at once, which happens on loopback.
 * */
NYA_API NYA_OsSocketStatus nya_os_socket_connect(NYA_OsAddress address, OUT NYA_OsSocket* out_socket) __attr_no_discard;

/** Closes a socket. A socket that was never opened is a no-op, so a zeroed struct is safe to close. */
NYA_API void nya_os_socket_close(NYA_OsSocket socket);

/*
 * ─────────────────────────────────────────────────────────
 * MOVING BYTES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Sends one datagram to `to`.
 *
 * A datagram is all or nothing: the host takes the whole thing or the call answers
 * NYA_OS_SOCKET_WOULD_BLOCK and nothing was sent. What arrives, and whether it arrives, is the
 * network's business — this says only that the host accepted it.
 * */
NYA_API NYA_OsSocketStatus nya_os_socket_send_to(NYA_OsSocket socket, NYA_OsAddress to, const u8* data, u64 size) __attr_no_discard;

/**
 * Takes the next datagram waiting, into `out_data`.
 *
 * A datagram longer than `capacity` is truncated and the rest is lost, which is the host's behaviour
 * and not worth hiding: a caller whose buffer is smaller than what it agreed to receive has a bug in
 * its own protocol.
 * */
NYA_API NYA_OsSocketStatus nya_os_socket_receive_from(NYA_OsSocket socket, OUT u8* out_data, u64 capacity, OUT u64* out_size, OUT NYA_OsAddress* out_from)
    __attr_no_discard;

/**
 * Writes what the host will take of `data`, which may be none of it and is often not all of it.
 *
 * `out_sent` is how much went; see the header on why the rest is the caller's. NYA_OS_SOCKET_CLOSED
 * when the peer has gone, which on a stream is the one failure that is not worth retrying.
 * */
NYA_API NYA_OsSocketStatus nya_os_socket_send(NYA_OsSocket socket, const u8* data, u64 size, OUT u64* out_sent) __attr_no_discard;

/**
 * Reads what has arrived, up to `capacity`.
 *
 * NYA_OS_SOCKET_CLOSED with `out_read` zero is end of stream: the peer closed its side and nothing
 * more will ever arrive. A read of zero bytes is never reported as NYA_OS_SOCKET_OK, precisely so that
 * a caller cannot mistake the end for a quiet moment.
 * */
NYA_API NYA_OsSocketStatus nya_os_socket_receive(NYA_OsSocket socket, OUT u8* out_data, u64 capacity, OUT u64* out_read) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * WAITING, AND ASKING
 * ─────────────────────────────────────────────────────────
 */

/**
 * Waits until one of `sockets` is ready or `timeout_ms` passes, and fills in what each one turned out
 * to be.
 *
 * `out_ready` is how many have something. Zero with NYA_OS_SOCKET_OK is the timeout, which is not a
 * failure: it is how a server with nothing to do spends its time. A wait over no sockets sleeps for
 * the timeout, so a server whose table is empty does not spin.
 * */
NYA_API NYA_OsSocketStatus nya_os_socket_wait(NYA_OsSocketWait* sockets, u32 count, u32 timeout_ms, OUT u32* out_ready) __attr_no_discard;

/**
 * What a socket's own error slot says, which is where a non-blocking connect's answer ends up.
 *
 * NYA_OS_SOCKET_OK means there is nothing wrong with it. Reading it clears it, as the host does.
 * */
NYA_API NYA_OsSocketStatus nya_os_socket_error(NYA_OsSocket socket) __attr_no_discard;

/** The address a socket is bound to, which is how a caller learns the port the host picked for it. */
NYA_API NYA_OsSocketStatus nya_os_socket_address(NYA_OsSocket socket, OUT NYA_OsAddress* out_address) __attr_no_discard;

/** Turns Nagle's algorithm off, so a small write goes now rather than waiting for company. */
NYA_API NYA_OsSocketStatus nya_os_socket_set_no_delay(NYA_OsSocket socket, b8 no_delay) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * ADDRESSES
 * ─────────────────────────────────────────────────────────
 */

/**
 * Turns a host name or a literal into an address, and may take as long as the host's resolver does.
 *
 * The one call here that blocks, and the reason base/base_socket.h has a thread: a name lookup can
 * take seconds and there is no non-blocking version of it worth having. A literal — "127.0.0.1",
 * "::1" — is answered without asking anybody.
 *
 * `prefer` picks which family to take when a name has both; NYA_OS_ADDRESS_NONE takes whichever the
 * resolver put first, which is what the host's own policy says.
 * */
NYA_API NYA_OsSocketStatus nya_os_address_resolve(NYA_ConstCString host, u16 port, NYA_OsAddressKind prefer, OUT NYA_OsAddress* out_address)
    __attr_no_discard;

/** The address that means "every interface", for a server that binds one. */
NYA_API NYA_OsAddress nya_os_address_any(NYA_OsAddressKind kind, u16 port) __attr_no_discard;

/** Whether two addresses are the same host and port. */
NYA_API b8 nya_os_address_equals(NYA_OsAddress a, NYA_OsAddress b) __attr_no_discard;

/**
 * Whether two addresses are the same host, whatever port each one came from.
 *
 * What a per-host rule is written against: one machine opening a second socket gets a second port, so
 * a limit on connections per address that compared ports would be a limit on nothing.
 * */
NYA_API b8 nya_os_address_equals_host(NYA_OsAddress a, NYA_OsAddress b) __attr_no_discard;

/**
 * Writes `address` as text into `out_text`, which holds NYA_OS_ADDRESS_TEXT_MAX bytes.
 *
 * With the port when `with_port` is set, in the form each family is written in: `1.2.3.4:80` and
 * `[::1]:80`. False when the address is not one, and then `out_text` is an empty string rather than
 * whatever was there.
 * */
NYA_API b8 nya_os_address_text(NYA_OsAddress address, b8 with_port, OUT char* out_text, u64 capacity) __attr_no_discard;
