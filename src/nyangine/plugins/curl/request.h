/**
 * @file request.h
 *
 * ```c
 * NYA_Arena* arena = nya_arena_create(.name = "request");
 * defer      nya_arena_destroy(arena);
 *
 * NYA_Object* body = nya_object_create(arena);
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
 * // `response` is filled even when `result` is an error: a 404 carries a body worth reading.
 * if (!result.ok) nya_log_error("score upload failed: %s", (NYA_ConstCString)result.message);
 * ```
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
     * */
    b8 follow_redirects;

    /** Accept any TLS certificate. Only for tests against a local server. */
    b8 insecure_skip_tls_verify;
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
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Performs `request` and fills `out_response`. Blocks until the server answers or the timeout runs out.
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
 * */
NYA_API b8 nya_request_status_is_success(u32 status) __attr_no_discard;
