/**
 * @file http_cookie.h
 *
 * Cookies, in both directions: reading the one header a browser sends, and writing the ones it should
 * store.
 *
 * ```c
 * NYA_HttpCookieValue session = { 0 };
 * if (nya_http_cookie_read(exchange->request, "__Host-session", &session)) {
 *     // session.text is not null terminated; session.size says how far it goes.
 * }
 *
 * NYA_TRY(nya_http_response_cookie(exchange->response,
 *                                  &(NYA_HttpCookie){
 *                                      .name      = "__Host-session",
 *                                      .value     = token,
 *                                      .max_age_s = 900,
 *                                      .http_only = true,
 *                                      .secure    = true,
 *                                      .same_site = NYA_HTTP_SAME_SITE_STRICT,
 *                                      .path      = "/",
 *                                  }));
 * ```
 *
 * ── what this refuses, and why it refuses instead of repairing ──
 *
 * A cookie header is one string a caller controls, read by a parser on the way in and by a browser on
 * the way out, and every session bug of the last twenty years lives in the gap between two readers that
 * disagree about the same bytes. So this parser has no tolerance to exploit: a name that is not a token,
 * a value with a space, a quote, a comma, a semicolon or a control byte in it, a pair with no `=`, an
 * empty name, a repeated name, or more pairs than NYA_HTTP_MAX_COOKIES, and the whole header is refused
 * and no cookie is read from it. A request that meant well and was mangled loses its session and says
 * so; a request that was crafted gets nothing.
 *
 * Writing is the same bargain. A value that would need quoting or escaping is refused rather than
 * encoded, because a caller putting arbitrary bytes in a cookie wants base64url and should say so.
 *
 * ── the prefixes ──
 *
 * `__Host-` and `__Secure-` are not naming conventions, they are rules a browser enforces, and this
 * refuses a cookie that claims one without keeping it: `__Secure-` needs `Secure`, and `__Host-` needs
 * `Secure`, `Path=/` and no `Domain`, which together mean "this name belongs to exactly this host and
 * no subdomain of it can set it". A session cookie should carry one.
 *
 * ── what is not here ──
 *
 * No cookie jar, no expiry arithmetic on the way in (a browser has already dropped what expired), and no
 * `Expires`: `Max-Age` says the same thing in a form with no date parsing in it. Deleting a cookie is
 * `max_age_s = 0` with the same attributes it was set with, which is what a browser matches on.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/http/http_types.h"

// CONSTANTS

/**
 * Pairs one request's `Cookie` header may hold.
 *
 * A browser sends every cookie in scope for the path, so this is a bound on what a page may have
 * accumulated, not on what this server sets: an access token, a refresh token, a CSRF token and a
 * preference is four, and sixteen leaves room for whatever else shares the origin. Past it the header is
 * refused whole, because picking the first sixteen means the pair that decides the session might be the
 * seventeenth.
 * */
#define NYA_HTTP_MAX_COOKIES 16

/** A cookie name, terminator included. Long enough for a `__Host-` prefix and a descriptive name. */
#define NYA_HTTP_MAX_COOKIE_NAME 64

/**
 * A cookie value, terminator included. A JWT with a subject, a session id and the usual claims fits in
 * 512, which is also NYA_HTTP_MAX_TOKEN_BYTES; a value past this belongs in a session row with its key
 * in the cookie.
 * */
#define NYA_HTTP_MAX_COOKIE_VALUE 512

// TYPES

/** How a browser should decide whether to send a cookie with a cross site request. */
typedef enum {
    /**
     * Never with a cross site request, navigations included. What a session cookie takes: it is the line
     * behind the origin check that dispatch already makes, and the two together are the CSRF defence.
     * */
    NYA_HTTP_SAME_SITE_STRICT = 0,

    /** With a cross site navigation but not with a cross site form post or fetch. */
    NYA_HTTP_SAME_SITE_LAX,

    /** With anything, which a browser only honours together with `Secure`. */
    NYA_HTTP_SAME_SITE_NONE,
} NYA_HttpSameSite;

/** One cookie to set, as `nya_http_response_cookie` renders it. */
typedef struct {
    /** A token: letters, digits and `!#$%&'*+-.^_`|~`. Required. */
    NYA_ConstCString name;

    /** Printable ASCII without space, quote, comma, semicolon or backslash. Empty deletes nothing; see `max_age_s`. */
    NYA_ConstCString value;

    /**
     * Seconds the browser should keep it. Zero deletes the cookie, which is the same attributes and an
     * expiry in the past. Omitted from the header when negative, making it a session cookie the browser
     * drops when it closes.
     * */
    s64 max_age_s;

    /** Unreadable from script, which is what keeps a token out of reach of an injected one. */
    b8 http_only;

    /** Sent over TLS only. Required by both prefixes, and by `SameSite=None`. */
    b8 secure;

    NYA_HttpSameSite same_site;

    /** Defaults to "/" when null. A `__Host-` cookie may name nothing else. */
    NYA_ConstCString path;

    /** Null for a host only cookie, which is what a `__Host-` name demands. */
    NYA_ConstCString domain;
} NYA_HttpCookie;

/** One cookie as it arrived: a view into the request, copying nothing and not null terminated. */
typedef struct {
    const char* text;
    u64         size;
} NYA_HttpCookieValue;

// FUNCTIONS

/**
 * The value of `name` out of the request's `Cookie` header, or false when the header is absent, holds no
 * such name, or is malformed in any of the ways the file note lists.
 *
 * The comparison on the name is exact: RFC 6265 cookie names are case sensitive, and `__Host-Session`
 * is not `__Host-session`.
 * */
NYA_API b8 nya_http_cookie_read(const NYA_HttpRequest* request, NYA_ConstCString name, OUT NYA_HttpCookieValue* out_value) __attr_no_discard;

/** How many well formed pairs the request carries, and zero for a header this parser refuses. */
NYA_API u32 nya_http_cookie_count(const NYA_HttpRequest* request) __attr_no_discard;

/**
 * The pair at `index` in the order the browser sent it, for a caller walking them. False past the count
 * or for a header this parser refuses.
 * */
NYA_API b8 nya_http_cookie_at(const NYA_HttpRequest* request, u32 index, OUT NYA_HttpCookieValue* out_name, OUT NYA_HttpCookieValue* out_value)
    __attr_no_discard;

/**
 * Adds one `Set-Cookie` header.
 *
 * NYA_ERROR_INVALID_ARGUMENT for a name that is not a token, a value with a byte that would need
 * quoting, a path or domain with a control byte or a semicolon, a `__Secure-` name without `Secure`, a
 * `__Host-` name without `Secure` or with a `Domain` or a `Path` other than "/", and `SameSite=None`
 * without `Secure`, which a browser refuses anyway. NYA_ERROR_OUT_OF_MEMORY past
 * NYA_HTTP_MAX_RESPONSE_HEADERS, which every cookie shares with every other custom header.
 * */
NYA_API NYA_Error nya_http_response_cookie(NYA_HttpResponse* response, const NYA_HttpCookie* cookie) __attr_no_discard;

/**
 * Deletes `name` by setting it empty with an expiry in the past. The attributes have to match the ones
 * it was set with, which is why they are asked for rather than assumed: a browser treats a cookie with a
 * different path as a different cookie and would leave the original in place.
 * */
NYA_API NYA_Error nya_http_response_cookie_clear(NYA_HttpResponse* response, NYA_ConstCString name, NYA_ConstCString path, b8 secure) __attr_no_discard;

/**
 * Parses a raw `Cookie` header value, which is what the fuzz target drives and what the reader above is
 * written over. `out_pairs` takes at most NYA_HTTP_MAX_COOKIES names and values, pointing into `header`.
 *
 * False for every refusal in the file note, with `out_count` left at zero: a partly parsed header is
 * exactly the disagreement this is trying to avoid.
 * */
NYA_API b8 nya_http_cookie_parse(const char* header, u64 size, OUT NYA_HttpCookieValue* out_names, OUT NYA_HttpCookieValue* out_values,
                                 OUT u32* out_count) __attr_no_discard;
