#include <string.h>

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_base64.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_clock.h"
#include "nyangine/base/base_memory.h"
#include "nyangine/base/base_string.h"
#include "nyangine/os/os_socket.h"
#include "nyangine/smtp/smtp.h"
#include "nyangine/tls/tls.h"

// PRIVATE TYPES

typedef struct NYA_SmtpChannel NYA_SmtpChannel;

/** The conversation runs over one of these, not a socket, so a test can script it; `upgrade` does STARTTLS and is null when it cannot. */
struct NYA_SmtpChannel {
    NYA_Error (*write)(NYA_SmtpChannel* self, const u8* data, u64 size);
    NYA_Error (*read)(NYA_SmtpChannel* self, OUT u8* out, u64 capacity, OUT u64* out_read);
    NYA_Error (*upgrade)(NYA_SmtpChannel* self);
};

/** The real channel: a socket, and a TLS session over it once there is one. `base` first, so a cast works. */
typedef struct {
    NYA_SmtpChannel base;

    NYA_OsSocket     socket;
    NYA_TlsContext*  tls_context;
    NYA_TlsSession*  tls;
    NYA_ConstCString host;

    /** When the whole exchange must be done by, on the monotonic clock. Every wait is bounded by it. */
    u64 deadline_ns;
} SmtpSocketChannel;

// PRIVATE API DECLARATION

/** No carriage return and no line feed, which is the whole of the header-injection defence for a header value. */
NYA_INTERNAL b8 _nya_smtp_header_safe(NYA_ConstCString value) __attr_no_discard;

/** A bare address: no whitespace, no angle brackets, no control byte, and not empty. */
NYA_INTERNAL b8 _nya_smtp_address_safe(NYA_ConstCString value) __attr_no_discard;

/** A display name as an RFC 5322 quoted-string, with `"` and `\` escaped. */
NYA_INTERNAL NYA_ConstCString _nya_smtp_quote(NYA_Arena* arena, NYA_ConstCString name) __attr_no_discard;

/** The domain part of an address, or "localhost" when there is none — for EHLO and the Message-ID. */
NYA_INTERNAL NYA_ConstCString _nya_smtp_domain(NYA_ConstCString address) __attr_no_discard;

/** `timestamp_s` as an RFC 5322 date: `Thu, 01 Jan 1970 00:00:00 +0000`. */
NYA_INTERNAL NYA_ConstCString _nya_smtp_date(NYA_Arena* arena, u64 timestamp_s) __attr_no_discard;

/** `Content-Type` and base64 body of one MIME part, wrapped at 76 columns, appended to `out`. */
NYA_INTERNAL void _nya_smtp_append_part(NYA_Arena* arena, OUT NYA_String* out, NYA_ConstCString content_type, NYA_ConstCString body);

/** Whether `buffer` holds a whole SMTP reply yet, and its status code when it does. */
NYA_INTERNAL b8 _nya_smtp_reply_complete(const NYA_String* buffer, OUT u32* out_code) __attr_no_discard;

/** Reads one whole reply — following its continuation lines — and answers its code. Bounded by NYA_SMTP_MAX_REPLY. */
NYA_INTERNAL NYA_Error _nya_smtp_read_reply(NYA_SmtpChannel* channel, NYA_Arena* arena, OUT u32* out_code, OUT NYA_ConstCString* out_text) __attr_no_discard;

/** Writes `line` and a CRLF. */
NYA_INTERNAL NYA_Error _nya_smtp_write_line(NYA_SmtpChannel* channel, NYA_ConstCString line) __attr_no_discard;

/** Reads a reply and fails, naming `step` and quoting the server, when its code is not `expected`. */
NYA_INTERNAL NYA_Error _nya_smtp_expect(NYA_SmtpChannel* channel, NYA_Arena* arena, u32 expected, NYA_ConstCString step) __attr_no_discard;

/** Writes `line` and then expects `expected`. */
NYA_INTERNAL NYA_Error _nya_smtp_do(NYA_SmtpChannel* channel, NYA_Arena* arena, NYA_ConstCString line, u32 expected, NYA_ConstCString step) __attr_no_discard;

/** EHLO, naming the sender's domain. Sent again after STARTTLS, which is why it is its own step. */
NYA_INTERNAL NYA_Error _nya_smtp_ehlo(NYA_SmtpChannel* channel, NYA_Arena* arena, const NYA_SmtpMessage* message) __attr_no_discard;

/** AUTH LOGIN or AUTH PLAIN, base64'ing the credentials and wiping the scratch they passed through. */
NYA_INTERNAL NYA_Error _nya_smtp_auth(NYA_SmtpChannel* channel, NYA_Arena* arena, const NYA_SmtpConfig* config) __attr_no_discard;

/** The whole conversation over `channel`, from greeting to QUIT. Split out so a test can script it. */
NYA_INTERNAL NYA_Error _nya_smtp_converse(NYA_Arena* arena, const NYA_SmtpConfig* config, const NYA_SmtpMessage* message, NYA_ConstCString blob, NYA_SmtpChannel* channel)
    __attr_no_discard;

// The real channel, over a socket and its TLS session.
NYA_INTERNAL NYA_Error _nya_smtp_wait(NYA_OsSocket socket, b8 readable, b8 writable, u64 deadline_ns) __attr_no_discard;
NYA_INTERNAL NYA_Error _nya_smtp_tls_handshake(SmtpSocketChannel* channel) __attr_no_discard;
NYA_INTERNAL NYA_Error _nya_smtp_channel_write(NYA_SmtpChannel* self, const u8* data, u64 size) __attr_no_discard;
NYA_INTERNAL NYA_Error _nya_smtp_channel_read(NYA_SmtpChannel* self, OUT u8* out, u64 capacity, OUT u64* out_read) __attr_no_discard;
NYA_INTERNAL NYA_Error _nya_smtp_channel_upgrade(NYA_SmtpChannel* self) __attr_no_discard;
NYA_INTERNAL NYA_Error _nya_smtp_connect(NYA_ConstCString host, u16 port, u64 deadline_ns, OUT NYA_OsSocket* out_socket) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

NYA_Error nya_smtp_message_build(NYA_Arena* arena, const NYA_SmtpMessage* message, NYA_String** out_message) {
    nya_assert(arena != nullptr && message != nullptr && out_message != nullptr);

    *out_message = nullptr;

    if (message->from == nullptr || message->to == nullptr || message->text == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a message needs a from address, a to address and a text body");
    }

    // The header-injection defence: a value with a newline is refused; addresses are held to more (see the checks).
    if (!_nya_smtp_address_safe(message->from)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the from address is not a bare address");
    if (!_nya_smtp_address_safe(message->to)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the to address is not a bare address");
    if (!_nya_smtp_header_safe(message->subject)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the subject carries a carriage return or a line feed");
    if (!_nya_smtp_header_safe(message->from_name)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the sender name carries a carriage return or a line feed");

    NYA_String* out = nya_string_create(arena);

    if (message->from_name != nullptr && message->from_name[0] != '\0') {
        nya_string_extend_sprintf(out, "From: %s <%s>\r\n", _nya_smtp_quote(arena, message->from_name), message->from);
    } else {
        nya_string_extend_sprintf(out, "From: %s\r\n", message->from);
    }

    nya_string_extend_sprintf(out, "To: %s\r\n", message->to);
    nya_string_extend_sprintf(out, "Subject: %s\r\n", message->subject != nullptr ? message->subject : "");
    nya_string_extend_sprintf(out, "Date: %s\r\n", _nya_smtp_date(arena, nya_clock_get_timestamp_s()));

    // A Message-ID distinct enough for transactional mail: two independent clocks at the sender's domain, not unpredictable.
    nya_string_extend_sprintf(out, "Message-ID: <%016llx.%016llx@%s>\r\n", (unsigned long long)nya_clock_get_timestamp_ns(),
                              (unsigned long long)nya_clock_get_monotonic_ns(), _nya_smtp_domain(message->from));

    nya_string_extend(out, "MIME-Version: 1.0\r\n");

    if (message->html != nullptr && message->html[0] != '\0') {
        // A multipart/alternative carrying both bodies; the boundary uses `=` and `_`, outside base64, so it never collides with a part.
        NYA_ConstCString boundary = nya_string_to_cstring(arena, nya_string_sprintf(arena, "=_nyangine_%016llx_=", (unsigned long long)nya_clock_get_monotonic_ns()));

        nya_string_extend_sprintf(out, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n\r\n", boundary);

        nya_string_extend_sprintf(out, "--%s\r\n", boundary);
        _nya_smtp_append_part(arena, out, "text/plain; charset=UTF-8", message->text);

        nya_string_extend_sprintf(out, "--%s\r\n", boundary);
        _nya_smtp_append_part(arena, out, "text/html; charset=UTF-8", message->html);

        nya_string_extend_sprintf(out, "--%s--\r\n", boundary);
    } else {
        nya_string_extend(out, "\r\n");
        _nya_smtp_append_part(arena, out, "text/plain; charset=UTF-8", message->text);
    }

    if (out->length > NYA_SMTP_MAX_MESSAGE) {
        nya_string_clear(out);
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the assembled message is larger than the %llu byte bound", (unsigned long long)NYA_SMTP_MAX_MESSAGE);
    }

    *out_message = out;

    return NYA_OK;
}

NYA_Error nya_smtp_send(NYA_Arena* arena, const NYA_SmtpConfig* config, const NYA_SmtpMessage* message) {
    nya_assert(arena != nullptr && config != nullptr && message != nullptr);

    if (config->host == nullptr || config->host[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a submission host is required");
    if (config->port == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a port is required");

    // The message first, since a refused header should fail before any connection is opened.
    NYA_String* blob = nullptr;
    NYA_TRY(nya_smtp_message_build(arena, message, &blob));

    if (!nya_tls_available()) return nya_error(NYA_ERROR_NOT_SUPPORTED, "this build has no TLS library, and mail is never sent in the clear");

    if (nya_os_socket_start() != NYA_OS_SOCKET_OK) return nya_error(NYA_ERROR_IO, "the host's socket library would not start");
    defer nya_os_socket_stop();

    u32 timeout_ms  = config->timeout_ms != 0 ? config->timeout_ms : NYA_SMTP_DEFAULT_TIMEOUT_MS;
    u64 deadline_ns = nya_clock_get_monotonic_ns() + nya_time_ms_to_ns(timeout_ms);

    NYA_TlsContext* tls_context = nullptr;
    NYA_TRY(nya_tls_context_create(arena, &tls_context, .client = true, .ca_path = config->ca_path));
    defer nya_tls_context_destroy(tls_context);

    NYA_OsSocket socket = NYA_OS_SOCKET_NONE;
    NYA_TRY(_nya_smtp_connect(config->host, config->port, deadline_ns, &socket));
    defer nya_os_socket_close(socket);

    (void)nya_os_socket_set_no_delay(socket, true);

    SmtpSocketChannel channel = {
        .base        = { .write = _nya_smtp_channel_write, .read = _nya_smtp_channel_read, .upgrade = _nya_smtp_channel_upgrade },
        .socket      = socket,
        .tls_context = tls_context,
        .tls         = nullptr,
        .host        = config->host,
        .deadline_ns = deadline_ns,
    };

    // Implicit TLS handshakes before any SMTP byte; STARTTLS waits for the greeting. Either way the session is the context's pool's.
    if (config->security == NYA_SMTP_SECURITY_IMPLICIT) {
        NYA_TRY(nya_tls_session_connect(tls_context, socket, config->host, &channel.tls));
        NYA_TRY(_nya_smtp_tls_handshake(&channel));
    }

    return _nya_smtp_converse(arena, config, message, nya_string_to_cstring(arena, blob), &channel.base);
}

// PRIVATE API IMPLEMENTATION — THE MESSAGE

b8 _nya_smtp_header_safe(NYA_ConstCString value) {
    if (value == nullptr) return true;

    for (NYA_ConstCString at = value; *at != '\0'; at++) {
        if (*at == '\r' || *at == '\n') return false;
    }

    return true;
}

b8 _nya_smtp_address_safe(NYA_ConstCString value) {
    if (value == nullptr || value[0] == '\0') return false;

    for (NYA_ConstCString at = value; *at != '\0'; at++) {
        u8 character = (u8)*at;

        if (character <= ' ' || character == '<' || character == '>' || character == 0x7F) return false;
    }

    return true;
}

NYA_ConstCString _nya_smtp_quote(NYA_Arena* arena, NYA_ConstCString name) {
    NYA_String* quoted = nya_string_create(arena);

    nya_string_push_back(quoted, '"');
    for (NYA_ConstCString at = name; *at != '\0'; at++) {
        if (*at == '"' || *at == '\\') nya_string_push_back(quoted, '\\');
        nya_string_push_back(quoted, (u8)*at);
    }
    nya_string_push_back(quoted, '"');

    return nya_string_to_cstring(arena, quoted);
}

NYA_ConstCString _nya_smtp_domain(NYA_ConstCString address) {
    NYA_ConstCString at = strchr(address, '@');

    return at != nullptr && at[1] != '\0' ? at + 1 : "localhost";
}

NYA_ConstCString _nya_smtp_date(NYA_Arena* arena, u64 timestamp_s) {
    static NYA_ConstCString const weekdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static NYA_ConstCString const months[]   = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    s64 days           = (s64)(timestamp_s / NYA_CLOCK_SECONDS_PER_DAY);
    u64 seconds_of_day = timestamp_s % NYA_CLOCK_SECONDS_PER_DAY;

    u32 hour   = (u32)(seconds_of_day / 3600);
    u32 minute = (u32)((seconds_of_day % 3600) / 60);
    u32 second = (u32)(seconds_of_day % 60);

    s32 year  = 0;
    u32 month = 0;
    u32 day   = 0;
    nya_clock_civil_from_days(days, &year, &month, &day);

    // The Unix epoch, day zero, was a Thursday, which is index 4 counting from Sunday.
    u32 weekday = (u32)(((days % 7) + 4) % 7);

    return nya_string_to_cstring(
        arena, nya_string_sprintf(arena, "%s, %02u %s %d %02u:%02u:%02u +0000", weekdays[weekday], day, months[month - 1], year, hour, minute, second)
    );
}

void _nya_smtp_append_part(NYA_Arena* arena, NYA_String* out, NYA_ConstCString content_type, NYA_ConstCString body) {
    nya_string_extend_sprintf(out, "Content-Type: %s\r\n", content_type);
    nya_string_extend(out, "Content-Transfer-Encoding: base64\r\n\r\n");

    // base64 keeps every line to the RFC's 76 columns and free of the bytes — a bare CR or LF, a line
    // that is only a dot — that a DATA phase would otherwise have to escape by hand.
    NYA_String* encoded = nya_string_create(arena);
    nya_base64_encode(encoded, (const u8*)body, strlen(body));

    for (u64 offset = 0; offset < encoded->length; offset += 76) {
        u64 span = encoded->length - offset;
        if (span > 76) span = 76;

        for (u64 index = 0; index < span; index++) nya_string_push_back(out, encoded->items[offset + index]);
        nya_string_extend(out, "\r\n");
    }
}

// PRIVATE API IMPLEMENTATION — THE CONVERSATION

b8 _nya_smtp_reply_complete(const NYA_String* buffer, u32* out_code) {
    u64 start = 0;

    for (u64 i = 0; i < buffer->length; i++) {
        if (buffer->items[i] != '\n') continue;

        u64 end = i;
        if (end > start && buffer->items[end - 1] == '\r') end--;

        // A reply's last line is `NNN ` (space, not `-`); three digits then a space means the server has finished.
        u64 length = end - start;
        if (length >= 4 && buffer->items[start + 3] == ' ') {
            u32 code       = 0;
            b8  are_digits = true;

            for (u64 k = 0; k < 3; k++) {
                u8 character = buffer->items[start + k];
                if (character < '0' || character > '9') are_digits = false;
                code = code * 10 + (u32)(character - '0');
            }

            if (are_digits) {
                *out_code = code;
                return true;
            }
        }

        start = i + 1;
    }

    return false;
}

NYA_Error _nya_smtp_read_reply(NYA_SmtpChannel* channel, NYA_Arena* arena, u32* out_code, NYA_ConstCString* out_text) {
    NYA_String* buffer = nya_string_create(arena);

    for (;;) {
        u32 code = 0;
        if (_nya_smtp_reply_complete(buffer, &code)) {
            *out_code = code;
            if (out_text != nullptr) *out_text = nya_string_to_cstring(arena, buffer);
            return NYA_OK;
        }

        if (buffer->length > NYA_SMTP_MAX_REPLY) return nya_error(NYA_ERROR_IO, "the server's reply ran past the %d byte bound", NYA_SMTP_MAX_REPLY);

        u8  chunk[512] = { 0 };
        u64 got        = 0;
        NYA_TRY(channel->read(channel, chunk, sizeof(chunk), &got));

        if (got == 0) return nya_error(NYA_ERROR_IO, "the server closed the connection before finishing a reply");

        for (u64 i = 0; i < got; i++) nya_string_push_back(buffer, chunk[i]);
    }
}

NYA_Error _nya_smtp_write_line(NYA_SmtpChannel* channel, NYA_ConstCString line) {
    NYA_TRY(channel->write(channel, (const u8*)line, strlen(line)));
    NYA_TRY(channel->write(channel, (const u8*)"\r\n", 2));

    return NYA_OK;
}

NYA_Error _nya_smtp_expect(NYA_SmtpChannel* channel, NYA_Arena* arena, u32 expected, NYA_ConstCString step) {
    u32              code = 0;
    NYA_ConstCString text = nullptr;
    NYA_TRY(_nya_smtp_read_reply(channel, arena, &code, &text));

    // The reply text is safe to carry; the command that drew it (an AUTH line) is never put in an error.
    if (code != expected) return nya_error(NYA_ERROR_NOT_OK, "the server refused %s (%u): %s", step, code, text);

    return NYA_OK;
}

NYA_Error _nya_smtp_do(NYA_SmtpChannel* channel, NYA_Arena* arena, NYA_ConstCString line, u32 expected, NYA_ConstCString step) {
    NYA_TRY(_nya_smtp_write_line(channel, line));
    NYA_TRY(_nya_smtp_expect(channel, arena, expected, step));

    return NYA_OK;
}

NYA_Error _nya_smtp_ehlo(NYA_SmtpChannel* channel, NYA_Arena* arena, const NYA_SmtpMessage* message) {
    NYA_ConstCString line = nya_string_to_cstring(arena, nya_string_sprintf(arena, "EHLO %s", _nya_smtp_domain(message->from)));

    return _nya_smtp_do(channel, arena, line, 250, "EHLO");
}

NYA_Error _nya_smtp_auth(NYA_SmtpChannel* channel, NYA_Arena* arena, const NYA_SmtpConfig* config) {
    if (config->auth == NYA_SMTP_AUTH_NONE) return NYA_OK;

    if (config->username == nullptr || config->password == nullptr) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "authentication was asked for without a username and a password");
    }

    u64 username_length = strlen(config->username);
    u64 password_length = strlen(config->password);

    if (config->auth == NYA_SMTP_AUTH_LOGIN) {
        NYA_TRY(_nya_smtp_do(channel, arena, "AUTH LOGIN", 334, "the AUTH LOGIN request"));

        NYA_String* username = nya_string_create(arena);
        nya_base64_encode(username, (const u8*)config->username, username_length);
        NYA_TRY(_nya_smtp_do(channel, arena, nya_string_to_cstring(arena, username), 334, "the username"));

        NYA_String* password = nya_string_create(arena);
        nya_base64_encode(password, (const u8*)config->password, password_length);

        NYA_Error sent = _nya_smtp_write_line(channel, nya_string_to_cstring(arena, password));
        nya_memset(password->items, 0, password->length); // the encoded secret is done with; do not leave it lying in the arena.
        NYA_TRY(sent);

        NYA_TRY(_nya_smtp_expect(channel, arena, 235, "the login"));
        return NYA_OK;
    }

    // AUTH PLAIN: one token of authorize-id (empty), authenticate-id and password, NUL separated.
    u64 token_size = 1 + username_length + 1 + password_length;
    u8* token      = nya_arena_alloc(arena, token_size);

    token[0] = 0;
    nya_memcpy(token + 1, config->username, username_length);
    token[1 + username_length] = 0;
    nya_memcpy(token + 2 + username_length, config->password, password_length);

    NYA_String* encoded = nya_string_create(arena);
    nya_base64_encode(encoded, token, token_size);
    nya_memset(token, 0, token_size); // the cleartext credential is done with the moment it is encoded.

    NYA_ConstCString line = nya_string_to_cstring(arena, nya_string_sprintf(arena, "AUTH PLAIN %s", nya_string_to_cstring(arena, encoded)));

    NYA_Error sent = _nya_smtp_write_line(channel, line);
    nya_memset(encoded->items, 0, encoded->length);
    NYA_TRY(sent);

    NYA_TRY(_nya_smtp_expect(channel, arena, 235, "the login"));
    return NYA_OK;
}

NYA_Error _nya_smtp_converse(NYA_Arena* arena, const NYA_SmtpConfig* config, const NYA_SmtpMessage* message, NYA_ConstCString blob, NYA_SmtpChannel* channel) {
    NYA_TRY(_nya_smtp_expect(channel, arena, 220, "the greeting"));
    NYA_TRY(_nya_smtp_ehlo(channel, arena, message));

    if (config->security == NYA_SMTP_SECURITY_STARTTLS) {
        NYA_TRY(_nya_smtp_do(channel, arena, "STARTTLS", 220, "STARTTLS"));

        if (channel->upgrade == nullptr) return nya_error(NYA_ERROR_NOT_SUPPORTED, "the connection cannot be upgraded to TLS");
        NYA_TRY(channel->upgrade(channel));

        // A second EHLO over the encrypted link: capabilities before and after STARTTLS differ and the earlier list is not trusted.
        NYA_TRY(_nya_smtp_ehlo(channel, arena, message));
    }

    NYA_TRY(_nya_smtp_auth(channel, arena, config));

    NYA_TRY(_nya_smtp_do(channel, arena, nya_string_to_cstring(arena, nya_string_sprintf(arena, "MAIL FROM:<%s>", message->from)), 250, "MAIL FROM"));

    // RCPT is the one step with two success codes: 250 is delivered here, 251 is accepted and forwarded on.
    NYA_TRY(_nya_smtp_write_line(channel, nya_string_to_cstring(arena, nya_string_sprintf(arena, "RCPT TO:<%s>", message->to))));
    {
        u32              code = 0;
        NYA_ConstCString text = nullptr;
        NYA_TRY(_nya_smtp_read_reply(channel, arena, &code, &text));
        if (code != 250 && code != 251) return nya_error(NYA_ERROR_NOT_OK, "the server refused the recipient (%u): %s", code, text);
    }

    NYA_TRY(_nya_smtp_do(channel, arena, "DATA", 354, "DATA"));

    NYA_TRY(channel->write(channel, (const u8*)blob, strlen(blob)));
    NYA_TRY(channel->write(channel, (const u8*)"\r\n.\r\n", 5));
    NYA_TRY(_nya_smtp_expect(channel, arena, 250, "the message"));

    // QUIT is a courtesy: the mail is already accepted, so its answer is not worth failing the send over.
    (void)_nya_smtp_write_line(channel, "QUIT");
    {
        u32 code = 0;
        (void)_nya_smtp_read_reply(channel, arena, &code, nullptr);
    }

    return NYA_OK;
}

// PRIVATE API IMPLEMENTATION — THE SOCKET CHANNEL

NYA_Error _nya_smtp_wait(NYA_OsSocket socket, b8 readable, b8 writable, u64 deadline_ns) {
    u64 now = nya_clock_get_monotonic_ns();
    if (now >= deadline_ns) return nya_error(NYA_ERROR_TIMEOUT, "the SMTP exchange ran out of time");

    u64 remaining_ms = nya_time_ns_to_ms(deadline_ns - now);
    if (remaining_ms == 0) remaining_ms = 1;
    if (remaining_ms > NYA_OS_SOCKET_WAIT_FOREVER - 1) remaining_ms = NYA_OS_SOCKET_WAIT_FOREVER - 1;

    NYA_OsSocketWait wait  = { .socket = socket, .readable = readable, .writable = writable };
    u32              ready = 0;
    if (nya_os_socket_wait(&wait, 1, (u32)remaining_ms, &ready) != NYA_OS_SOCKET_OK) return nya_error(NYA_ERROR_IO, "waiting on the connection failed");

    if (ready == 0) return nya_error(NYA_ERROR_TIMEOUT, "the SMTP exchange ran out of time");

    return NYA_OK;
}

NYA_Error _nya_smtp_tls_handshake(SmtpSocketChannel* channel) {
    for (;;) {
        switch (nya_tls_handshake(channel->tls)) {
            case NYA_TLS_OK: return NYA_OK;
            case NYA_TLS_WANT_READ:  NYA_TRY(_nya_smtp_wait(channel->socket, true, false, channel->deadline_ns)); break;
            case NYA_TLS_WANT_WRITE: NYA_TRY(_nya_smtp_wait(channel->socket, false, true, channel->deadline_ns)); break;
            default:                 return nya_error(NYA_ERROR_IO, "the TLS handshake with the server failed: %s", nya_tls_error(channel->tls));
        }
    }
}

NYA_Error _nya_smtp_channel_write(NYA_SmtpChannel* self, const u8* data, u64 size) {
    SmtpSocketChannel* channel = (SmtpSocketChannel*)self;

    u64 total = 0;
    while (total < size) {
        if (channel->tls != nullptr) {
            u64 written = 0;
            switch (nya_tls_send(channel->tls, data + total, size - total, &written)) {
                case NYA_TLS_OK: total += written; break;
                case NYA_TLS_WANT_READ:  NYA_TRY(_nya_smtp_wait(channel->socket, true, false, channel->deadline_ns)); break;
                case NYA_TLS_WANT_WRITE: NYA_TRY(_nya_smtp_wait(channel->socket, false, true, channel->deadline_ns)); break;
                default:                 return nya_error(NYA_ERROR_IO, "sending over TLS failed: %s", nya_tls_error(channel->tls));
            }
            continue;
        }

        u64 sent = 0;
        switch (nya_os_socket_send(channel->socket, data + total, size - total, &sent)) {
            case NYA_OS_SOCKET_OK:
                total += sent;
                if (sent == 0) NYA_TRY(_nya_smtp_wait(channel->socket, false, true, channel->deadline_ns));
                break;
            case NYA_OS_SOCKET_WOULD_BLOCK: NYA_TRY(_nya_smtp_wait(channel->socket, false, true, channel->deadline_ns)); break;
            default:                        return nya_error(NYA_ERROR_IO, "the connection was lost while sending");
        }
    }

    return NYA_OK;
}

NYA_Error _nya_smtp_channel_read(NYA_SmtpChannel* self, u8* out, u64 capacity, u64* out_read) {
    SmtpSocketChannel* channel = (SmtpSocketChannel*)self;

    *out_read = 0;

    for (;;) {
        if (channel->tls != nullptr) {
            u64 read = 0;
            switch (nya_tls_receive(channel->tls, out, capacity, &read)) {
                case NYA_TLS_OK: *out_read = read; return NYA_OK;
                case NYA_TLS_WANT_READ:  NYA_TRY(_nya_smtp_wait(channel->socket, true, false, channel->deadline_ns)); break;
                case NYA_TLS_WANT_WRITE: NYA_TRY(_nya_smtp_wait(channel->socket, false, true, channel->deadline_ns)); break;
                case NYA_TLS_CLOSED: return NYA_OK; // out_read stays zero: the caller reads that as a close.
                default:             return nya_error(NYA_ERROR_IO, "receiving over TLS failed: %s", nya_tls_error(channel->tls));
            }
            continue;
        }

        u64 read = 0;
        switch (nya_os_socket_receive(channel->socket, out, capacity, &read)) {
            case NYA_OS_SOCKET_OK: *out_read = read; return NYA_OK;
            case NYA_OS_SOCKET_WOULD_BLOCK: NYA_TRY(_nya_smtp_wait(channel->socket, true, false, channel->deadline_ns)); break;
            case NYA_OS_SOCKET_CLOSED: return NYA_OK; // out_read stays zero.
            default:                   return nya_error(NYA_ERROR_IO, "the connection was lost while receiving");
        }
    }
}

NYA_Error _nya_smtp_channel_upgrade(NYA_SmtpChannel* self) {
    SmtpSocketChannel* channel = (SmtpSocketChannel*)self;

    NYA_TRY(nya_tls_session_connect(channel->tls_context, channel->socket, channel->host, &channel->tls));

    return _nya_smtp_tls_handshake(channel);
}

NYA_Error _nya_smtp_connect(NYA_ConstCString host, u16 port, u64 deadline_ns, NYA_OsSocket* out_socket) {
    *out_socket = NYA_OS_SOCKET_NONE;

    NYA_OsAddress address = { 0 };
    if (nya_os_address_resolve(host, port, NYA_OS_ADDRESS_NONE, &address) != NYA_OS_SOCKET_OK) return nya_error(NYA_ERROR_IO, "'%s' could not be resolved", host);

    NYA_OsSocket       socket = NYA_OS_SOCKET_NONE;
    NYA_OsSocketStatus status = nya_os_socket_connect(address, &socket);

    if (status == NYA_OS_SOCKET_OK) {
        *out_socket = socket;
        return NYA_OK;
    }

    if (status != NYA_OS_SOCKET_WOULD_BLOCK) {
        nya_os_socket_close(socket);
        return nya_error(NYA_ERROR_IO, "could not connect to %s:%u", host, (u32)port);
    }

    // Non-blocking connect: wait for the socket to be writable, then ask what came of it.
    NYA_Error waited = _nya_smtp_wait(socket, false, true, deadline_ns);
    if (!waited.ok) {
        nya_os_socket_close(socket);
        return waited;
    }

    if (nya_os_socket_error(socket) != NYA_OS_SOCKET_OK) {
        nya_os_socket_close(socket);
        return nya_error(NYA_ERROR_IO, "the connection to %s:%u was refused", host, (u32)port);
    }

    *out_socket = socket;
    return NYA_OK;
}
