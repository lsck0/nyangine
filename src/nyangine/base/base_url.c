#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL const NYA_ConstCString _NYA_URL_RULE_TEXT[NYA_URL_RULE_COUNT] = {
    [NYA_URL_RULE_NONE]               = "none",
    [NYA_URL_RULE_EMPTY]              = "empty",
    [NYA_URL_RULE_TOO_LONG]           = "too long",
    [NYA_URL_RULE_CONTROL_CHARACTER]  = "a control character",
    [NYA_URL_RULE_SPACE]              = "a space",
    [NYA_URL_RULE_CHARACTER]          = "a character the grammar has no place for",
    [NYA_URL_RULE_PERCENT_MALFORMED]  = "a malformed percent escape",
    [NYA_URL_RULE_SCHEME_MISSING]     = "no scheme",
    [NYA_URL_RULE_SCHEME_UNKNOWN]     = "an unknown scheme",
    [NYA_URL_RULE_AUTHORITY_MISSING]  = "no \"//\" and host after the scheme",
    [NYA_URL_RULE_HOST_EMPTY]         = "an empty host",
    [NYA_URL_RULE_HOST_TOO_LONG]      = "a host that is too long",
    [NYA_URL_RULE_HOST_MALFORMED]     = "a malformed host",
    [NYA_URL_RULE_IPV6_MALFORMED]     = "a malformed IPv6 address",
    [NYA_URL_RULE_PORT_MALFORMED]     = "a malformed port",
    [NYA_URL_RULE_PORT_OUT_OF_RANGE]  = "a port outside 1..65535",
    [NYA_URL_RULE_PATH_NOT_ABSOLUTE]  = "a path that does not start with '/'",
    [NYA_URL_RULE_PATH_ENCODED_SLASH] = "an encoded '/' in a path segment",
    [NYA_URL_RULE_PATH_DOT_SEGMENT]   = "a \".\" or \"..\" path segment",
    [NYA_URL_RULE_PATH_DOUBLE_SLASH]  = "a path starting with \"//\"",
};

/** Lower case, which is what nya_url_format writes and what an input scheme is folded to before comparing. */
NYA_INTERNAL const NYA_ConstCString _NYA_URL_SCHEME_TEXT[NYA_URL_SCHEME_COUNT] = {
    [NYA_URL_SCHEME_NONE]  = "",
    [NYA_URL_SCHEME_HTTP]  = "http",
    [NYA_URL_SCHEME_HTTPS] = "https",
    [NYA_URL_SCHEME_WS]    = "ws",
    [NYA_URL_SCHEME_WSS]   = "wss",
};

/** Upper case, as RFC 3986 2.1 asks producers to write an escape. */
NYA_INTERNAL const char _NYA_URL_HEX_UPPER[] = "0123456789ABCDEF";

/** Highest port. Five digits are checked against it, so the accumulator never passes 99999. */
#define _NYA_URL_PORT_MAX 65535U

/** Octets in an IPv4 address, the largest one, and the most digits one may be written in. */
#define _NYA_URL_IPV4_OCTETS           4U
#define _NYA_URL_IPV4_OCTET_MAX        255U
#define _NYA_URL_IPV4_OCTET_MAX_DIGITS 3U

/** Groups in an IPv6 address, the hex digits one may have, and how many groups an embedded dotted quad stands for. */
#define _NYA_URL_IPV6_GROUPS           8U
#define _NYA_URL_IPV6_GROUP_MAX_DIGITS 4U
#define _NYA_URL_IPV6_IPV4_GROUPS      2U

/** Everything nya_url_parse checks, as a bool so every rule is one early return. */
NYA_INTERNAL b8 _nya_url_parse_absolute(const char* text, u64 size, NYA_Url* url, NYA_UrlFailure* failure) __attr_no_discard;

/** Everything nya_url_parse_target checks. */
NYA_INTERNAL b8 _nya_url_parse_origin(const char* text, u64 size, NYA_Url* url, NYA_UrlFailure* failure) __attr_no_discard;

/** Records the refusal and returns false, so every check reads `return _nya_url_refuse(...)`. */
NYA_INTERNAL b8 _nya_url_refuse(NYA_UrlFailure* failure, NYA_UrlRule rule, u64 offset);

/** Turns a refusal into the error both parse calls return, and zeroes the half built URL. */
NYA_INTERNAL NYA_Error _nya_url_refused(NYA_Url* url, const NYA_UrlFailure* failure) __attr_no_discard;

/** The value of one hex digit, or false. */
NYA_INTERNAL b8 _nya_url_hex_value(char character, OUT u8* out_value) __attr_no_discard;

NYA_INTERNAL b8 _nya_url_is_alpha(char character) __attr_no_discard;
NYA_INTERNAL b8 _nya_url_is_digit(char character) __attr_no_discard;
NYA_INTERNAL b8 _nya_url_is_hex(char character) __attr_no_discard;
NYA_INTERNAL b8 _nya_url_is_unreserved(char character) __attr_no_discard;
NYA_INTERNAL b8 _nya_url_is_sub_delim(char character) __attr_no_discard;

/** RFC 3986's scheme alphabet after the first letter: letters, digits, '+', '-' and '.'. */
NYA_INTERNAL b8 _nya_url_is_scheme_char(char character) __attr_no_discard;

/**
 * The byte at `*index`, decoded when it starts an escape, with `*index` moved past what it read. False
 * for a '%' without two hex digits, leaving `*index` on the '%'. '+' is a space only when asked, since
 * only a query reads it that way.
 * */
NYA_INTERNAL b8 _nya_url_decode_next(const char* text, u64 size, u64* index, b8 plus_is_space, OUT u8* out_byte) __attr_no_discard;

/**
 * Decodes a whole run into `buffer`. On failure `out_offset` is the bad escape, or `size` when it was
 * `buffer` that ran out, which is how a caller tells the two apart.
 * */
NYA_INTERNAL b8
_nya_url_decode_run(const char* text, u64 size, b8 plus_is_space, OUT u8* buffer, u64 capacity, OUT u64* out_length, OUT u64* out_offset)
    __attr_no_discard;

/** Whether `text` decodes, form style, to exactly `name`. A malformed escape is not a match. */
NYA_INTERNAL b8 _nya_url_form_equals(const char* text, u64 size, NYA_ConstCString name) __attr_no_discard;

/** The pass over the whole input: controls, spaces, non-ASCII and anything outside RFC 3986's alphabet. */
NYA_INTERNAL b8 _nya_url_check_alphabet(const char* text, u64 size, NYA_UrlFailure* failure) __attr_no_discard;

/**
 * Userinfo, query and fragment: RFC 3986's pchar plus `extra`, well formed escapes, and no NUL decoded
 * from one.
 * */
NYA_INTERNAL b8 _nya_url_check_component(const char* text, u64 start, u64 end, NYA_ConstCString extra, NYA_UrlFailure* failure)
    __attr_no_discard;

/** The path: pchar and '/', plus the segment rules from NYA_UrlRule. `start == end` is an empty path. */
NYA_INTERNAL b8 _nya_url_check_path(const char* text, u64 start, u64 end, NYA_UrlFailure* failure) __attr_no_discard;

/** A dotted quad in the one spelling every resolver agrees on: four decimal octets, no leading zeros. */
NYA_INTERNAL b8 _nya_url_is_ipv4(const char* text, u64 size) __attr_no_discard;

/** RFC 4291's text form, "::" at most once, a dotted quad allowed last. No zone id. */
NYA_INTERNAL b8 _nya_url_is_ipv6(const char* text, u64 size) __attr_no_discard;

/** A host that is not bracketed: a DNS name, or an IPv4 address when its last label is a number. */
NYA_INTERNAL b8 _nya_url_check_host(const char* text, u64 start, u64 end, OUT NYA_UrlHostKind* out_kind, NYA_UrlFailure* failure)
    __attr_no_discard;

/** Everything between "//" and the path: optional userinfo, the host, an optional port. */
NYA_INTERNAL b8 _nya_url_parse_authority(const char* text, u64 start, u64 end, NYA_Url* url, NYA_UrlFailure* failure) __attr_no_discard;

/** The path, query and fragment from `start` on, checked and stored. `fragment_allowed` is false for a request target. */
NYA_INTERNAL b8 _nya_url_parse_rest(const char* text, u64 start, u64 size, b8 fragment_allowed, NYA_Url* url, NYA_UrlFailure* failure)
    __attr_no_discard;

/** Appends `text[start, end)` to `url->text` and returns where it went. */
NYA_INTERNAL NYA_UrlSpan _nya_url_store(NYA_Url* url, const char* text, u64 start, u64 end) __attr_no_discard;

/** Copies `length` bytes to `buffer + *written`, keeping a byte for the terminator. False when they do not fit. */
NYA_INTERNAL b8 _nya_url_append(char* buffer, u64 capacity, u64* written, const char* piece, u64 length) __attr_no_discard;

/** A span lies inside the stored text. For the calls that take a URL a caller may have written to by hand. */
NYA_INTERNAL void _nya_url_assert_span(const NYA_Url* url, NYA_UrlSpan span);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_url_parse(const char* text, u64 size, NYA_Url* out_url, NYA_UrlFailure* out_failure) {
    nya_assert(text != nullptr || size == 0);
    nya_assert(out_url != nullptr);

    NYA_UrlFailure  local   = { 0 };
    NYA_UrlFailure* failure = out_failure != nullptr ? out_failure : &local;

    *failure = (NYA_UrlFailure){ 0 };
    *out_url = (NYA_Url){ 0 };

    if (!_nya_url_parse_absolute(text, size, out_url, failure)) return _nya_url_refused(out_url, failure);

    nya_assert(out_url->scheme != NYA_URL_SCHEME_NONE && out_url->scheme < NYA_URL_SCHEME_COUNT);
    nya_assert(out_url->host_kind != NYA_URL_HOST_NONE && out_url->host.length > 0, "an absolute url parsed without a host");
    nya_assert(!out_url->has_port || out_url->port > 0);
    nya_assert(out_url->length <= size, "the stored components are longer than the input");

    return NYA_OK;
}

NYA_Error nya_url_parse_target(const char* text, u64 size, NYA_Url* out_url, NYA_UrlFailure* out_failure) {
    nya_assert(text != nullptr || size == 0);
    nya_assert(out_url != nullptr);

    NYA_UrlFailure  local   = { 0 };
    NYA_UrlFailure* failure = out_failure != nullptr ? out_failure : &local;

    *failure = (NYA_UrlFailure){ 0 };
    *out_url = (NYA_Url){ 0 };

    if (!_nya_url_parse_origin(text, size, out_url, failure)) return _nya_url_refused(out_url, failure);

    nya_assert(out_url->scheme == NYA_URL_SCHEME_NONE && out_url->host_kind == NYA_URL_HOST_NONE);
    nya_assert(out_url->path.length > 0 && !out_url->has_fragment && !out_url->has_port && !out_url->has_userinfo);
    nya_assert(out_url->length <= size, "the stored components are longer than the input");

    return NYA_OK;
}

NYA_Error nya_url_format(const NYA_Url* url, char* buffer, u64 capacity, u64* out_length) {
    nya_assert(url != nullptr);
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);
    nya_assert(out_length != nullptr);
    nya_assert(url->scheme < NYA_URL_SCHEME_COUNT && url->host_kind < NYA_URL_HOST_COUNT);
    _nya_url_assert_span(url, url->host);
    _nya_url_assert_span(url, url->path);
    _nya_url_assert_span(url, url->query);
    _nya_url_assert_span(url, url->fragment);

    buffer[0]   = '\0';
    *out_length = 0;

    u64 written = 0;
    b8  fits    = true;

    if (url->scheme != NYA_URL_SCHEME_NONE) {
        NYA_ConstCString scheme = _NYA_URL_SCHEME_TEXT[url->scheme];

        fits = fits && _nya_url_append(buffer, capacity, &written, scheme, strlen(scheme));
        fits = fits && _nya_url_append(buffer, capacity, &written, "://", 3);

        if (url->host_kind == NYA_URL_HOST_IPV6) fits = fits && _nya_url_append(buffer, capacity, &written, "[", 1);
        fits = fits && _nya_url_append(buffer, capacity, &written, url->text + url->host.offset, url->host.length);
        if (url->host_kind == NYA_URL_HOST_IPV6) fits = fits && _nya_url_append(buffer, capacity, &written, "]", 1);

        if (url->has_port) {
            char port_text[NYA_URL_PORT_MAX_DIGITS + 2] = { 0 };

            s32 length = snprintf(port_text, sizeof(port_text), ":%u", (unsigned)url->port);
            nya_assert(length > 0 && (u64)length < sizeof(port_text));

            fits = fits && _nya_url_append(buffer, capacity, &written, port_text, (u64)length);
        }
    }

    fits = fits && _nya_url_append(buffer, capacity, &written, url->text + url->path.offset, url->path.length);

    if (url->has_query) {
        fits = fits && _nya_url_append(buffer, capacity, &written, "?", 1);
        fits = fits && _nya_url_append(buffer, capacity, &written, url->text + url->query.offset, url->query.length);
    }

    if (url->has_fragment) {
        fits = fits && _nya_url_append(buffer, capacity, &written, "#", 1);
        fits = fits && _nya_url_append(buffer, capacity, &written, url->text + url->fragment.offset, url->fragment.length);
    }

    if (!fits) {
        buffer[0] = '\0';
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a url does not fit %llu bytes", (unsigned long long)capacity);
    }

    nya_assert(written < capacity);

    buffer[written] = '\0';
    *out_length     = written;

    return NYA_OK;
}

NYA_Error nya_url_path_decode(const NYA_Url* url, char* buffer, u64 capacity, u64* out_length) {
    nya_assert(url != nullptr);
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);
    nya_assert(out_length != nullptr);
    _nya_url_assert_span(url, url->path);

    buffer[0]   = '\0';
    *out_length = 0;

    u64 length = 0;
    u64 offset = 0;

    // capacity - 1 keeps a byte for the terminator.
    if (!_nya_url_decode_run(url->text + url->path.offset, url->path.length, false, (u8*)buffer, capacity - 1, &length, &offset)) {
        buffer[0] = '\0';

        if (offset < url->path.length) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a path with a malformed escape was not parsed");

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a path does not fit %llu bytes decoded", (unsigned long long)capacity);
    }

    // parse refused a decoded NUL, so one here means the struct was written by hand; it would cut the path
    // short for every C string reader after this.
    if (memchr(buffer, '\0', length) != nullptr) {
        buffer[0] = '\0';
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a path decodes to a NUL");
    }

    buffer[length] = '\0';
    *out_length    = length;

    return NYA_OK;
}

NYA_Error nya_url_query_find(const NYA_Url* url, NYA_ConstCString name, char* buffer, u64 capacity, b8* out_found) {
    nya_assert(url != nullptr);
    nya_assert(name != nullptr);
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);
    nya_assert(out_found != nullptr);
    _nya_url_assert_span(url, url->query);

    buffer[0]  = '\0';
    *out_found = false;

    const char* query = url->text + url->query.offset;
    u64         size  = url->query.length;

    b8  found       = false;
    u64 value_start = 0;
    u64 value_end   = 0;

    // bounded by the query's length: every pass moves the cursor forward by at least one byte.
    u64 cursor = 0;
    while (cursor < size) {
        u64 pair_end = cursor;
        while (pair_end < size && query[pair_end] != '&') pair_end++;

        u64 equals = cursor;
        while (equals < pair_end && query[equals] != '=') equals++;

        // "a&&b" and a trailing '&' hold empty pairs, which name nothing.
        if (pair_end > cursor && _nya_url_form_equals(query + cursor, equals - cursor, name)) {
            if (found) return nya_error(NYA_ERROR_PARSE, "the query names '%s' more than once", name);

            found       = true;
            value_start = equals < pair_end ? equals + 1 : pair_end;
            value_end   = pair_end;
        }

        cursor = pair_end + 1;
    }

    if (!found) return NYA_OK;

    u64 length = 0;
    u64 offset = 0;

    if (!_nya_url_decode_run(query + value_start, value_end - value_start, true, (u8*)buffer, capacity - 1, &length, &offset)) {
        buffer[0] = '\0';

        if (offset < value_end - value_start) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a query with a malformed escape was not parsed");

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the value of '%s' does not fit %llu bytes", name, (unsigned long long)capacity);
    }

    if (memchr(buffer, '\0', length) != nullptr) {
        buffer[0] = '\0';
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the value of '%s' decodes to a NUL", name);
    }

    buffer[length] = '\0';
    *out_found     = true;

    return NYA_OK;
}

NYA_ConstCString nya_url_rule_text(NYA_UrlRule rule) {
    nya_assert(rule < NYA_URL_RULE_COUNT);

    return _NYA_URL_RULE_TEXT[rule];
}

NYA_Error nya_percent_encode(const u8* data, u64 size, char* buffer, u64 capacity, u64* out_length) {
    nya_assert(data != nullptr || size == 0);
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);
    nya_assert(out_length != nullptr);

    buffer[0]   = '\0';
    *out_length = 0;

    u64 written = 0;

    for (u64 index = 0; index < size; index++) {
        char character = (char)data[index];
        u64  needed    = _nya_url_is_unreserved(character) ? 1 : 3;

        // written < capacity holds throughout, so this cannot wrap; >= keeps the terminator's byte.
        if (needed >= capacity - written) {
            buffer[0] = '\0';
            return nya_error(NYA_ERROR_OUT_OF_MEMORY, "percent encoding %llu bytes does not fit %llu", (unsigned long long)size, (unsigned long long)capacity);
        }

        if (needed == 1) {
            buffer[written++] = character;
            continue;
        }

        buffer[written++] = '%';
        buffer[written++] = _NYA_URL_HEX_UPPER[data[index] >> 4];
        buffer[written++] = _NYA_URL_HEX_UPPER[data[index] & 0x0F];
    }

    buffer[written] = '\0';
    *out_length     = written;

    // at most three bytes out per byte in, written as a division so the check itself cannot wrap.
    nya_assert(written >= size && written / 3 <= size);

    return NYA_OK;
}

NYA_Error nya_percent_decode(const char* text, u64 size, u8* buffer, u64 capacity, u64* out_length) {
    nya_assert(text != nullptr || size == 0);
    nya_assert(buffer != nullptr || capacity == 0);
    nya_assert(out_length != nullptr);

    *out_length = 0;

    u64 length = 0;
    u64 offset = 0;

    if (!_nya_url_decode_run(text, size, false, buffer, capacity, &length, &offset)) {
        if (offset < size) return nya_error(NYA_ERROR_PARSE, "a malformed percent escape at byte %llu", (unsigned long long)offset);

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "percent decoding %llu bytes does not fit %llu", (unsigned long long)size, (unsigned long long)capacity);
    }

    nya_assert(length <= size, "a decode grew");
    *out_length = length;

    return NYA_OK;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_url_parse_absolute(const char* text, u64 size, NYA_Url* url, NYA_UrlFailure* failure) {
    if (size == 0) return _nya_url_refuse(failure, NYA_URL_RULE_EMPTY, 0);
    if (size > NYA_URL_MAX_BYTES) return _nya_url_refuse(failure, NYA_URL_RULE_TOO_LONG, NYA_URL_MAX_BYTES);
    if (!_nya_url_check_alphabet(text, size, failure)) return false;

    // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ), and then a ':'.
    u64 colon = 0;
    if (_nya_url_is_alpha(text[0])) {
        colon = 1;
        while (colon < size && _nya_url_is_scheme_char(text[colon])) colon++;
    }

    if (colon == 0 || colon >= size || text[colon] != ':') return _nya_url_refuse(failure, NYA_URL_RULE_SCHEME_MISSING, 0);

    for (u32 scheme = NYA_URL_SCHEME_NONE + 1; scheme < NYA_URL_SCHEME_COUNT; scheme++) {
        NYA_ConstCString known = _NYA_URL_SCHEME_TEXT[scheme];

        // folded one way only: the table is lower case, and a scheme is case insensitive (RFC 3986 3.1).
        u64 matched = 0;
        while (matched < colon && known[matched] != '\0' && (text[matched] | 0x20) == known[matched]) matched++;

        if (matched == colon && known[matched] == '\0') url->scheme = (NYA_UrlScheme)scheme;
    }

    if (url->scheme == NYA_URL_SCHEME_NONE) return _nya_url_refuse(failure, NYA_URL_RULE_SCHEME_UNKNOWN, 0);

    u64 authority = colon + 1;

    if (size - authority < 2 || text[authority] != '/' || text[authority + 1] != '/') {
        return _nya_url_refuse(failure, NYA_URL_RULE_AUTHORITY_MISSING, authority);
    }

    authority += 2;

    u64 authority_end = authority;
    while (authority_end < size && text[authority_end] != '/' && text[authority_end] != '?' && text[authority_end] != '#') authority_end++;

    if (!_nya_url_parse_authority(text, authority, authority_end, url, failure)) return false;

    return _nya_url_parse_rest(text, authority_end, size, true, url, failure);
}

b8 _nya_url_parse_origin(const char* text, u64 size, NYA_Url* url, NYA_UrlFailure* failure) {
    if (size == 0) return _nya_url_refuse(failure, NYA_URL_RULE_EMPTY, 0);
    if (size > NYA_URL_MAX_BYTES) return _nya_url_refuse(failure, NYA_URL_RULE_TOO_LONG, NYA_URL_MAX_BYTES);
    if (!_nya_url_check_alphabet(text, size, failure)) return false;

    // "*", the absolute form and the authority form are for OPTIONS to a whole server, proxies and
    // CONNECT, and a server that is none of those takes only the origin form.
    if (text[0] != '/') return _nya_url_refuse(failure, NYA_URL_RULE_PATH_NOT_ABSOLUTE, 0);

    return _nya_url_parse_rest(text, 0, size, false, url, failure);
}

b8 _nya_url_refuse(NYA_UrlFailure* failure, NYA_UrlRule rule, u64 offset) {
    nya_assert(rule != NYA_URL_RULE_NONE && rule < NYA_URL_RULE_COUNT);
    nya_assert(offset <= NYA_URL_MAX_BYTES, "an offset past the longest url accepted");

    failure->rule   = rule;
    failure->offset = (u32)offset;

    return false;
}

NYA_Error _nya_url_refused(NYA_Url* url, const NYA_UrlFailure* failure) {
    nya_assert(failure->rule != NYA_URL_RULE_NONE && failure->rule < NYA_URL_RULE_COUNT);

    *url = (NYA_Url){ 0 };

    return nya_error(NYA_ERROR_PARSE, "not a url: %s at byte %u", _NYA_URL_RULE_TEXT[failure->rule], failure->offset);
}

b8 _nya_url_hex_value(char character, u8* out_value) {
    if (character >= '0' && character <= '9') {
        *out_value = (u8)(character - '0');
        return true;
    }

    if (character >= 'a' && character <= 'f') {
        *out_value = (u8)(character - 'a' + 10);
        return true;
    }

    if (character >= 'A' && character <= 'F') {
        *out_value = (u8)(character - 'A' + 10);
        return true;
    }

    return false;
}

b8 _nya_url_is_alpha(char character) { return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z'); }

b8 _nya_url_is_digit(char character) { return character >= '0' && character <= '9'; }

b8 _nya_url_is_hex(char character) {
    u8 value = 0;
    return _nya_url_hex_value(character, &value);
}

b8 _nya_url_is_unreserved(char character) {
    return _nya_url_is_alpha(character) || _nya_url_is_digit(character) || character == '-' || character == '.' || character == '_' || character == '~';
}

b8 _nya_url_is_sub_delim(char character) {
    switch (character) {
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=': return true;
        default:  return false;
    }
}

b8 _nya_url_is_scheme_char(char character) {
    return _nya_url_is_alpha(character) || _nya_url_is_digit(character) || character == '+' || character == '-' || character == '.';
}

b8 _nya_url_decode_next(const char* text, u64 size, u64* index, b8 plus_is_space, u8* out_byte) {
    nya_assert(*index < size);

    char character = text[*index];

    if (character == '%') {
        u8 high = 0;
        u8 low  = 0;

        // size - *index rather than *index + 2, so nothing is ever added to an index before the compare.
        if (size - *index <= 2) return false;
        if (!_nya_url_hex_value(text[*index + 1], &high) || !_nya_url_hex_value(text[*index + 2], &low)) return false;

        *out_byte  = (u8)((u32)high * 16U + (u32)low);
        *index    += 3;

        return true;
    }

    *out_byte  = plus_is_space && character == '+' ? (u8)' ' : (u8)character;
    *index    += 1;

    return true;
}

b8 _nya_url_decode_run(const char* text, u64 size, b8 plus_is_space, u8* buffer, u64 capacity, u64* out_length, u64* out_offset) {
    u64 index   = 0;
    u64 written = 0;

    *out_length = 0;
    *out_offset = size;

    while (index < size) {
        u64 at   = index;
        u8  byte = 0;

        if (!_nya_url_decode_next(text, size, &index, plus_is_space, &byte)) {
            *out_offset = at;
            return false;
        }

        if (written >= capacity) return false;

        buffer[written++] = byte;
    }

    *out_length = written;

    return true;
}

b8 _nya_url_form_equals(const char* text, u64 size, NYA_ConstCString name) {
    u64 index    = 0;
    u64 position = 0;

    while (index < size) {
        u8 byte = 0;

        if (!_nya_url_decode_next(text, size, &index, true, &byte)) return false;
        if (name[position] == '\0' || (u8)name[position] != byte) return false;

        position++;
    }

    return name[position] == '\0';
}

b8 _nya_url_check_alphabet(const char* text, u64 size, NYA_UrlFailure* failure) {
    for (u64 index = 0; index < size; index++) {
        char character = text[index];
        u8   byte      = (u8)character;

        if (byte < 0x20 || byte == 0x7F) return _nya_url_refuse(failure, NYA_URL_RULE_CONTROL_CHARACTER, index);
        if (byte == ' ') return _nya_url_refuse(failure, NYA_URL_RULE_SPACE, index);

        // unreserved, sub-delims, gen-delims and '%': RFC 3986's whole alphabet. Where each may appear is
        // the component checks' business.
        b8 known = _nya_url_is_unreserved(character) || _nya_url_is_sub_delim(character) || character == '%' || character == ':' ||
                   character == '/' || character == '?' || character == '#' || character == '[' || character == ']' || character == '@';

        if (!known) return _nya_url_refuse(failure, NYA_URL_RULE_CHARACTER, index);
    }

    return true;
}

b8 _nya_url_check_component(const char* text, u64 start, u64 end, NYA_ConstCString extra, NYA_UrlFailure* failure) {
    nya_assert(start <= end);

    u64 index = start;

    while (index < end) {
        u64  at        = index;
        char character = text[index];

        if (character != '%') {
            b8 allowed = _nya_url_is_unreserved(character) || _nya_url_is_sub_delim(character) || character == ':' || character == '@' ||
                         (character != '\0' && strchr(extra, character) != nullptr);

            if (!allowed) return _nya_url_refuse(failure, NYA_URL_RULE_CHARACTER, at);

            index++;
            continue;
        }

        u8 byte = 0;
        if (!_nya_url_decode_next(text, end, &index, false, &byte)) return _nya_url_refuse(failure, NYA_URL_RULE_PERCENT_MALFORMED, at);

        // every C string reader after this would stop at it and see a different value than a length aware one.
        if (byte == 0) return _nya_url_refuse(failure, NYA_URL_RULE_CONTROL_CHARACTER, at);
    }

    return true;
}

b8 _nya_url_check_path(const char* text, u64 start, u64 end, NYA_UrlFailure* failure) {
    nya_assert(start <= end);

    if (start == end) return true;
    if (text[start] != '/') return _nya_url_refuse(failure, NYA_URL_RULE_PATH_NOT_ABSOLUTE, start);
    if (end - start >= 2 && text[start + 1] == '/') return _nya_url_refuse(failure, NYA_URL_RULE_PATH_DOUBLE_SLASH, start);

    u64 index = start;

    // one segment per pass; each starts on a '/', and the inner loop stops on the next or at the end.
    while (index < end) {
        nya_assert(text[index] == '/');
        index++;

        u64 segment   = index;
        u64 decoded   = 0;
        b8  only_dots = true;

        while (index < end && text[index] != '/') {
            u64  at        = index;
            char character = text[index];

            if (character != '%') {
                b8 allowed = _nya_url_is_unreserved(character) || _nya_url_is_sub_delim(character) || character == ':' || character == '@';
                if (!allowed) return _nya_url_refuse(failure, NYA_URL_RULE_CHARACTER, at);
            }

            u8 byte = 0;
            if (!_nya_url_decode_next(text, end, &index, false, &byte)) return _nya_url_refuse(failure, NYA_URL_RULE_PERCENT_MALFORMED, at);

            if (byte == '/') return _nya_url_refuse(failure, NYA_URL_RULE_PATH_ENCODED_SLASH, at);
            if (byte < 0x20 || byte == 0x7F) return _nya_url_refuse(failure, NYA_URL_RULE_CONTROL_CHARACTER, at);

            only_dots = only_dots && byte == '.';
            decoded++;
        }

        // after decoding, so "%2e%2e" is caught by the same rule as "..". Refused rather than collapsed: a
        // path that meant to climb names no resource, and normalising it would invent one.
        if (only_dots && (decoded == 1 || decoded == 2)) return _nya_url_refuse(failure, NYA_URL_RULE_PATH_DOT_SEGMENT, segment);
    }

    return true;
}

b8 _nya_url_is_ipv4(const char* text, u64 size) {
    u64 index = 0;

    for (u32 octet = 0; octet < _NYA_URL_IPV4_OCTETS; octet++) {
        if (octet > 0) {
            if (index >= size || text[index] != '.') return false;
            index++;
        }

        u64 start = index;
        u32 value = 0;

        // three digits at most, so the value stays below 1000 and cannot wrap.
        while (index < size && _nya_url_is_digit(text[index]) && index - start < _NYA_URL_IPV4_OCTET_MAX_DIGITS) {
            value = value * 10U + (u32)(text[index] - '0');
            index++;
        }

        u64 digits = index - start;

        if (digits == 0 || value > _NYA_URL_IPV4_OCTET_MAX) return false;
        // "010" is ten to one resolver and eight to another.
        if (digits > 1 && text[start] == '0') return false;
    }

    return index == size;
}

b8 _nya_url_is_ipv6(const char* text, u64 size) {
    if (size < 2 || size > NYA_URL_IPV6_MAX_BYTES) return false;

    u32 groups     = 0;
    b8  compressed = false;
    u64 index      = 0;

    if (text[0] == ':') {
        if (text[1] != ':') return false;

        compressed = true;
        index      = 2;
    }

    // every pass consumes at least one byte or returns, so this is bounded by size.
    while (index < size) {
        u64 start = index;
        while (index < size && _nya_url_is_hex(text[index])) index++;

        if (index < size && text[index] == '.') {
            // a dotted quad is the last thing in the address and stands for two groups.
            if (groups + _NYA_URL_IPV6_IPV4_GROUPS > _NYA_URL_IPV6_GROUPS) return false;
            if (!_nya_url_is_ipv4(text + start, size - start)) return false;

            groups += _NYA_URL_IPV6_IPV4_GROUPS;
            break;
        }

        u64 digits = index - start;
        if (digits == 0 || digits > _NYA_URL_IPV6_GROUP_MAX_DIGITS) return false;

        groups++;
        if (groups > _NYA_URL_IPV6_GROUPS) return false;

        if (index == size) break;
        if (text[index] != ':') return false;

        index++;

        if (index < size && text[index] == ':') {
            if (compressed) return false;

            compressed = true;
            index++;
        } else if (index == size) {
            // "1:" ends on a group that is not there.
            return false;
        }
    }

    // "::" stands for at least one group of zeros.
    return compressed ? groups < _NYA_URL_IPV6_GROUPS : groups == _NYA_URL_IPV6_GROUPS;
}

b8 _nya_url_check_host(const char* text, u64 start, u64 end, NYA_UrlHostKind* out_kind, NYA_UrlFailure* failure) {
    nya_assert(start < end);

    if (end - start > NYA_URL_HOST_MAX_BYTES) return _nya_url_refuse(failure, NYA_URL_RULE_HOST_TOO_LONG, start);

    u64 label      = start;
    u64 last_label = start;

    // one label per pass; `label` passes `end` only after the last one.
    while (label <= end) {
        u64 label_end = label;
        while (label_end < end && text[label_end] != '.') label_end++;

        u64 length = label_end - label;

        // "a..b" and a trailing dot name the same host to DNS as their tidy spelling and a different one to a comparison.
        if (length == 0 || length > NYA_URL_HOST_LABEL_MAX_BYTES) return _nya_url_refuse(failure, NYA_URL_RULE_HOST_MALFORMED, label);
        if (text[label] == '-' || text[label_end - 1] == '-') return _nya_url_refuse(failure, NYA_URL_RULE_HOST_MALFORMED, label);

        for (u64 index = label; index < label_end; index++) {
            char character = text[index];

            // no escapes in a host: a percent-encoded name is how a filter and a resolver come to disagree.
            if (!_nya_url_is_alpha(character) && !_nya_url_is_digit(character) && character != '-' && character != '_') {
                return _nya_url_refuse(failure, NYA_URL_RULE_HOST_MALFORMED, index);
            }
        }

        last_label = label;
        label      = label_end + 1;
    }

    /*
     * A last label that is a number makes the whole host an address to a resolver, which reads "1.2.3",
     * "2130706433" and "0x7f.1" as addresses by rules of its own. Only the dotted quad is accepted, so the
     * host a filter compares is the address the socket connects to.
     */
    b8 numeric = true;
    for (u64 index = last_label; index < end && numeric; index++) numeric = _nya_url_is_digit(text[index]);

    b8 hex = end - last_label >= 2 && text[last_label] == '0' && (text[last_label + 1] | 0x20) == 'x';

    if (!numeric && !hex) {
        *out_kind = NYA_URL_HOST_NAME;
        return true;
    }

    if (!_nya_url_is_ipv4(text + start, end - start)) return _nya_url_refuse(failure, NYA_URL_RULE_HOST_MALFORMED, start);

    *out_kind = NYA_URL_HOST_IPV4;
    return true;
}

b8 _nya_url_parse_authority(const char* text, u64 start, u64 end, NYA_Url* url, NYA_UrlFailure* failure) {
    nya_assert(start <= end);

    if (start == end) return _nya_url_refuse(failure, NYA_URL_RULE_HOST_EMPTY, start);

    u64 at = start;
    while (at < end && text[at] != '@') at++;

    u64 host_start = start;

    // a second '@' is not in the host's alphabet, so "a@b@c" is refused there rather than guessed at.
    if (at < end) {
        // checked but never copied; see "userinfo is noticed, never kept" in the header.
        if (!_nya_url_check_component(text, start, at, "", failure)) return false;

        url->has_userinfo = true;
        host_start        = at + 1;
    }

    u64 port_colon = end;

    if (host_start < end && text[host_start] == '[') {
        u64 close = host_start + 1;
        while (close < end && text[close] != ']') close++;

        if (close >= end || !_nya_url_is_ipv6(text + host_start + 1, close - host_start - 1)) {
            return _nya_url_refuse(failure, NYA_URL_RULE_IPV6_MALFORMED, host_start);
        }

        if (close + 1 < end && text[close + 1] != ':') return _nya_url_refuse(failure, NYA_URL_RULE_HOST_MALFORMED, close + 1);

        url->host_kind = NYA_URL_HOST_IPV6;
        url->host      = _nya_url_store(url, text, host_start + 1, close);
        port_colon     = close + 1;
    } else {
        u64 host_end = host_start;
        while (host_end < end && text[host_end] != ':') host_end++;

        if (host_end == host_start) return _nya_url_refuse(failure, NYA_URL_RULE_HOST_EMPTY, host_start);

        NYA_UrlHostKind kind = NYA_URL_HOST_NONE;
        if (!_nya_url_check_host(text, host_start, host_end, &kind, failure)) return false;

        url->host_kind = kind;
        url->host      = _nya_url_store(url, text, host_start, host_end);
        port_colon     = host_end;
    }

    if (port_colon >= end) return true;

    nya_assert(text[port_colon] == ':');

    u64 digits_start = port_colon + 1;
    u64 digits       = end - digits_start;

    // "host:" means the default port to RFC 3986; refused so one port has one spelling. Same for "0080".
    if (digits == 0) return _nya_url_refuse(failure, NYA_URL_RULE_PORT_MALFORMED, port_colon);
    if (digits > 1 && text[digits_start] == '0') return _nya_url_refuse(failure, NYA_URL_RULE_PORT_MALFORMED, digits_start);

    for (u64 index = digits_start; index < end; index++) {
        if (!_nya_url_is_digit(text[index])) return _nya_url_refuse(failure, NYA_URL_RULE_PORT_MALFORMED, index);
    }

    if (digits > NYA_URL_PORT_MAX_DIGITS) return _nya_url_refuse(failure, NYA_URL_RULE_PORT_OUT_OF_RANGE, digits_start);

    // five digits at most, so this stays below 100000 and cannot wrap.
    u32 port = 0;
    for (u64 index = digits_start; index < end; index++) port = port * 10U + (u32)(text[index] - '0');

    if (port == 0 || port > _NYA_URL_PORT_MAX) return _nya_url_refuse(failure, NYA_URL_RULE_PORT_OUT_OF_RANGE, digits_start);

    url->has_port = true;
    url->port     = (u16)port;

    return true;
}

b8 _nya_url_parse_rest(const char* text, u64 start, u64 size, b8 fragment_allowed, NYA_Url* url, NYA_UrlFailure* failure) {
    nya_assert(start <= size);

    u64 path_end = start;
    while (path_end < size && text[path_end] != '?' && text[path_end] != '#') path_end++;

    // no client sends a fragment in a request target, so one there is not a request from a client.
    if (!fragment_allowed && path_end < size && text[path_end] == '#') return _nya_url_refuse(failure, NYA_URL_RULE_CHARACTER, path_end);
    if (!_nya_url_check_path(text, start, path_end, failure)) return false;

    url->path = _nya_url_store(url, text, start, path_end);

    u64 cursor = path_end;

    if (cursor < size && text[cursor] == '?') {
        u64 query_end = cursor + 1;
        while (query_end < size && text[query_end] != '#') query_end++;

        if (!fragment_allowed && query_end < size) return _nya_url_refuse(failure, NYA_URL_RULE_CHARACTER, query_end);
        if (!_nya_url_check_component(text, cursor + 1, query_end, "/?", failure)) return false;

        url->has_query = true;
        url->query     = _nya_url_store(url, text, cursor + 1, query_end);
        cursor         = query_end;
    }

    if (cursor < size) {
        nya_assert(fragment_allowed && text[cursor] == '#');

        // a second '#' is not in the fragment's alphabet, so the component check refuses it.
        if (!_nya_url_check_component(text, cursor + 1, size, "/?", failure)) return false;

        url->has_fragment = true;
        url->fragment     = _nya_url_store(url, text, cursor + 1, size);
    }

    return true;
}

NYA_UrlSpan _nya_url_store(NYA_Url* url, const char* text, u64 start, u64 end) {
    nya_assert(start <= end);
    nya_assert(url->length <= NYA_URL_MAX_BYTES);
    nya_assert(end - start <= (u64)NYA_URL_MAX_BYTES - url->length, "the components outgrew the input they came from");

    NYA_UrlSpan span = { .offset = url->length, .length = (u16)(end - start) };

    nya_memcpy(url->text + url->length, text + start, end - start);
    url->length = (u16)(url->length + span.length);

    return span;
}

b8 _nya_url_append(char* buffer, u64 capacity, u64* written, const char* piece, u64 length) {
    nya_assert(*written < capacity);

    // *written < capacity, so the subtraction cannot wrap; >= keeps the terminator's byte.
    if (length >= capacity - *written) return false;

    nya_memcpy(buffer + *written, piece, length);
    *written += length;

    return true;
}

void _nya_url_assert_span(const NYA_Url* url, NYA_UrlSpan span) {
    nya_assert(url->length <= NYA_URL_MAX_BYTES);
    nya_assert((u32)span.offset + (u32)span.length <= (u32)url->length, "a span reaches past the stored text");
}
