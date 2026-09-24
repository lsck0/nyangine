/**
 * @file base_url.h
 *
 * URLs and percent encoding, parsed once where the text arrives.
 *
 * Everything downstream takes an NYA_Url, which only exists if the text was a URL this engine is willing
 * to act on.
 *
 * ```
 * nya_url_parse              "https://host:port/path?query#fragment" -> NYA_Url, or the rule it broke and where
 * nya_url_parse_target       "/path?query", what an HTTP request line carries -> NYA_Url, same contract
 * nya_url_format             NYA_Url -> text; the pair, and the only place a URL is rendered
 * nya_url_path_decode        the path, percent-decoded into a C string
 * nya_url_query_find         one query parameter by name, decoded, with '+' as a space
 *
 * nya_percent_encode         bytes -> text with everything but RFC 3986's unreserved set escaped
 * nya_percent_decode         the pair; refuses a malformed escape rather than passing it through
 * ```
 *
 * ```c
 * NYA_Url        url     = { 0 };
 * NYA_UrlFailure failure = { 0 };
 *
 * if (!nya_url_parse(text, strlen(text), &url, &failure).ok) {
 *     nya_log_warn("refused: %s at byte %u", nya_url_rule_text(failure.rule), failure.offset);
 *     return;
 * }
 *
 * char value[64];
 * b8   found = false;
 * NYA_TRY(nya_url_query_find(&url, "page", value, sizeof(value), &found));
 * ```
 *
 * ── what a parsed URL promises ──
 *
 * The scheme is one this engine speaks, the host is a DNS name, a dotted quad or an IPv6 literal, the
 * port is in 1..65535, every escape is well formed and none decodes to NUL, the path has no "." or ".."
 * segment however spelled, no encoded '/', no decoded control byte and does not begin with "//", and no
 * byte anywhere is a control, a space, non-ASCII, or outside RFC 3986's grammar. Nothing is repaired.
 *
 * ── one bounded copy, not slices ──
 *
 * The components live in `text`, back to back, and each is an offset and a length into it. A slice into
 * the caller's buffer would dangle the moment that buffer moves on, which the HTTP server does after
 * every pipelined request, and could be changed after the checks ran. A bounded copy per field was the
 * other option; it needs a maximum per field, and their sum is larger than one bound on the whole.
 *
 * ── strict RFC 3986, not WHATWG ──
 *
 * Browsers follow the WHATWG URL standard, which repairs: it strips whitespace, turns '\' into '/',
 * reads "0x7f.1" as an address and leaves '{', '|' and '^' raw in a query. Every one of those is a place
 * where two parsers disagree about what a URL means, which is the SSRF and cache poisoning bug. The
 * cost is that a hand typed URL with such a byte in it is refused; anything built through
 * nya_percent_encode never has one.
 *
 * ── userinfo is noticed, never kept ──
 *
 * `user:password@` is parsed so `has_userinfo` can be refused by a caller, but the bytes are not copied
 * and nya_url_format does not write them. A password in a URL ends up in logs and history, and
 * "https://bank.example@evil.example" is a URL people misread.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

// CONSTANTS

/**
 * Longest URL accepted, in bytes. 2048 is the limit links on the web already keep to (the sitemaps
 * protocol's cap, and old Internet Explorer's 2083), and it keeps an NYA_Url small enough for the stack.
 * Past it is NYA_URL_RULE_TOO_LONG, which the HTTP server answers with 414.
 * */
#ifndef NYA_URL_MAX_BYTES
#define NYA_URL_MAX_BYTES 2048
#endif

static_assert(NYA_URL_MAX_BYTES <= U16_MAX, "a span is a u16 offset and a u16 length into the text");

/** Longest host name. RFC 1035's 255 octets on the wire are 253 characters written out. */
#define NYA_URL_HOST_MAX_BYTES 253

/** Longest label between two dots of a host name, from RFC 1035. */
#define NYA_URL_HOST_LABEL_MAX_BYTES 63

/** Longest IPv6 literal inside the brackets: eight groups of four, or six and an embedded dotted quad. */
#define NYA_URL_IPV6_MAX_BYTES 45

/** Digits in a port. Five is enough for 65535 and stops the accumulator long before it could wrap. */
#define NYA_URL_PORT_MAX_DIGITS 5

// TYPES

typedef enum NYA_UrlScheme   NYA_UrlScheme;
typedef enum NYA_UrlHostKind NYA_UrlHostKind;
typedef enum NYA_UrlRule     NYA_UrlRule;
typedef struct NYA_UrlSpan   NYA_UrlSpan;
typedef struct NYA_Url       NYA_Url;
typedef struct NYA_UrlFailure NYA_UrlFailure;

/**
 * The schemes this engine speaks. Anything else is NYA_URL_RULE_SCHEME_UNKNOWN, since a scheme is a
 * promise about what happens when the URL is followed and `file:` or `gopher:` makes a different one.
 * `_NONE` is what a request target parses to and nothing else.
 * */
enum NYA_UrlScheme {
    NYA_URL_SCHEME_NONE = 0,
    NYA_URL_SCHEME_HTTP,
    NYA_URL_SCHEME_HTTPS,
    NYA_URL_SCHEME_WS,
    NYA_URL_SCHEME_WSS,

    NYA_URL_SCHEME_COUNT,
};

/** What the host turned out to be. Stored rather than re-derived, so nothing downstream guesses. */
enum NYA_UrlHostKind {
    NYA_URL_HOST_NONE = 0,
    NYA_URL_HOST_NAME,
    NYA_URL_HOST_IPV4,
    /** Stored without its brackets; nya_url_format writes them back. */
    NYA_URL_HOST_IPV6,

    NYA_URL_HOST_COUNT,
};

/** Which rule a refused URL broke. */
enum NYA_UrlRule {
    NYA_URL_RULE_NONE = 0,
    NYA_URL_RULE_EMPTY,
    NYA_URL_RULE_TOO_LONG,
    /** A byte below 0x20 or DEL, raw or decoded from an escape where it is not allowed. NUL is never allowed. */
    NYA_URL_RULE_CONTROL_CHARACTER,
    NYA_URL_RULE_SPACE,
    /** A byte the grammar has no place for here: non-ASCII, one of "<>\"\\^`{|}", or '[', ']', '#' out of place. */
    NYA_URL_RULE_CHARACTER,
    /** A '%' not followed by two hex digits. */
    NYA_URL_RULE_PERCENT_MALFORMED,
    NYA_URL_RULE_SCHEME_MISSING,
    NYA_URL_RULE_SCHEME_UNKNOWN,
    /** Every scheme here needs "//host"; "http:example" is refused rather than guessed at. */
    NYA_URL_RULE_AUTHORITY_MISSING,
    NYA_URL_RULE_HOST_EMPTY,
    NYA_URL_RULE_HOST_TOO_LONG,
    /** An empty or overlong label, a byte outside [A-Za-z0-9_-], a leading or trailing '-', or a number that is not a dotted quad. */
    NYA_URL_RULE_HOST_MALFORMED,
    NYA_URL_RULE_IPV6_MALFORMED,
    NYA_URL_RULE_PORT_MALFORMED,
    NYA_URL_RULE_PORT_OUT_OF_RANGE,
    NYA_URL_RULE_PATH_NOT_ABSOLUTE,
    /** "%2F" in a segment: decoding it would move a segment boundary that every check before it respected. */
    NYA_URL_RULE_PATH_ENCODED_SLASH,
    NYA_URL_RULE_PATH_DOT_SEGMENT,
    /** A path starting "//" reads as a host to anything that resolves it as a reference: the open redirect. */
    NYA_URL_RULE_PATH_DOUBLE_SLASH,

    NYA_URL_RULE_COUNT,
};

/** A component: where it sits in NYA_Url.text and how long it is. Not null terminated. */
struct NYA_UrlSpan {
    u16 offset;
    u16 length;
};

/**
 * A URL that parsed. There is no constructor but the two parse calls, and every field is already
 * inside its bound. Transparent like every struct here; a caller that writes to it has taken the
 * promise at the top of this file into its own hands, and the calls below assert what they can.
 * */
struct NYA_Url {
    NYA_UrlScheme   scheme;
    NYA_UrlHostKind host_kind;

    b8 has_userinfo;
    b8 has_port;
    /** A trailing '?' with nothing after it is a query that is empty, not a missing one. Same for '#'. */
    b8 has_query;
    b8 has_fragment;

    u16 port;

    NYA_UrlSpan host;
    /** Still percent-encoded. Empty only for an absolute URL with nothing after the authority. */
    NYA_UrlSpan path;
    /** Without the '?', still encoded. See nya_url_query_find. */
    NYA_UrlSpan query;
    /** Without the '#', still encoded. Never present on a request target. */
    NYA_UrlSpan fragment;

    /** Bytes of `text` in use: the components back to back, as they arrived. Not the input, and not terminated. */
    u16  length;
    char text[NYA_URL_MAX_BYTES];
};

/** Which rule a refusal broke and the byte of the input it broke it at. */
struct NYA_UrlFailure {
    NYA_UrlRule rule;
    u32         offset;
};

// FUNCTIONS

/**
 * Parses an absolute URL with one of the schemes in NYA_UrlScheme.
 *
 * NYA_ERROR_PARSE carrying the rule and offset in its message, and in `out_failure` when that is not
 * null. `out_url` is zeroed on failure, never half filled. Allocates nothing and reads only
 * `text[0, size)`.
 * */
NYA_API NYA_Error nya_url_parse(const char* text, u64 size, OUT NYA_Url* out_url, OUT NYA_UrlFailure* out_failure) __attr_no_discard;

/**
 * Parses RFC 9112's origin form, the target of every request an HTTP server takes: an absolute path and
 * an optional query. No scheme, no host, and a '#' is refused, since no client sends a fragment.
 * Same contract as nya_url_parse.
 * */
NYA_API NYA_Error nya_url_parse_target(const char* text, u64 size, OUT NYA_Url* out_url, OUT NYA_UrlFailure* out_failure) __attr_no_discard;

/**
 * Renders `url` into `buffer`, null terminated, `out_length` not counting the terminator. Parsing the
 * result gives back an equal URL, without the userinfo. A buffer of NYA_URL_MAX_BYTES + 1 always fits;
 * a smaller one that does not is NYA_ERROR_OUT_OF_MEMORY.
 * */
NYA_API NYA_Error nya_url_format(const NYA_Url* url, OUT char* buffer, u64 capacity, OUT u64* out_length) __attr_no_discard;

/**
 * The path, percent-decoded and null terminated. What a router matches on. The parse already refused
 * everything that could make the decoded form mean something else, so the only failure is a buffer too
 * small, NYA_ERROR_OUT_OF_MEMORY.
 * */
NYA_API NYA_Error nya_url_path_decode(const NYA_Url* url, OUT char* buffer, u64 capacity, OUT u64* out_length) __attr_no_discard;

/**
 * The query parameter called `name`, decoded into `buffer` and null terminated, with '+' read as a
 * space as HTML forms write it. Keys are compared decoded. A key with no '=' has an empty value.
 *
 * Absent is `*out_found == false` and NYA_OK. A name given twice is NYA_ERROR_PARSE rather than either
 * value: a proxy that takes the last and a handler that takes the first is parameter pollution. A value
 * that does not fit `capacity` is NYA_ERROR_OUT_OF_MEMORY. `buffer` holds an empty string on any failure.
 * */
NYA_API NYA_Error nya_url_query_find(const NYA_Url* url, NYA_ConstCString name, OUT char* buffer, u64 capacity, OUT b8* out_found)
    __attr_no_discard;

/** The rule's name, for a log line. */
NYA_API NYA_ConstCString nya_url_rule_text(NYA_UrlRule rule) __attr_no_discard;

// PERCENT ENCODING

/**
 * Escapes every byte but RFC 3986's unreserved set (letters, digits, "-._~") as "%XX" in upper case, so
 * the output is legal in any component. A space is "%20", never '+', which only a query would read.
 *
 * Null terminated; `out_length` does not count the terminator. At most three bytes out per byte in, and
 * NYA_ERROR_OUT_OF_MEMORY rather than a truncated escape when `capacity` is short.
 * */
NYA_API NYA_Error nya_percent_encode(const u8* data, u64 size, OUT char* buffer, u64 capacity, OUT u64* out_length) __attr_no_discard;

/**
 * Decodes every "%XX" and copies every other byte as it is, so a decode never grows: `capacity >= size`
 * always fits. NYA_ERROR_PARSE naming the offset for a '%' without two hex digits after it. Any byte may
 * come out, NUL included, which is why the output is a length and not a C string.
 * */
NYA_API NYA_Error nya_percent_decode(const char* text, u64 size, OUT u8* buffer, u64 capacity, OUT u64* out_length) __attr_no_discard;
