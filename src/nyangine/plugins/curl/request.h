/**
 * @file request.h
 *
 * ```c
 * NYA_Arena* arena = nya_arena_create(.name = "request");
 * defer      nya_arena_destroy(arena);
 *
 * NYA_Object* body = nya_object_create(arena);
 * nya_object_add(body, "score", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 4200 });
 *
 * NYA_Response response = { 0 };
 * NYA_Error    result   = nya_request_perform(arena, (NYA_Request){
 *     .method       = NYA_REQUEST_METHOD_POST,
 *     .url          = "https://api.example.com/v1/scores",
 *     .body         = body,
 *     .bearer_token = token,
 *     .timeout_ms   = 5000,
 * }, &response);
 *
 * // `response` is filled even when `result` is an error: a 404 carries a body worth reading.
 * if (!result.ok) nya_log_error("score upload failed: %s", (NYA_ConstCString)result.message);
 * ```
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_rate.h"
#include "nyangine/base/base_circuit.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_string.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_RequestMethod  NYA_RequestMethod;
typedef enum NYA_RequestBody    NYA_RequestBody;
typedef struct NYA_Request      NYA_Request;
typedef struct NYA_RequestHeader NYA_RequestHeader;
typedef struct NYA_Response     NYA_Response;

/** Room for the headers a caller adds. Content-Type, Accept and auth are added by the module itself. */
#define NYA_REQUEST_MAX_HEADERS 32

/** What a request waits before giving up, when it does not say. Zero would mean forever. */
#define NYA_REQUEST_DEFAULT_TIMEOUT_MS 30000

/**
 * Response header bytes kept, the whole block together.
 *
 * A server decides how many headers it sends and how long each one is, so without a bound a reply is a
 * way to make this process allocate. Sixteen kilobytes is far past any real reply; past it the rest is
 * dropped and nothing grows.
 * */
#define NYA_RESPONSE_MAX_HEADER_BYTES 16384

enum NYA_RequestMethod {
    NYA_REQUEST_METHOD_GET,
    NYA_REQUEST_METHOD_POST,
    NYA_REQUEST_METHOD_PUT,
    NYA_REQUEST_METHOD_PATCH,
    NYA_REQUEST_METHOD_DELETE,
    NYA_REQUEST_METHOD_COUNT,
};

/** How `body` is written onto the wire, and what `Content-Type` says about it. */
enum NYA_RequestBody {
    /** Compact JSON, `application/json`. What an ordinary API takes and the default. */
    NYA_REQUEST_BODY_JSON = 0,

    /**
     * `application/x-www-form-urlencoded`: `key=value&key=value`, percent encoded.
     *
     * What OAuth 2 and OpenID Connect token endpoints take, and what a default Keycloak install takes
     * and nothing else — RFC 6749 says the request parameters are form encoded, and several providers
     * read that strictly. The object's values must be strings, numbers or booleans, because a form body
     * has no nesting to put an object or an array into; one of those is an error rather than something
     * quietly flattened.
     * */
    NYA_REQUEST_BODY_FORM,

    NYA_REQUEST_BODY_COUNT,
};

struct NYA_RequestHeader {
    NYA_ConstCString name;
    NYA_ConstCString value;
};

struct NYA_Request {
    NYA_RequestMethod method;

    /** Required. Must carry a scheme; libcurl is configured to accept only http and https. */
    NYA_ConstCString url;

    /**
     * Sent as the body, in whatever `body_kind` says. Null sends none.
     * */
    const NYA_Object* body;

    /** How to write `body`. Zero is JSON, which is what everything but a token endpoint wants. */
    NYA_RequestBody body_kind;

    /** Terminated by the first entry with a null name. Overrides anything this module sets by default. */
    NYA_RequestHeader headers[NYA_REQUEST_MAX_HEADERS];

    /** Sent as `Authorization: Bearer <token>`. Mutually exclusive with basic_auth; bearer wins. */
    NYA_ConstCString bearer_token;

    struct {
        NYA_ConstCString user;
        NYA_ConstCString password;
    } basic_auth;

    /** Whole transfer, not just connect. Zero means NYA_REQUEST_DEFAULT_TIMEOUT_MS. */
    u64 timeout_ms;

    /**
     * Follow 3xx redirects. Off by default.
     * */
    b8 follow_redirects;

    /** Accept any TLS certificate. Only for tests against a local server. */
    b8 insecure_skip_tls_verify;

    /*
     * ─────────────────────────────────────────────────────────
     * BEING A GOOD CLIENT
     * ─────────────────────────────────────────────────────────
     */

    /**
     * How many *extra* attempts a failure is worth. Zero, the default, is one try and no retry.
     *
     * Only what nya_retry_is_worthwhile calls worth repeating: a 408, a 425, a 429, a 5xx, and a
     * transport failure where no answer ever came. A 400 sent again is the same 400, and a 401 sent
     * again is how a token gets locked.
     *
     * The wait between attempts is a 429's `Retry-After` when there is one, and nya_backoff_ms
     * otherwise — exponential with full jitter, because clients that failed together and backed off by
     * the same doubling arrive together again.
     * */
    u32 retries;

    /**
     * Whether a POST or a PATCH may be retried on a *failure*, rather than only on a 429.
     *
     * Off by default, and it is the one setting here with a way to lose money. GET, PUT and DELETE are
     * idempotent, so sending one twice is sending it once. A POST is not: a 5xx or a timeout means the
     * answer was lost, never that the request was — the charge may well have gone through, and
     * retrying it charges twice.
     *
     * A 429 is retried whatever this says, because a 429 is the server stating it did *not* do the
     * thing. Turn this on for a POST that is safe to repeat — an idempotency key, a search, an upsert
     * — and leave it off for everything else.
     * */
    b8 retry_unsafe_methods;

    /**
     * The budget this call spends from, or null for a call that spends from none.
     *
     * Waited for before the request goes and corrected from the reply afterwards — a `Retry-After`,
     * Discord's `X-RateLimit-Remaining` and `-Reset-After`, GitHub's `X-RateLimit-*`. That correction
     * is the point: a local guess that argued with the server's own answer is a program that gets
     * itself banned while believing it is within the rules. See base_rate.h.
     * */
    NYA_RateLimiter* limiter;

    /**
     * Which bucket of the limiter this call belongs to. Empty means the request's host.
     *
     * The host is the right default and the wrong answer for an API that limits per route: name the
     * route, or — better, where the server publishes one, as Discord does in `X-RateLimit-Bucket` —
     * name what the server calls it, so routes that share a budget share a bucket here too.
     * */
    NYA_ConstCString rate_key;

    /**
     * A circuit breaker for the dependency, keyed the same as the limiter (`rate_key`, else the host).
     *
     * Different question from the limiter: the limiter waits when this program is over budget; the
     * breaker fails fast when the dependency is *down*, so a dead service is not hammered by retries and
     * gets quiet air to recover. When it is OPEN the call returns NYA_ERROR_TIMEOUT at once with a
     * status of zero, touching no socket. Every attempt's outcome trains it: a 5xx or a transport
     * failure is a failure, any answered status (a 4xx included) is a success. See base_circuit.h.
     * */
    NYA_CircuitBreaker* breaker;
};

struct NYA_Response {
    /** HTTP status. Zero when the transport failed and no response was ever received. */
    u32 status;

    /**
     * The body parsed as JSON, or null when it was empty or not JSON.
     * */
    NYA_Object* body;

    /** Exactly what came back, before any parsing. Never null; empty when the body was. */
    NYA_String* raw_body;

    /** The Content-Type header as sent, or null when the server omitted it. */
    NYA_String* content_type;

    /**
     * Every response header as it arrived, one `name: value` per line, bounded by
     * NYA_RESPONSE_MAX_HEADER_BYTES. Never null; empty when the transfer never got a reply. Read it with
     * nya_response_header rather than searching it, so the case insensitive match happens in one place.
     *
     * A redirect chain leaves only the last reply's headers here: the earlier ones described a response
     * the caller never sees a body for.
     * */
    NYA_String* raw_headers;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Performs `request` and fills `out_response`. Blocks until the server answers or the timeout runs out.
 *
 * With `retries` set this may take considerably longer than one timeout: it is one attempt, a wait,
 * and another attempt, up to that many extra times. `out_response` describes the last attempt.
 *
 * With a `limiter` it also waits for the budget before each attempt; see NYA_Request.
 * */
NYA_API NYA_Error nya_request_perform(NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) __attr_no_discard;

/** GET `url`. Shorthand for the common case; everything else needs the full struct. */
NYA_API NYA_Error nya_request_get(NYA_Arena* arena, NYA_ConstCString url, OUT NYA_Response* out_response) __attr_no_discard;

/** POST `body` as JSON to `url`. */
NYA_API NYA_Error nya_request_post(NYA_Arena* arena, NYA_ConstCString url, const NYA_Object* body, OUT NYA_Response* out_response) __attr_no_discard;

/**
 * Copies the value of response header `name` into `out_value`, NUL terminated, and answers whether it was
 * there.
 *
 * The name is matched case insensitively, as HTTP requires, and the value is trimmed of the surrounding
 * whitespace and the line ending. A value longer than `capacity` is refused rather than truncated: a
 * half read header is worse than a missing one, since a caller would act on a number that lost its
 * digits. Only the first occurrence is returned.
 * */
NYA_API b8 nya_response_header(const NYA_Response* response, NYA_ConstCString name, OUT char* out_value, u64 capacity) __attr_no_discard;

/** The method as it goes on the wire: "GET", "POST", and so on. */
NYA_API NYA_ConstCString nya_request_method_name(NYA_RequestMethod method) __attr_no_discard;

/**
 * Whether `status` is a 2xx.
 * */
NYA_API b8 nya_request_status_is_success(u32 status) __attr_no_discard;
