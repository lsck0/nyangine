#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-core/http/http_cookie.h"
#include "nyangine-core/http/http_message.h"

// PRIVATE API DECLARATION

/**
 * A token character, as RFC 9110 spells it. Its own copy rather than http_message.c's: that one is
 * private to its file, and a cookie name is checked here whatever order the unity build uses.
 * */
NYA_INTERNAL b8 _nya_http_cookie_name_char(char character) __attr_no_discard;

/** RFC 6265's cookie-octet: printable ASCII without space, quote, comma, semicolon and backslash. */
NYA_INTERNAL b8 _nya_http_cookie_value_char(char character) __attr_no_discard;

/** Whether every byte of `text` may appear in a value, and there is at least one of them. */
NYA_INTERNAL b8 _nya_http_cookie_value_is(NYA_ConstCString text, u64 size) __attr_no_discard;

/** Whether `text` is a token, which is what a name and an attribute's value both have to be. */
NYA_INTERNAL b8 _nya_http_cookie_token_is(NYA_ConstCString text) __attr_no_discard;

/** Whether a path or a domain is safe to put in the header: no control byte, no semicolon, no comma. */
NYA_INTERNAL b8 _nya_http_cookie_attribute_is(NYA_ConstCString text) __attr_no_discard;

// PUBLIC API IMPLEMENTATION

b8 nya_http_cookie_parse(const char* header, u64 size, NYA_HttpCookieValue* out_names, NYA_HttpCookieValue* out_values, u32* out_count) {
    nya_assert(out_names != nullptr && out_values != nullptr && out_count != nullptr);

    *out_count = 0;

    if (header == nullptr || size == 0) return false;

    u32 count = 0;
    u64 at    = 0;

    while (at < size) {
        // one space after a separator, and exactly one: "a=1;  b=2" is two readers' worth of guessing.
        if (count > 0) {
            if (at >= size || header[at] != ';') return false;
            at++;

            if (at >= size || header[at] != ' ') return false;
            at++;
        }

        u64 name_start = at;
        while (at < size && header[at] != '=' && header[at] != ';') at++;

        if (at >= size || header[at] != '=') return false;

        u64 name_size = at - name_start;
        at++;

        u64 value_start = at;
        while (at < size && header[at] != ';') at++;

        u64 value_size = at - value_start;

        // a quoted value is legal in the grammar and one more thing two parsers can read differently, so it's refused here: what goes in comes out byte for byte.
        if (name_size == 0 || name_size >= NYA_HTTP_MAX_COOKIE_NAME || value_size >= NYA_HTTP_MAX_COOKIE_VALUE) return false;

        for (u64 index = name_start; index < name_start + name_size; index++) {
            if (!_nya_http_cookie_name_char(header[index])) return false;
        }

        for (u64 index = value_start; index < value_start + value_size; index++) {
            if (!_nya_http_cookie_value_char(header[index])) return false;
        }

        // the same name twice is two answers to one question, and which one wins is the parser's opinion.
        for (u32 index = 0; index < count; index++) {
            if (out_names[index].size == name_size && memcmp(out_names[index].text, header + name_start, name_size) == 0) return false;
        }

        if (count >= NYA_HTTP_MAX_COOKIES) return false;

        out_names[count]  = (NYA_HttpCookieValue){ .text = header + name_start, .size = name_size };
        out_values[count] = (NYA_HttpCookieValue){ .text = header + value_start, .size = value_size };
        count++;
    }

    if (count == 0) return false;

    *out_count = count;

    return true;
}

b8 nya_http_cookie_read(const NYA_HttpRequest* request, NYA_ConstCString name, NYA_HttpCookieValue* out_value) {
    nya_assert(name != nullptr && out_value != nullptr);

    *out_value = (NYA_HttpCookieValue){ 0 };

    NYA_ConstCString header = nya_http_request_header(request, "cookie");
    if (header == nullptr) return false;

    NYA_HttpCookieValue names[NYA_HTTP_MAX_COOKIES]  = { 0 };
    NYA_HttpCookieValue values[NYA_HTTP_MAX_COOKIES] = { 0 };
    u32                 count                        = 0;

    if (!nya_http_cookie_parse(header, strlen(header), names, values, &count)) return false;

    u64 wanted = strlen(name);

    for (u32 index = 0; index < count; index++) {
        if (names[index].size != wanted || memcmp(names[index].text, name, wanted) != 0) continue;

        *out_value = values[index];

        return true;
    }

    return false;
}

u32 nya_http_cookie_count(const NYA_HttpRequest* request) {
    NYA_ConstCString header = nya_http_request_header(request, "cookie");
    if (header == nullptr) return 0;

    NYA_HttpCookieValue names[NYA_HTTP_MAX_COOKIES]  = { 0 };
    NYA_HttpCookieValue values[NYA_HTTP_MAX_COOKIES] = { 0 };
    u32                 count                        = 0;

    return nya_http_cookie_parse(header, strlen(header), names, values, &count) ? count : 0;
}

b8 nya_http_cookie_at(const NYA_HttpRequest* request, u32 index, NYA_HttpCookieValue* out_name, NYA_HttpCookieValue* out_value) {
    nya_assert(out_name != nullptr && out_value != nullptr);

    *out_name  = (NYA_HttpCookieValue){ 0 };
    *out_value = (NYA_HttpCookieValue){ 0 };

    NYA_ConstCString header = nya_http_request_header(request, "cookie");
    if (header == nullptr) return false;

    NYA_HttpCookieValue names[NYA_HTTP_MAX_COOKIES]  = { 0 };
    NYA_HttpCookieValue values[NYA_HTTP_MAX_COOKIES] = { 0 };
    u32                 count                        = 0;

    if (!nya_http_cookie_parse(header, strlen(header), names, values, &count) || index >= count) return false;

    *out_name  = names[index];
    *out_value = values[index];

    return true;
}

NYA_Error nya_http_response_cookie(NYA_HttpResponse* response, const NYA_HttpCookie* cookie) {
    nya_assert(response != nullptr);
    nya_assert(cookie != nullptr);

    if (cookie->name == nullptr || !_nya_http_cookie_token_is(cookie->name)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a cookie name is a token");
    }

    NYA_ConstCString value = cookie->value != nullptr ? cookie->value : "";
    u64              size  = strlen(value);

    if (size >= NYA_HTTP_MAX_COOKIE_VALUE || !_nya_http_cookie_value_is(value, size)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the value of '%s' is not a cookie value; encode it first", cookie->name);
    }

    NYA_ConstCString path = cookie->path != nullptr ? cookie->path : "/";

    if (!_nya_http_cookie_attribute_is(path)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the path of '%s' is not one", cookie->name);
    if (cookie->domain != nullptr && !_nya_http_cookie_attribute_is(cookie->domain)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the domain of '%s' is not one", cookie->name);
    }

    // a prefix a browser enforces, enforced here too: a name claiming one and not keeping it is silently ignored by the browser — a session that never arrives with nothing saying why.
    b8 host_prefix   = strncmp(cookie->name, "__Host-", 7) == 0;
    b8 secure_prefix = strncmp(cookie->name, "__Secure-", 9) == 0;

    if ((host_prefix || secure_prefix) && !cookie->secure) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' claims a prefix that demands Secure", cookie->name);
    }

    if (host_prefix && (cookie->domain != nullptr || strcmp(path, "/") != 0)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is a __Host- cookie, so it takes no Domain and Path=/", cookie->name);
    }

    if (cookie->same_site == NYA_HTTP_SAME_SITE_NONE && !cookie->secure) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' is SameSite=None, which a browser only keeps with Secure", cookie->name);
    }

    NYA_ConstCString same_site = cookie->same_site == NYA_HTTP_SAME_SITE_LAX    ? "Lax"
                                 : cookie->same_site == NYA_HTTP_SAME_SITE_NONE ? "None"
                                                                                : "Strict";

    char rendered[NYA_HTTP_MAX_HEADER_VALUE] = { 0 };

    s32 written = snprintf(rendered, sizeof(rendered), "%s=%s; Path=%s; SameSite=%s", cookie->name, value, path, same_site);
    if (written < 0 || (u64)written >= sizeof(rendered)) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "'%s' does not fit a header", cookie->name);

    u64 length = (u64)written;

    if (cookie->domain != nullptr) {
        written = snprintf(rendered + length, sizeof(rendered) - length, "; Domain=%s", cookie->domain);
        if (written < 0 || (u64)written >= sizeof(rendered) - length) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "'%s' does not fit a header", cookie->name);

        length += (u64)written;
    }

    if (cookie->max_age_s >= 0) {
        // Max-Age rather than Expires: the same instruction without a date format to disagree about.
        written = snprintf(rendered + length, sizeof(rendered) - length, "; Max-Age=%lld", (long long)cookie->max_age_s);
        if (written < 0 || (u64)written >= sizeof(rendered) - length) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "'%s' does not fit a header", cookie->name);

        length += (u64)written;
    }

    if (cookie->secure) {
        if (length + 8 >= sizeof(rendered)) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "'%s' does not fit a header", cookie->name);

        memcpy(rendered + length, "; Secure", 8);
        length += 8;
    }

    if (cookie->http_only) {
        if (length + 10 >= sizeof(rendered)) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "'%s' does not fit a header", cookie->name);

        memcpy(rendered + length, "; HttpOnly", 10);
        length += 10;
    }

    rendered[length] = '\0';

    return nya_http_response_header(response, "Set-Cookie", rendered);
}

NYA_Error nya_http_response_cookie_clear(NYA_HttpResponse* response, NYA_ConstCString name, NYA_ConstCString path, b8 secure) {
    return nya_http_response_cookie(response,
                                    &(NYA_HttpCookie){
                                        .name      = name,
                                        .value     = "",
                                        .max_age_s = 0,
                                        .http_only = true,
                                        .secure    = secure,
                                        .same_site = NYA_HTTP_SAME_SITE_STRICT,
                                        .path      = path,
                                    });
}

// PRIVATE API IMPLEMENTATION

b8 _nya_http_cookie_name_char(char character) {
    if (character >= 'a' && character <= 'z') return true;
    if (character >= 'A' && character <= 'Z') return true;
    if (character >= '0' && character <= '9') return true;

    return strchr("!#$%&'*+-.^_`|~", character) != nullptr && character != '\0';
}

b8 _nya_http_cookie_value_char(char character) {
    u8 byte = (u8)character;

    if (byte <= 0x20 || byte >= 0x7F) return false;

    return byte != '"' && byte != ',' && byte != ';' && byte != '\\';
}

b8 _nya_http_cookie_value_is(NYA_ConstCString text, u64 size) {
    for (u64 index = 0; index < size; index++) {
        if (!_nya_http_cookie_value_char(text[index])) return false;
    }

    return true;
}

b8 _nya_http_cookie_token_is(NYA_ConstCString text) {
    u64 size = strlen(text);

    if (size == 0 || size >= NYA_HTTP_MAX_COOKIE_NAME) return false;

    for (u64 index = 0; index < size; index++) {
        if (!_nya_http_cookie_name_char(text[index])) return false;
    }

    return true;
}

b8 _nya_http_cookie_attribute_is(NYA_ConstCString text) {
    u64 size = strlen(text);

    if (size == 0 || size >= NYA_HTTP_MAX_COOKIE_VALUE) return false;

    for (u64 index = 0; index < size; index++) {
        u8 byte = (u8)text[index];

        if (byte <= 0x20 || byte >= 0x7F || byte == ';' || byte == ',') return false;
    }

    return true;
}
