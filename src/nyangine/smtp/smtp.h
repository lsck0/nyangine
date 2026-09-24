/**
 * @file smtp.h
 *
 * ── the smtp module ──
 *
 * Sending one mail to a submission server: the message a one-binary webapp needs for a verification or
 * a password-reset link, and nothing a mail *server* does. There is no queue, no retry and no receiving
 * — a program that wants those wants a different thing than this.
 *
 * ```c
 * NYA_SmtpConfig config = {
 *     .host     = "smtp.example.com",
 *     .port     = 587,
 *     .security = NYA_SMTP_SECURITY_STARTTLS,
 *     .auth     = NYA_SMTP_AUTH_LOGIN,
 *     .username = "postmaster@example.com",
 *     .password = secret,                       // read from the environment, never a literal
 * };
 * NYA_SmtpMessage message = {
 *     .from    = "noreply@example.com",
 *     .to      = "person@elsewhere.com",
 *     .subject = "Confirm your address",
 *     .text    = "Open this link to confirm: https://…",
 * };
 * NYA_TRY(nya_smtp_send(arena, &config, &message));
 * ```
 *
 * ── the transport, and why the conversation is its own thing ──
 *
 * A send opens a TCP socket to the submission server, wraps it in TLS, and runs the SMTP conversation
 * over it. The socket is os_socket's and the TLS is the tls module's — this module owns neither and
 * reuses both, so there is one place a certificate is verified and one place a cipher list is chosen,
 * and it is not here. See tls.h.
 *
 * The conversation — greeting, EHLO, STARTTLS, AUTH, MAIL FROM, RCPT TO, DATA — is written against a
 * byte channel rather than against a socket, so the state machine can be driven over an in-memory
 * script with no network at all. That is how the tests check the command sequence deterministically;
 * the real channel is the socket-and-TLS one a send builds for itself.
 *
 * ── TLS is not optional ──
 *
 * Both transports encrypt. NYA_SMTP_SECURITY_IMPLICIT is TLS from the first byte, the port-465 form;
 * NYA_SMTP_SECURITY_STARTTLS connects in the clear and upgrades before a single credential is sent, the
 * port-587 form. Either way the certificate is verified against the configured host, and either way a
 * build with no TLS library refuses the send rather than sending a password in the clear. There is no
 * plaintext mode, because a password crossing a plaintext SMTP link is a password published.
 *
 * ── what it refuses ──
 *
 * A caller-supplied header value — a subject, a recipient, a display name — with a carriage return or a
 * line feed in it is refused, because a newline in a header value is how an attacker writes headers of
 * their own: a second `To`, a `Bcc`, a whole injected body. The credentials never reach a log, an error
 * message or a backtrace. The assembled message is bounded, and a message past the bound is refused
 * rather than sent in pieces.
 *
 * Thread safety: nothing here holds shared state; a send owns everything it touches for the length of
 * the call, and two threads may each send at once.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

/**
 * The largest assembled message this will send, headers, MIME framing and base64 body included.
 *
 * A verification mail is a few hundred bytes; a megabyte is already far past anything a transactional
 * mail has a reason to be, and the point of the bound is that the size is decided here and not by
 * whatever a caller happened to pass as a body.
 * */
#ifndef NYA_SMTP_MAX_MESSAGE
#define NYA_SMTP_MAX_MESSAGE (1024ULL * 1024ULL)
#endif

/** The most bytes one server reply may run to before the conversation gives up on it. */
#define NYA_SMTP_MAX_REPLY 8192

/** How long the whole exchange may take when a config does not say, in milliseconds. */
#define NYA_SMTP_DEFAULT_TIMEOUT_MS 30000

// TYPES

/** How the connection is protected. Both encrypt; see the header on why there is no third option. */
typedef enum {
    /** Connect in the clear on the submission port, then STARTTLS before anything secret is said. */
    NYA_SMTP_SECURITY_STARTTLS = 0,

    /** TLS from the first byte, the port-465 form. */
    NYA_SMTP_SECURITY_IMPLICIT,
} NYA_SmtpSecurity;

/** How the client proves who it is, once the link is encrypted. */
typedef enum {
    /** No login at all, for a server that decides by network rather than by credential. */
    NYA_SMTP_AUTH_NONE = 0,

    /** AUTH LOGIN: the username and the password base64'd in turn, each after a `334` prompt. */
    NYA_SMTP_AUTH_LOGIN,

    /** AUTH PLAIN: one base64 token carrying the username and the password. */
    NYA_SMTP_AUTH_PLAIN,
} NYA_SmtpAuth;

/**
 * Where to send, how to protect it, and how to log in.
 *
 * `username` and `password` are secrets: this module never writes either to a log, an error message or
 * a backtrace, and a caller should read them from the environment rather than compile them in.
 * */
typedef struct {
    /** The submission server's host name. Also the name the TLS certificate is verified against. */
    NYA_ConstCString host;

    /** The port: 587 for STARTTLS submission, 465 for implicit TLS, 25 for a relay that allows it. */
    u16 port;

    NYA_SmtpSecurity security;
    NYA_SmtpAuth     auth;

    /** The login name. A secret; never logged. */
    NYA_ConstCString username;

    /** The password. A secret; never logged, and wiped from the AUTH scratch once it has been sent. */
    NYA_ConstCString password;

    /**
     * A PEM bundle of trust anchors to verify the server against, or null to use the system's store.
     * A private submission server with its own certificate authority is what this is for.
     * */
    NYA_ConstCString ca_path;

    /** How long the whole exchange may take, or zero for NYA_SMTP_DEFAULT_TIMEOUT_MS. */
    u32 timeout_ms;
} NYA_SmtpConfig;

/**
 * One message: who it is from, who it is to, and what it says.
 *
 * Every field a caller fills that becomes a header — `from`, `from_name`, `to`, `subject` — is refused
 * if it carries a carriage return or a line feed; see the header. `text` is required and `html` is an
 * optional richer alternative the receiving client shows when it can.
 * */
typedef struct {
    /** The sender's address, `local@domain`, no display name and no angle brackets. */
    NYA_ConstCString from;

    /** An optional display name shown beside the address, `"Ada Lovelace" <ada@…>`. May be null. */
    NYA_ConstCString from_name;

    /** The one recipient's address, `local@domain`. */
    NYA_ConstCString to;

    /** The subject line. */
    NYA_ConstCString subject;

    /** The plain-text body, required. */
    NYA_ConstCString text;

    /** An optional HTML body, carried beside the text as a multipart/alternative. May be null. */
    NYA_ConstCString html;
} NYA_SmtpMessage;

// FUNCTIONS

/**
 * Builds the RFC 5322 message `nya_smtp_send` would put in the DATA phase, into `out_message`.
 *
 * The headers are From, To, Subject, Date and Message-ID, with MIME-Version and the content headers a
 * body needs; a message with `html` set is a multipart/alternative carrying the text and the HTML, and
 * one without is a single text/plain part. Both bodies are base64 encoded, which keeps every line short
 * and free of the bytes a DATA phase would otherwise have to escape.
 *
 * Returns NYA_ERROR_INVALID_ARGUMENT when a header value carries a CR or an LF, or when a required field
 * is missing, and NYA_ERROR_OUT_OF_MEMORY when the result would pass NYA_SMTP_MAX_MESSAGE. On a failure
 * `out_message` is left empty. Split out from the send so a test can read the bytes without a socket,
 * and so a caller can inspect exactly what would be sent.
 * */
NYA_API NYA_Error nya_smtp_message_build(NYA_Arena* arena, const NYA_SmtpMessage* message, OUT NYA_String** out_message) __attr_no_discard;

/**
 * Sends `message` through the server `config` names, and returns once the server has taken it.
 *
 * Opens the connection, encrypts it, logs in, and runs the MAIL FROM / RCPT TO / DATA exchange. Returns
 * NYA_ERROR_INVALID_ARGUMENT for a message a header rule refuses (see nya_smtp_message_build),
 * NYA_ERROR_NOT_SUPPORTED when the build has no TLS library, NYA_ERROR_TIMEOUT when the exchange outruns
 * the configured budget, and NYA_ERROR_IO or NYA_ERROR_NOT_OK when the connection fails or the server
 * refuses a step — the server's own reply text is carried in the error, the credentials never are.
 * */
NYA_API NYA_Error nya_smtp_send(NYA_Arena* arena, const NYA_SmtpConfig* config, const NYA_SmtpMessage* message) __attr_no_discard;
