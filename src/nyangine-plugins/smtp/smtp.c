#include <curl/curl.h>
#include <string.h>

#include "nyangine-std/base/base_arena.h"
#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_base64.h"
#include "nyangine-std/base/base_basic.h"
#include "nyangine-std/base/base_clock.h"
#include "nyangine-std/base/base_compare.h"
#include "nyangine-std/base/base_memory.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-plugins/curl/request.h"
#include "nyangine-plugins/smtp/smtp.h"

// PRIVATE TYPES

/** Where curl's read callback has got to in the assembled message. */
typedef struct {
    NYA_ConstCString blob;
    u64              size;
    u64              offset;
} _NYA_SmtpUpload;

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

/** curl's CURLOPT_READFUNCTION: hands out the next slice of the message for the DATA phase. */
NYA_INTERNAL u64 _nya_smtp_read_callback(char* buffer, u64 size, u64 count, void* user_data);

/** What a failed transfer means to the caller: the request plugin's mapping plus the SMTP cases. */
NYA_INTERNAL NYA_ErrorKind _nya_smtp_kind_from_curl(CURLcode code) __attr_no_discard;

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

    if (config->auth != NYA_SMTP_AUTH_NONE && (config->username == nullptr || config->password == nullptr)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "authentication was asked for without a username and a password");
    }

    // The message first, since a refused header should fail before any connection is opened.
    NYA_String* blob = nullptr;
    NYA_TRY(nya_smtp_message_build(arena, message, &blob));

    if (!_nya_request_global_ready) return nya_error(NYA_ERROR_NOT_OK, "libcurl was not initialized, see the earlier error");

    CURL* handle = curl_easy_init();
    if (handle == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "curl_easy_init() failed");
    defer curl_easy_cleanup(handle);

    // curl sends the URL's path as the EHLO name: the sender's domain, which is what the client always introduced itself as.
    NYA_ConstCString scheme = config->security == NYA_SMTP_SECURITY_IMPLICIT ? "smtps" : "smtp";
    NYA_ConstCString url    = nya_string_to_cstring(
        arena, nya_string_sprintf(arena, "%s://%s:%u/%s", scheme, config->host, (u32)config->port, _nya_smtp_domain(message->from))
    );

    NYA_ConstCString from      = nya_string_to_cstring(arena, nya_string_sprintf(arena, "<%s>", message->from));
    NYA_ConstCString recipient = nya_string_to_cstring(arena, nya_string_sprintf(arena, "<%s>", message->to));

    struct curl_slist* recipients = curl_slist_append(nullptr, recipient);
    if (recipients == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "curl_slist_append() failed");
    defer curl_slist_free_all(recipients);

    _NYA_SmtpUpload upload = { .blob = nya_string_to_cstring(arena, blob), .size = blob->length, .offset = 0 };

    u32  timeout_ms                 = config->timeout_ms != 0 ? config->timeout_ms : NYA_SMTP_DEFAULT_TIMEOUT_MS;
    char error[CURL_ERROR_SIZE + 1] = { 0 };

    curl_easy_setopt(handle, CURLOPT_URL, url);
    // smtp and smtps only, so no host a caller passes can turn this into a file read or another protocol.
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "smtp,smtps");
    // the no-plaintext rule: a server that does not offer STARTTLS is refused, never spoken to in the clear.
    curl_easy_setopt(handle, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    if (config->ca_path != nullptr && config->ca_path[0] != '\0') curl_easy_setopt(handle, CURLOPT_CAINFO, config->ca_path);

    if (config->auth != NYA_SMTP_AUTH_NONE) {
        curl_easy_setopt(handle, CURLOPT_USERNAME, config->username);
        curl_easy_setopt(handle, CURLOPT_PASSWORD, config->password);
        curl_easy_setopt(handle, CURLOPT_LOGIN_OPTIONS, config->auth == NYA_SMTP_AUTH_LOGIN ? "AUTH=LOGIN" : "AUTH=PLAIN");
    }

    curl_easy_setopt(handle, CURLOPT_MAIL_FROM, from);
    curl_easy_setopt(handle, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, (curl_off_t)upload.size);
    curl_easy_setopt(handle, CURLOPT_READFUNCTION, _nya_smtp_read_callback);
    curl_easy_setopt(handle, CURLOPT_READDATA, &upload);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error);

    CURLcode code = curl_easy_perform(handle);
    if (code == CURLE_OK) return NYA_OK;

    long reply = 0;
    (void)curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &reply);

    // curl's error buffer names the step and the server's code; it never carries the credentials.
    return nya_error(
        _nya_smtp_kind_from_curl(code),
        "sending mail through %s:%u failed (last reply %ld): %s",
        config->host,
        (u32)config->port,
        reply,
        error[0] != '\0' ? error : curl_easy_strerror(code)
    );
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

// PRIVATE API IMPLEMENTATION — THE TRANSFER

u64 _nya_smtp_read_callback(char* buffer, u64 size, u64 count, void* user_data) {
    _NYA_SmtpUpload* upload = user_data;
    nya_assert(upload->offset <= upload->size, "the upload cursor ran past the message");

    u64 span = nya_min(size * count, upload->size - upload->offset);
    nya_memcpy(buffer, upload->blob + upload->offset, span);
    upload->offset += span;

    return span;
}

NYA_ErrorKind _nya_smtp_kind_from_curl(CURLcode code) {
    switch (code) {
        case CURLE_USE_SSL_FAILED: return NYA_ERROR_NOT_SUPPORTED;
        case CURLE_LOGIN_DENIED:   return NYA_ERROR_PERMISSION_DENIED;
        // a refused MAIL FROM, RCPT TO or DATA: the server answered and said no.
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_WEIRD_SERVER_REPLY:
        case CURLE_UPLOAD_FAILED:  return NYA_ERROR_NOT_OK;
        default:                   return _nya_request_kind_from_curl(code);
    }
}
