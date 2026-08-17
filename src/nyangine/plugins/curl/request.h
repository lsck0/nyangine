/**
 * @file request.h
 *
 * Synchronous JSON REST over libcurl, in terms of NYA_Object.
 *
 * Example:
 * ```c
 * NYA_Arena* arena = nya_arena_create(.name = "request");
 * defer      nya_arena_destroy(arena);
 *
 * NYA_Object* body = nya_object_create(arena);
 * // There is no nya_value_u64 constructor, which this example used to call; a value is a literal.
 * nya_object_set(body, "score", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = 4200 });
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
 * // `response` is filled whether or not `result` is an error: a 404 carries a body worth reading.
 * if (!result.ok) nya_log_error("score upload failed: %s", (NYA_ConstCString)result.message);
 * ```
 *
 * **This blocks the calling thread for the whole round trip.** There is no worker behind it and no
 * polling API. base sits below core_job, so a request module down here cannot hand work to the job
 * system, and a request issued from a layer's update will stall the frame for as long as the server
 * takes. Call it from a tool, from a loading screen, or from a thread you own — not from the middle
 * of a frame you care about.
 *
 * A plugin: nothing here is compiled unless -DNYA_PLUGIN_CURL is set. See plugins.h for why these
 * are not part of base.
 * */
#pragma once

#include "nyangine/base/base_arena.h"
#include "nyangine/base/base_attributes.h"
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
typedef struct NYA_Request      NYA_Request;
typedef struct NYA_RequestHeader NYA_RequestHeader;
typedef struct NYA_Response     NYA_Response;

/** Room for the headers a caller adds. Content-Type, Accept and auth are added by the module itself. */
#define NYA_REQUEST_MAX_HEADERS 32

/** What a request waits before giving up, when it does not say. Zero would mean forever. */
#define NYA_REQUEST_DEFAULT_TIMEOUT_MS 30000

enum NYA_RequestMethod {
    NYA_REQUEST_METHOD_GET,
    NYA_REQUEST_METHOD_POST,
    NYA_REQUEST_METHOD_PUT,
    NYA_REQUEST_METHOD_PATCH,
    NYA_REQUEST_METHOD_DELETE,
    NYA_REQUEST_METHOD_COUNT,
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
     * Serialized as compact JSON and sent as the body. Null sends none.
     *
     * Ignored for GET, which has no body in any REST API worth talking to. Sending one is not an
     * error, it simply does not happen — flagging it would turn a shared request struct filled in
     * by a helper into a special case at every call site.
     * */
    const NYA_Object* body;

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
     *
     * On by default would be the friendlier choice and the wrong one: a redirect that crosses to
     * another host carries the Authorization header with it unless something stops it, which is how
     * tokens end up somewhere they were never meant to go. When this is on, the header is dropped
     * on a cross host hop.
     * */
    b8 follow_redirects;

    /**
     * Accept any TLS certificate. **Never set this outside a test against a local server.**
     *
     * Present because a self signed development endpoint is a real thing, and people who need it
     * will otherwise reach past this module to curl directly and turn verification off for
     * everything. Named to be greppable in review.
     * */
    b8 insecure_skip_tls_verify;
};

struct NYA_Response {
    /** HTTP status. Zero when the transport failed and no response was ever received. */
    u32 status;

    /**
     * The body parsed as JSON, or null when it was empty or not JSON.
     *
     * Null is not an error on its own: plenty of endpoints answer a DELETE with 204 and no body,
     * and an error page is very often HTML. `raw_body` always holds what actually arrived.
     * */
    NYA_Object* body;

    /** Exactly what came back, before any parsing. Never null; empty when the body was. */
    NYA_String* raw_body;

    /** The Content-Type header as sent, or null when the server omitted it. */
    NYA_String* content_type;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Performs `request` and fills `out_response`. Blocks until the server answers or the timeout runs out.
 *
 * Everything allocated for the response — the raw body, the parsed object, the content type — comes
 * from `arena` and lives as long as it does.
 *
 * A non-2xx status is returned as an error, mapped to the closest NYA_ErrorKind, because a caller
 * that ignores the return value should not silently proceed on a 500. `out_response` is filled
 * either way: the status and the body of a failed request are usually the whole point, and an API
 * that discarded them would force every caller into the raw curl path to find out what went wrong.
 *
 * `out_response` is only left untouched when the request itself is malformed — no url, unknown
 * method — since there was never a response to describe.
 * */
NYA_API NYA_Error nya_request_perform(NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) __attr_no_discard;

/** GET `url`. Shorthand for the common case; everything else needs the full struct. */
NYA_API NYA_Error nya_request_get(NYA_Arena* arena, NYA_ConstCString url, OUT NYA_Response* out_response) __attr_no_discard;

/** POST `body` as JSON to `url`. */
NYA_API NYA_Error nya_request_post(NYA_Arena* arena, NYA_ConstCString url, const NYA_Object* body, OUT NYA_Response* out_response) __attr_no_discard;

/** The method as it goes on the wire: "GET", "POST", and so on. */
NYA_API NYA_ConstCString nya_request_method_name(NYA_RequestMethod method) __attr_no_discard;

/**
 * Whether `status` is a 2xx.
 *
 * Exists so a caller that deliberately tolerates a 404 can ask the question the same way this
 * module does, rather than open coding a range check that drifts from it.
 * */
NYA_API b8 nya_request_status_is_success(u32 status) __attr_no_discard;
