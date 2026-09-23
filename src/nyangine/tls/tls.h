/**
 * @file tls.h
 *
 * ── the tls module ──
 *
 * TLS on a socket this process already has, so a server can be reached over https without something
 * else in front of it.
 *
 * ```c
 * NYA_TlsContext* tls = nullptr;
 * NYA_TRY(nya_tls_context_create(arena, &tls, .certificate_path = "fullchain.pem", .key_path = "key.pem"));
 * defer nya_tls_context_destroy(tls);
 *
 * // one session per connection, over a socket that is already accepted
 * NYA_TlsSession* session = nullptr;
 * NYA_TRY(nya_tls_session_create(tls, accepted, &session));
 *
 * // and then, every time the socket says it is ready, until it stops saying WANT_ anything
 * switch (nya_tls_handshake(session)) {
 *     case NYA_TLS_OK:         serve(session); break;
 *     case NYA_TLS_WANT_READ:  wait_readable(accepted); break;
 *     case NYA_TLS_WANT_WRITE: wait_writable(accepted); break;
 *     default:                 drop(session); break;
 * }
 * ```
 *
 * ── this is OpenSSL, and it is the system's ──
 *
 * Linked rather than vendored, and linked dynamically: `-lssl -lcrypto` off the machine the program
 * runs on. That is the same trade pgp.h makes with gpg, for the same reason — the alternative is
 * carrying a TLS stack and being the one who ships the fix on the morning a TLS bug is published,
 * where a system library is fixed by the box's own updates before this program is even rebuilt.
 *
 * The cost is honest: a machine with no OpenSSL has no TLS here, and nya_tls_available says so rather
 * than the program failing to start. Today that means Linux; the Windows build reaches TLS through
 * Schannel in curl and has no OpenSSL on its link line, so nya_tls_available is false there until
 * somebody writes the Schannel half.
 *
 * ── what a session is, and what it is not ──
 *
 * It is a wrapper over one accepted socket, which OpenSSL reads and writes itself through the host's
 * own descriptor (nya_os_socket_descriptor). It is not a socket: a caller still owns the socket, still
 * waits on it, and still closes it. This module answers one question — what happened to these bytes —
 * in the four answers a non-blocking socket has.
 *
 * Every call may say WANT_READ or WANT_WRITE, **including a send**, because a TLS record may need a
 * read to make progress on a write and the other way round. A caller that waits for the wrong one
 * hangs, so the answer says which, and NYA_TlsProgress is the whole of the protocol between the two.
 *
 * ── the bounds ──
 *
 * Sessions come from a fixed pool in the context, NYA_TLS_MAX_SESSIONS of them, because everything a
 * peer can cause here is something the server has already bounded: a connection table is fixed, so the
 * sessions over it are too. Past the pool nya_tls_session_create fails, and a server whose connection
 * table is larger than this pool has a configuration error rather than an allocation.
 *
 * Resumption is off — no session cache, no tickets — so every connection is a full handshake and this
 * process keeps no secret that outlives a connection. That is a CPU cost paid for a memory bound and
 * one less key to rotate; a resumption story is later work, and it belongs behind a config flag.
 *
 * Renegotiation is off, compression is off, and the floor is TLS 1.2 with AEAD ciphers only. What a
 * client may speak is the server's list, not the client's preference.
 *
 * ── what this does not do ──
 *
 * It does not get a certificate. A PEM chain and a key are paths this is handed; acme, self-signed
 * generation and reloading on renewal are a program's business, and a server that wants to rotate one
 * builds a second context and swaps it.
 *
 * It does not do client certificates yet, and it advertises `http/1.1` and nothing else through ALPN,
 * which is the honest answer while this server speaks HTTP/1.1 and nothing else.
 *
 * Thread safety: a context may be shared; a session belongs to whichever thread owns its socket.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/os/os_socket.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Sessions one context may hold at once.
 *
 * Sized against NYA_HTTP_MAX_CONNECTIONS rather than guessed: a session exists only for a connection,
 * and a server that raised its connection table raises this with it. Sixty four leaves room for a
 * server built with a larger table without a second define to remember.
 * */
#ifndef NYA_TLS_MAX_SESSIONS
#define NYA_TLS_MAX_SESSIONS 64
#endif

/** Bytes of the last error a session can report, terminator included. */
#define NYA_TLS_MAX_ERROR 192

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef struct NYA_TlsContext NYA_TlsContext;
typedef struct NYA_TlsSession NYA_TlsSession;

/**
 * What happened, in the four answers a non-blocking connection has.
 *
 * WANT_READ and WANT_WRITE are not failures and not partial success: they mean nothing moved and the
 * same call should be made again when the socket says it is ready for *that* direction. See the header
 * on why a send may ask to read.
 * */
typedef enum {
    /** It worked. For a receive or a send, `out_read` or `out_written` says how much. */
    NYA_TLS_OK = 0,

    /** Nothing moved; call again when the socket is readable. */
    NYA_TLS_WANT_READ,

    /** Nothing moved; call again when the socket is writable. */
    NYA_TLS_WANT_WRITE,

    /** The peer closed the connection, cleanly or by going away. Nothing more will move. */
    NYA_TLS_CLOSED,

    /** The connection is broken. nya_tls_error says what OpenSSL called it. */
    NYA_TLS_FAILED,
} NYA_TlsProgress;

/** What a context is made of. Both paths are PEM and both are required. */
typedef struct {
    /** The certificate chain: the leaf first, then whatever signs it. */
    NYA_ConstCString certificate_path;

    /** The private key for the leaf. Read once, at creation, and never held as bytes here. */
    NYA_ConstCString key_path;
} NYA_TlsContextOptions;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Whether this build has a TLS library at all. False on a build with no OpenSSL; see the header. */
NYA_API b8 nya_tls_available(void) __attr_no_discard;

/** What library and version this is, for a startup line. "no TLS" when there is none. */
NYA_API NYA_ConstCString nya_tls_version(void) __attr_no_discard;

/**
 * Reads the certificate and the key and builds what every session on them shares.
 *
 * Fails, naming the file, when either path is missing, unreadable, not PEM, or when the key is not the
 * one the certificate carries — the last of which is worth its own check, because a mismatched pair is
 * a server that starts and then fails every handshake.
 *
 * `arena` owns the context's own memory. OpenSSL's is OpenSSL's, and nya_tls_context_destroy is what
 * gives it back.
 * */
NYA_API NYA_Error _nya_tls_context_create(NYA_Arena* arena, OUT NYA_TlsContext** out_context, NYA_TlsContextOptions options) __attr_no_discard;

/** Takes the options by name; see base_basic.h on why every constructor here is a macro over a struct. */
#define nya_tls_context_create(arena, out_context, ...) _nya_tls_context_create((arena), (out_context), (NYA_TlsContextOptions){ __VA_ARGS__ })

/** Releases the context and every session still on it. Safe on null, and idempotent. */
NYA_API void nya_tls_context_destroy(NYA_TlsContext* context);

/**
 * Starts a TLS session over an accepted socket.
 *
 * Nothing is read or written here: the handshake is nya_tls_handshake's, so a caller never blocks
 * inside an accept. The socket stays the caller's to wait on and to close.
 *
 * NYA_ERROR_OUT_OF_MEMORY when the pool is full, which is a bound rather than a failure; see the header.
 * */
NYA_API NYA_Error nya_tls_session_create(NYA_TlsContext* context, NYA_OsSocket socket, OUT NYA_TlsSession** out_session) __attr_no_discard;

/**
 * Gives the session's slot back. Does not close the socket, which the caller opened and still owns.
 *
 * Sends no close_notify: that is nya_tls_shutdown, and it is the caller's choice whether a connection
 * being dropped is worth the round trip. Safe on null.
 * */
NYA_API void nya_tls_session_destroy(NYA_TlsSession* session);

/**
 * Moves the handshake along, and answers what it needs next.
 *
 * Call it whenever the socket is ready for what the last answer asked for, until it says OK. Calling it
 * on an established session is OK and does nothing.
 * */
NYA_API NYA_TlsProgress nya_tls_handshake(NYA_TlsSession* session) __attr_no_discard;

/** Whether the handshake is done, which is the one thing a caller may not ask the socket. */
NYA_API b8 nya_tls_is_established(const NYA_TlsSession* session) __attr_no_discard;

/**
 * Reads up to `capacity` decrypted bytes.
 *
 * OK with `out_read` at zero never happens: no bytes is WANT_READ, and end of stream is CLOSED, so a
 * caller never has to guess which nothing it got.
 * */
NYA_API NYA_TlsProgress nya_tls_receive(NYA_TlsSession* session, OUT u8* out_data, u64 capacity, OUT u64* out_read) __attr_no_discard;

/**
 * Writes up to `size` bytes, and says how many went.
 *
 * A short write is OK with `out_written` below `size`: the rest is the caller's to send again, exactly
 * as a socket would have it. What may *not* be done is to send different bytes next time — see
 * NYA_TLS_WANT_WRITE, where nothing moved at all and the same buffer has to come back.
 * */
NYA_API NYA_TlsProgress nya_tls_send(NYA_TlsSession* session, const u8* data, u64 size, OUT u64* out_written) __attr_no_discard;

/**
 * Sends close_notify, so the peer can tell a finished connection from a cut one.
 *
 * Worth doing when a response is complete and worth skipping when a peer is being dropped for
 * misbehaving. May want another read or write, like everything else here.
 * */
NYA_API NYA_TlsProgress nya_tls_shutdown(NYA_TlsSession* session) __attr_no_discard;

/** What went wrong, as OpenSSL describes it. Empty when nothing has. */
NYA_API NYA_ConstCString nya_tls_error(const NYA_TlsSession* session) __attr_no_discard;

/** Which protocol was agreed: "TLSv1.3", "TLSv1.2", or empty before the handshake finishes. */
NYA_API NYA_ConstCString nya_tls_protocol(const NYA_TlsSession* session) __attr_no_discard;

/** Which cipher was agreed, in OpenSSL's own name for it. Empty before the handshake finishes. */
NYA_API NYA_ConstCString nya_tls_cipher(const NYA_TlsSession* session) __attr_no_discard;

/** How many sessions the pool is holding, for the ceiling audit. */
NYA_API u32 nya_tls_session_count(const NYA_TlsContext* context) __attr_no_discard;
