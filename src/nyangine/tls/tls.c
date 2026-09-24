#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_logging.h"
#include "nyangine/tls/tls.h"

#ifdef NYA_MODULE_TLS

#include <openssl/err.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

struct NYA_TlsSession {
    SSL* ssl;

    /** Which context owns the slot, and whether the slot is in use at all. */
    NYA_TlsContext* context;
    b8              live;

    b8 established;

    char error[NYA_TLS_MAX_ERROR];
};

struct NYA_TlsContext {
    SSL_CTX* ssl_context;

    /** Whether this is the connecting, verifying side. Decides SSL_connect against SSL_accept in the handshake. */
    b8 client;

    /** The pool, for the reason tls.h gives: a session exists only for a connection, and those are bounded. */
    NYA_TlsSession sessions[NYA_TLS_MAX_SESSIONS];
    u32            session_count;
};

/*
 * What a client may speak on TLS 1.2. Forward secrecy and AEAD only: every ECDHE suite with GCM or
 * ChaCha20-Poly1305 behind it, and nothing with CBC, RSA key exchange or a MAC-then-encrypt
 * construction in it. TLS 1.3's suites are not named here because 1.3 does not let them be turned off
 * one by one, and all three of them are already this list's shape.
 */
#define _NYA_TLS_CIPHERS_1_2 "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305"

/** What this server speaks, as ALPN writes it: one byte of length, then the name. */
NYA_INTERNAL const u8 _NYA_TLS_ALPN[] = { 8, 'h', 't', 't', 'p', '/', '1', '.', '1' };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Builds the connecting, verifying context: TLS_client_method, a trust store, and peer verification on. */
NYA_INTERNAL NYA_Error _nya_tls_client_context_create(NYA_Arena* arena, OUT NYA_TlsContext** out_context, NYA_TlsContextOptions options) __attr_no_discard;

/** Turns whatever OpenSSL last said into `session->error`, and answers NYA_TLS_FAILED so a caller can return it. */
NYA_INTERNAL NYA_TlsProgress _nya_tls_fail(NYA_TlsSession* session, NYA_ConstCString what) __attr_no_discard;

/** What one SSL_* return means, given what the session asked OpenSSL to do. */
NYA_INTERNAL NYA_TlsProgress _nya_tls_progress(NYA_TlsSession* session, s32 result, NYA_ConstCString what) __attr_no_discard;

/** Whatever is on OpenSSL's error queue, as one line, with the queue left empty. */
NYA_INTERNAL void _nya_tls_last_error(OUT char* out_text, u64 capacity);

/** The ALPN callback: this server speaks http/1.1, and a client that offers nothing else is refused. */
NYA_INTERNAL s32 _nya_tls_alpn_select(
    SSL* ssl, const u8** out_selected, u8* out_size, const u8* offered, u32 offered_size, void* user_data
);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 nya_tls_available(void) {
    return true;
}

NYA_ConstCString nya_tls_version(void) {
    return OPENSSL_VERSION_TEXT;
}

NYA_Error _nya_tls_context_create(NYA_Arena* arena, NYA_TlsContext** out_context, NYA_TlsContextOptions options) {
    nya_assert(arena != nullptr && out_context != nullptr);

    *out_context = nullptr;

    // The connecting side is a different context: it verifies rather than presents, and carries no key.
    if (options.client) return _nya_tls_client_context_create(arena, out_context, options);

    if (options.certificate_path == nullptr || options.certificate_path[0] == '\0') {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a TLS context needs a certificate");
    }

    if (options.key_path == nullptr || options.key_path[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a TLS context needs a key");

    SSL_CTX* ssl_context = SSL_CTX_new(TLS_server_method());
    if (ssl_context == nullptr) {
        char text[NYA_TLS_MAX_ERROR] = { 0 };
        _nya_tls_last_error(text, sizeof(text));

        return nya_error(NYA_ERROR_NOT_OK, "the TLS context could not be made: %s", text);
    }

    /*
     * The floor is 1.2. Everything below it is broken in public — SSLv3 by POODLE, 1.0 and 1.1 by
     * their MAC-then-encrypt construction and SHA-1 signatures — and a server that offers a broken
     * version offers it to whoever asks for it first.
     */
    if (SSL_CTX_set_min_proto_version(ssl_context, TLS1_2_VERSION) != 1) {
        SSL_CTX_free(ssl_context);
        return nya_error(NYA_ERROR_NOT_OK, "TLS 1.2 could not be set as the floor");
    }

    /*
     * Renegotiation is a request a client can make over and over, each one costing a handshake: it is
     * the oldest denial of service TLS has and 1.3 removed it outright. Compression is CRIME.
     * Server preference means the cipher is this list's order and not the client's.
     */
    (void)SSL_CTX_set_options(
        ssl_context,
        SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION | SSL_OP_CIPHER_SERVER_PREFERENCE | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1
    );

    if (SSL_CTX_set_cipher_list(ssl_context, _NYA_TLS_CIPHERS_1_2) != 1) {
        SSL_CTX_free(ssl_context);
        return nya_error(NYA_ERROR_NOT_OK, "no cipher this build allows is available");
    }

    /*
     * No resumption, for the reason tls.h gives: a session cache is memory a peer decides the size of,
     * and a ticket key is a secret that outlives a connection and has to be rotated by somebody.
     */
    SSL_CTX_set_session_cache_mode(ssl_context, SSL_SESS_CACHE_OFF);
    (void)SSL_CTX_set_options(ssl_context, SSL_OP_NO_TICKET);

    /*
     * Partial writes and a moving buffer are what make nya_tls_send honest about short writes, and
     * releasing buffers gives a quiet connection's 32 KiB back rather than holding it per connection.
     */
    (void)SSL_CTX_set_mode(ssl_context, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_RELEASE_BUFFERS);

    SSL_CTX_set_alpn_select_cb(ssl_context, _nya_tls_alpn_select, nullptr);

    if (SSL_CTX_use_certificate_chain_file(ssl_context, options.certificate_path) != 1) {
        char text[NYA_TLS_MAX_ERROR] = { 0 };
        _nya_tls_last_error(text, sizeof(text));

        SSL_CTX_free(ssl_context);
        return nya_error(NYA_ERROR_NOT_OK, "'%s' is not a certificate chain: %s", options.certificate_path, text);
    }

    if (SSL_CTX_use_PrivateKey_file(ssl_context, options.key_path, SSL_FILETYPE_PEM) != 1) {
        char text[NYA_TLS_MAX_ERROR] = { 0 };
        _nya_tls_last_error(text, sizeof(text));

        SSL_CTX_free(ssl_context);
        return nya_error(NYA_ERROR_NOT_OK, "'%s' is not a private key: %s", options.key_path, text);
    }

    // Worth its own check: a certificate and a key that are not a pair is a server that starts and then
    // fails every handshake, which is a fault that only shows up on somebody else's browser.
    if (SSL_CTX_check_private_key(ssl_context) != 1) {
        SSL_CTX_free(ssl_context);
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is not the key for '%s'", options.key_path, options.certificate_path);
    }

    NYA_TlsContext* context = nya_arena_alloc(arena, sizeof(NYA_TlsContext));

    nya_memset(context, 0, sizeof(*context));
    context->ssl_context = ssl_context;

    *out_context = context;

    return NYA_OK;
}

void nya_tls_context_destroy(NYA_TlsContext* context) {
    if (context == nullptr) return;

    for (u32 index = 0; index < NYA_TLS_MAX_SESSIONS; index++) {
        if (context->sessions[index].live) nya_tls_session_destroy(&context->sessions[index]);
    }

    if (context->ssl_context != nullptr) SSL_CTX_free(context->ssl_context);

    context->ssl_context = nullptr;
}

NYA_Error nya_tls_session_create(NYA_TlsContext* context, NYA_OsSocket socket, NYA_TlsSession** out_session) {
    nya_assert(context != nullptr && out_session != nullptr);

    *out_session = nullptr;

    s64 descriptor = nya_os_socket_descriptor(socket);
    if (descriptor < 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is not a socket");

    NYA_TlsSession* session = nullptr;
    for (u32 index = 0; index < NYA_TLS_MAX_SESSIONS; index++) {
        if (context->sessions[index].live) continue;

        session = &context->sessions[index];
        break;
    }

    if (session == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "every one of the %d TLS sessions is in use", NYA_TLS_MAX_SESSIONS);

    SSL* ssl = SSL_new(context->ssl_context);
    if (ssl == nullptr) return nya_error(NYA_ERROR_NOT_OK, "a TLS session could not be made");

    if (SSL_set_fd(ssl, (s32)descriptor) != 1) {
        SSL_free(ssl);
        return nya_error(NYA_ERROR_NOT_OK, "the socket could not be given to TLS");
    }

    // The server side of the handshake, and nothing of it runs until nya_tls_handshake is called.
    SSL_set_accept_state(ssl);

    nya_memset(session, 0, sizeof(*session));

    session->ssl     = ssl;
    session->context = context;
    session->live    = true;

    context->session_count++;

    *out_session = session;

    return NYA_OK;
}

NYA_Error nya_tls_session_connect(NYA_TlsContext* context, NYA_OsSocket socket, NYA_ConstCString host, NYA_TlsSession** out_session) {
    nya_assert(context != nullptr && out_session != nullptr);

    *out_session = nullptr;

    if (!context->client) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a client session needs a client context");
    if (host == nullptr || host[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a client session needs a host to verify against");

    s64 descriptor = nya_os_socket_descriptor(socket);
    if (descriptor < 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "that is not a socket");

    NYA_TlsSession* session = nullptr;
    for (u32 index = 0; index < NYA_TLS_MAX_SESSIONS; index++) {
        if (context->sessions[index].live) continue;

        session = &context->sessions[index];
        break;
    }

    if (session == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "every one of the %d TLS sessions is in use", NYA_TLS_MAX_SESSIONS);

    SSL* ssl = SSL_new(context->ssl_context);
    if (ssl == nullptr) return nya_error(NYA_ERROR_NOT_OK, "a TLS session could not be made");

    if (SSL_set_fd(ssl, (s32)descriptor) != 1) {
        SSL_free(ssl);
        return nya_error(NYA_ERROR_NOT_OK, "the socket could not be given to TLS");
    }

    // SNI: which name this is asking for, so a host serving many gets it right. A name, not a literal —
    // OpenSSL rejects an IP address here, which is correct, since SNI is a host name extension.
    if (SSL_set_tlsext_host_name(ssl, host) != 1) {
        SSL_free(ssl);
        return nya_error(NYA_ERROR_NOT_OK, "the server name could not be set for '%s'", host);
    }

    /*
     * The one line that makes this verify anything: SSL_set1_host tells OpenSSL the name the certificate
     * must be for, and with SSL_VERIFY_PEER already on the context a certificate for another host, or one
     * no trusted authority signed, fails the handshake rather than being quietly accepted. Without it,
     * verification would check the chain but not that the chain is *this server's*.
     */
    if (SSL_set1_host(ssl, host) != 1) {
        SSL_free(ssl);
        return nya_error(NYA_ERROR_NOT_OK, "the host to verify could not be set to '%s'", host);
    }

    // The client side of the handshake, and nothing of it runs until nya_tls_handshake is called.
    SSL_set_connect_state(ssl);

    nya_memset(session, 0, sizeof(*session));

    session->ssl     = ssl;
    session->context = context;
    session->live    = true;

    context->session_count++;

    *out_session = session;

    return NYA_OK;
}

void nya_tls_session_destroy(NYA_TlsSession* session) {
    if (session == nullptr || !session->live) return;

    if (session->ssl != nullptr) SSL_free(session->ssl);

    NYA_TlsContext* context = session->context;

    nya_memset(session, 0, sizeof(*session));

    nya_assert(context->session_count > 0, "a TLS session was released that was never counted");
    context->session_count--;
}

NYA_TlsProgress nya_tls_handshake(NYA_TlsSession* session) {
    nya_assert(session != nullptr);

    if (!session->live) return NYA_TLS_FAILED;
    if (session->established) return NYA_TLS_OK;

    // The connecting side drives the handshake with SSL_connect, the accepting side with SSL_accept; the
    // state was set to match at session creation, so this only picks which name to call it by.
    s32 result = session->context->client ? SSL_connect(session->ssl) : SSL_accept(session->ssl);

    if (result == 1) {
        session->established = true;
        return NYA_TLS_OK;
    }

    return _nya_tls_progress(session, result, "the handshake");
}

b8 nya_tls_is_established(const NYA_TlsSession* session) {
    nya_assert(session != nullptr);

    return session->live && session->established;
}

NYA_TlsProgress nya_tls_receive(NYA_TlsSession* session, u8* out_data, u64 capacity, u64* out_read) {
    nya_assert(session != nullptr && out_data != nullptr && out_read != nullptr);

    *out_read = 0;

    if (!session->live) return NYA_TLS_FAILED;
    if (capacity == 0) return NYA_TLS_OK;

    // The handshake first, always: a caller that reads before it is done would otherwise have to know
    // the difference between a handshake byte and a payload one, which is the whole job of this module.
    if (!session->established) {
        NYA_TlsProgress handshake = nya_tls_handshake(session);
        if (handshake != NYA_TLS_OK) return handshake;
    }

    // Bounded to what one read can say it returned, which is an int in OpenSSL's own API.
    u64 wanted = capacity > S32_MAX ? (u64)S32_MAX : capacity;

    s32 result = SSL_read(session->ssl, out_data, (s32)wanted);

    if (result > 0) {
        *out_read = (u64)result;
        return NYA_TLS_OK;
    }

    return _nya_tls_progress(session, result, "a read");
}

NYA_TlsProgress nya_tls_send(NYA_TlsSession* session, const u8* data, u64 size, u64* out_written) {
    nya_assert(session != nullptr && data != nullptr && out_written != nullptr);

    *out_written = 0;

    if (!session->live) return NYA_TLS_FAILED;
    if (size == 0) return NYA_TLS_OK;

    if (!session->established) {
        NYA_TlsProgress handshake = nya_tls_handshake(session);
        if (handshake != NYA_TLS_OK) return handshake;
    }

    u64 wanted = size > S32_MAX ? (u64)S32_MAX : size;

    s32 result = SSL_write(session->ssl, data, (s32)wanted);

    if (result > 0) {
        *out_written = (u64)result;
        return NYA_TLS_OK;
    }

    return _nya_tls_progress(session, result, "a write");
}

NYA_TlsProgress nya_tls_shutdown(NYA_TlsSession* session) {
    nya_assert(session != nullptr);

    if (!session->live) return NYA_TLS_FAILED;
    if (!session->established) return NYA_TLS_OK;

    s32 result = SSL_shutdown(session->ssl);

    // Zero means our close_notify went and the peer's has not come back. That is enough: this server
    // does not wait for a peer to agree that a finished connection is finished.
    if (result >= 0) return NYA_TLS_OK;

    return _nya_tls_progress(session, result, "the shutdown");
}

NYA_ConstCString nya_tls_error(const NYA_TlsSession* session) {
    nya_assert(session != nullptr);

    return session->error;
}

NYA_ConstCString nya_tls_protocol(const NYA_TlsSession* session) {
    nya_assert(session != nullptr);

    if (!session->live || !session->established) return "";

    return SSL_get_version(session->ssl);
}

NYA_ConstCString nya_tls_cipher(const NYA_TlsSession* session) {
    nya_assert(session != nullptr);

    if (!session->live || !session->established) return "";

    const SSL_CIPHER* cipher = SSL_get_current_cipher(session->ssl);
    if (cipher == nullptr) return "";

    return SSL_CIPHER_get_name(cipher);
}

u32 nya_tls_session_count(const NYA_TlsContext* context) {
    nya_assert(context != nullptr);

    return context->session_count;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error _nya_tls_client_context_create(NYA_Arena* arena, NYA_TlsContext** out_context, NYA_TlsContextOptions options) {
    SSL_CTX* ssl_context = SSL_CTX_new(TLS_client_method());
    if (ssl_context == nullptr) {
        char text[NYA_TLS_MAX_ERROR] = { 0 };
        _nya_tls_last_error(text, sizeof(text));

        return nya_error(NYA_ERROR_NOT_OK, "the TLS client context could not be made: %s", text);
    }

    // The same floor and the same suite shape as the server side: 1.2 or 1.3, forward secrecy and AEAD.
    if (SSL_CTX_set_min_proto_version(ssl_context, TLS1_2_VERSION) != 1) {
        SSL_CTX_free(ssl_context);
        return nya_error(NYA_ERROR_NOT_OK, "TLS 1.2 could not be set as the floor");
    }

    (void)SSL_CTX_set_options(ssl_context, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);

    if (SSL_CTX_set_cipher_list(ssl_context, _NYA_TLS_CIPHERS_1_2) != 1) {
        SSL_CTX_free(ssl_context);
        return nya_error(NYA_ERROR_NOT_OK, "no cipher this build allows is available");
    }

    SSL_CTX_set_session_cache_mode(ssl_context, SSL_SESS_CACHE_OFF);
    (void)SSL_CTX_set_options(ssl_context, SSL_OP_NO_TICKET);
    (void)SSL_CTX_set_mode(ssl_context, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_RELEASE_BUFFERS);

    /*
     * The trust store, either a bundle the caller named — a private authority for an in-house server —
     * or the system's own, which is what a public submission server is signed under. A load that finds
     * nothing to trust is a context that would reject every certificate, so it is a failure here rather
     * than a surprise at the first handshake.
     */
    if (options.ca_path != nullptr && options.ca_path[0] != '\0') {
        if (SSL_CTX_load_verify_locations(ssl_context, options.ca_path, nullptr) != 1) {
            char text[NYA_TLS_MAX_ERROR] = { 0 };
            _nya_tls_last_error(text, sizeof(text));

            SSL_CTX_free(ssl_context);
            return nya_error(NYA_ERROR_NOT_OK, "'%s' is not a certificate authority bundle: %s", options.ca_path, text);
        }
    } else if (SSL_CTX_set_default_verify_paths(ssl_context) != 1) {
        SSL_CTX_free(ssl_context);
        return nya_error(NYA_ERROR_NOT_OK, "the system's trust store could not be loaded");
    }

    // Verify the peer, and fail the handshake when it does not verify. The host to match is per-session,
    // set in nya_tls_session_connect; this only says that verification happens at all.
    SSL_CTX_set_verify(ssl_context, SSL_VERIFY_PEER, nullptr);

    NYA_TlsContext* context = nya_arena_alloc(arena, sizeof(NYA_TlsContext));

    nya_memset(context, 0, sizeof(*context));
    context->ssl_context = ssl_context;
    context->client      = true;

    *out_context = context;

    return NYA_OK;
}

NYA_TlsProgress _nya_tls_progress(NYA_TlsSession* session, s32 result, NYA_ConstCString what) {
    s32 reason = SSL_get_error(session->ssl, result);

    switch (reason) {
        case SSL_ERROR_WANT_READ:  return NYA_TLS_WANT_READ;
        case SSL_ERROR_WANT_WRITE: return NYA_TLS_WANT_WRITE;

        // The peer said close_notify, which is the only clean end a TLS connection has.
        case SSL_ERROR_ZERO_RETURN: return NYA_TLS_CLOSED;

        case SSL_ERROR_SYSCALL:
            /*
             * End of file with nothing on the error queue: the peer went away without a close_notify.
             * Common enough from a browser closing a tab that it is not worth calling a failure, and
             * indistinguishable from a truncation attack only on a protocol that needs length framing —
             * HTTP has Content-Length, so this server already knows whether it got everything.
             */
            if (ERR_peek_error() == 0) return NYA_TLS_CLOSED;

            return _nya_tls_fail(session, what);

        case SSL_ERROR_SSL:
            /*
             * The same peer-went-away-without-close_notify as the syscall case above, moved onto the
             * error queue as its own reason in OpenSSL 3.0: a TCP FIN before the close_notify. It is the
             * clean end this server itself produces — nya_tls_shutdown is deliberately not sent when a
             * connection is dropped, see tls.h — so its own client has to read it as a close rather than
             * a break, for the length-framing reason the syscall case gives. Any other SSL error is real.
             */
            if (ERR_GET_REASON(ERR_peek_error()) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
                ERR_clear_error();
                return NYA_TLS_CLOSED;
            }

            return _nya_tls_fail(session, what);

        default: return _nya_tls_fail(session, what);
    }
}

NYA_TlsProgress _nya_tls_fail(NYA_TlsSession* session, NYA_ConstCString what) {
    char text[NYA_TLS_MAX_ERROR] = { 0 };
    _nya_tls_last_error(text, sizeof(text));

    (void)snprintf(session->error, sizeof(session->error), "%s failed: %s", what, text);

    return NYA_TLS_FAILED;
}

void _nya_tls_last_error(char* out_text, u64 capacity) {
    nya_assert(out_text != nullptr && capacity > 0);

    out_text[0] = '\0';

    u64 error = ERR_get_error();

    if (error == 0) {
        (void)snprintf(out_text, capacity, "no reason given");
        return;
    }

    ERR_error_string_n(error, out_text, capacity);

    // The queue holds every reason, and a caller only ever shows one. The rest are dropped here so the
    // next failure cannot report this one's.
    ERR_clear_error();
}

s32 _nya_tls_alpn_select(SSL* ssl, const u8** out_selected, u8* out_size, const u8* offered, u32 offered_size, void* user_data) {
    (void)ssl;
    (void)user_data;

    /*
     * OpenSSL writes the chosen protocol through a `u8**` and hands it back to the caller as a
     * `const u8**`, so one of the two has to lose its const. It is written here and read there, and
     * what it points at is this file's own static table.
     */
    u8* selected = nullptr;

    s32 chosen = SSL_select_next_proto(&selected, out_size, _NYA_TLS_ALPN, (u32)sizeof(_NYA_TLS_ALPN), offered, offered_size);

    *out_selected = selected;

    // A client that offered a list with nothing this server speaks in it gets no ALPN rather than a
    // guess: OpenSSL's "no overlap" answer names the client's first choice, which this cannot serve.
    return chosen == OPENSSL_NPN_NEGOTIATED ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_ALERT_FATAL;
}

#else

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * WITHOUT A TLS LIBRARY
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Every call answers rather than failing to link, so a program can ask whether TLS is here and carry on
 * without it — which is what a Windows build does today; see tls.h.
 */

struct NYA_TlsContext {
    u32 session_count;
};

struct NYA_TlsSession {
    char error[NYA_TLS_MAX_ERROR];
};

b8 nya_tls_available(void) {
    return false;
}

NYA_ConstCString nya_tls_version(void) {
    return "no TLS";
}

NYA_Error _nya_tls_context_create(NYA_Arena* arena, NYA_TlsContext** out_context, NYA_TlsContextOptions options) {
    (void)arena;
    (void)options;

    *out_context = nullptr;

    return nya_error(NYA_ERROR_NOT_SUPPORTED, "this build has no TLS library");
}

void nya_tls_context_destroy(NYA_TlsContext* context) {
    (void)context;
}

NYA_Error nya_tls_session_create(NYA_TlsContext* context, NYA_OsSocket socket, NYA_TlsSession** out_session) {
    (void)context;
    (void)socket;

    *out_session = nullptr;

    return nya_error(NYA_ERROR_NOT_SUPPORTED, "this build has no TLS library");
}

NYA_Error nya_tls_session_connect(NYA_TlsContext* context, NYA_OsSocket socket, NYA_ConstCString host, NYA_TlsSession** out_session) {
    (void)context;
    (void)socket;
    (void)host;

    *out_session = nullptr;

    return nya_error(NYA_ERROR_NOT_SUPPORTED, "this build has no TLS library");
}

void nya_tls_session_destroy(NYA_TlsSession* session) {
    (void)session;
}

NYA_TlsProgress nya_tls_handshake(NYA_TlsSession* session) {
    (void)session;

    return NYA_TLS_FAILED;
}

b8 nya_tls_is_established(const NYA_TlsSession* session) {
    (void)session;

    return false;
}

NYA_TlsProgress nya_tls_receive(NYA_TlsSession* session, u8* out_data, u64 capacity, u64* out_read) {
    (void)session;
    (void)out_data;
    (void)capacity;

    *out_read = 0;

    return NYA_TLS_FAILED;
}

NYA_TlsProgress nya_tls_send(NYA_TlsSession* session, const u8* data, u64 size, u64* out_written) {
    (void)session;
    (void)data;
    (void)size;

    *out_written = 0;

    return NYA_TLS_FAILED;
}

NYA_TlsProgress nya_tls_shutdown(NYA_TlsSession* session) {
    (void)session;

    return NYA_TLS_FAILED;
}

NYA_ConstCString nya_tls_error(const NYA_TlsSession* session) {
    (void)session;

    return "this build has no TLS library";
}

NYA_ConstCString nya_tls_protocol(const NYA_TlsSession* session) {
    (void)session;

    return "";
}

NYA_ConstCString nya_tls_cipher(const NYA_TlsSession* session) {
    (void)session;

    return "";
}

u32 nya_tls_session_count(const NYA_TlsContext* context) {
    (void)context;

    return 0;
}

#endif // NYA_MODULE_TLS
