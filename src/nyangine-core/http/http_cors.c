#include <stdio.h>
#include <string.h>

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_logging.h"
#include "nyangine-std/base/base_string.h"
#include "nyangine-core/http/http_cors.h"

// PRIVATE API DECLARATION

/** Whether `method` is one of the policy's allowed verbs. */
NYA_INTERNAL b8 _nya_http_cors_method_allowed(const NYA_HttpCors* cors, NYA_HttpMethod method) __attr_no_discard;

/**
 * Joins `items` into `out` separated by ", ", bounded by `capacity` and always terminated. A field that
 * would not fit whole stops the join rather than being cut, the same way the head renderer bounds a header.
 * */
NYA_INTERNAL void _nya_http_cors_join(char* out, u64 capacity, const NYA_ConstCString* items, u32 count);

/** Adds one header best effort, logging and swallowing the refusal a full header budget gives. */
NYA_INTERNAL void _nya_http_cors_add(NYA_HttpResponse* response, NYA_ConstCString name, NYA_ConstCString value);

// PUBLIC API IMPLEMENTATION

NYA_Error nya_http_cors_check(const NYA_HttpCors* cors) {
    // No policy is default-deny, which is well formed: a route simply answers no CORS header.
    if (cors == nullptr) return NYA_OK;

    if (cors->origin_count > NYA_HTTP_CORS_MAX_ORIGINS) {
        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "a CORS policy lists more than %d origins", NYA_HTTP_CORS_MAX_ORIGINS);
    }

    if (cors->origin_count > 0 && cors->origins == nullptr)
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a CORS policy counts origins it does not have");
    if (cors->method_count > 0 && cors->methods == nullptr)
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a CORS policy counts methods it does not have");
    if (cors->header_count > 0 && cors->headers == nullptr)
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a CORS policy counts headers it does not have");
    if (cors->expose_count > 0 && cors->expose == nullptr)
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a CORS policy counts exposed headers it does not have");

    // The one combination that is a vulnerability and not a preference: a wildcard origin answered with credentials hands every site a credentialed
    // reply, so a table that asks for it does not start.
    if (cors->credentials) {
        for (u32 index = 0; index < cors->origin_count && index < NYA_HTTP_CORS_MAX_ORIGINS; index++) {
            if (nya_string_equals(cors->origins[index], "*")) {
                return nya_error(
                    NYA_ERROR_INVALID_ARGUMENT,
                    "a CORS policy allows credentials with a '*' origin, which reflects a credentialed reply to any site"
                );
            }
        }
    }

    return NYA_OK;
}

NYA_ConstCString nya_http_cors_allow_origin(const NYA_HttpCors* cors, const NYA_HttpRequest* request) {
    nya_assert(request != nullptr);

    if (cors == nullptr || cors->origin_count == 0 || cors->origins == nullptr) return nullptr;

    NYA_ConstCString origin = nya_http_request_header(request, "Origin");
    if (origin == nullptr || origin[0] == '\0') return nullptr;

    b8 wildcard = false;

    for (u32 index = 0; index < cors->origin_count && index < NYA_HTTP_CORS_MAX_ORIGINS; index++) {
        NYA_ConstCString allowed = cors->origins[index];
        if (allowed == nullptr) continue;

        if (nya_string_equals(allowed, "*")) {
            wildcard = true;
            continue;
        }

        // An exact match reflects the caller's own Origin, which is what a credentialed reply is allowed to name and what keeps a browser from ever
        // seeing another site's.
        if (nya_string_equals(allowed, origin)) return origin;
    }

    // A wildcard is honoured only without credentials; nya_http_cors_check has already refused the pair, so this is belt to that suspenders.
    if (wildcard && !cors->credentials) return "*";

    return nullptr;
}

void nya_http_cors_apply(const NYA_HttpCors* cors, const NYA_HttpRequest* request, NYA_HttpResponse* response) {
    nya_assert(request != nullptr);
    nya_assert(response != nullptr);

    NYA_ConstCString allow = nya_http_cors_allow_origin(cors, request);
    if (allow == nullptr) return;

    _nya_http_cors_add(response, "Access-Control-Allow-Origin", allow);

    // A specific origin means the answer differs by Origin, so a shared cache must key on it; a wildcard answer is the same for everyone and needs no
    // Vary.
    if (!nya_string_equals(allow, "*")) _nya_http_cors_add(response, "Vary", "Origin");

    if (cors->credentials) _nya_http_cors_add(response, "Access-Control-Allow-Credentials", "true");

    if (cors->expose_count > 0) {
        char list[NYA_HTTP_MAX_HEADER_VALUE] = { 0 };
        _nya_http_cors_join(list, sizeof(list), cors->expose, cors->expose_count);
        if (list[0] != '\0') _nya_http_cors_add(response, "Access-Control-Expose-Headers", list);
    }
}

NYA_HttpStatus nya_http_cors_preflight(const NYA_HttpCors* cors, const NYA_HttpRequest* request, NYA_HttpResponse* response) {
    nya_assert(request != nullptr);
    nya_assert(response != nullptr);

    // A preflight carries nothing back but its headers, so the response is emptied to a bodiless 204 whichever way the negotiation goes.
    nya_http_response_reset(response);

    NYA_ConstCString allow = nya_http_cors_allow_origin(cors, request);

    NYA_ConstCString requested = nya_http_request_header(request, "Access-Control-Request-Method");
    NYA_HttpMethod   method    = requested != nullptr ? nya_http_method_parse(requested, strlen(requested)) : NYA_HTTP_METHOD_NONE;

    // Both have to hold: an allowed origin, and a requested method the route grants. Either missing leaves the headers off, which a browser reads as
    // the refusal it is.
    if (allow != nullptr && _nya_http_cors_method_allowed(cors, method)) {
        _nya_http_cors_add(response, "Access-Control-Allow-Origin", allow);

        if (!nya_string_equals(allow, "*")) _nya_http_cors_add(response, "Vary", "Origin");

        if (cors->credentials) _nya_http_cors_add(response, "Access-Control-Allow-Credentials", "true");

        char methods[NYA_HTTP_MAX_HEADER_VALUE] = { 0 };
        for (u32 index = 0; index < cors->method_count && index < NYA_HTTP_METHOD_COUNT; index++) {
            NYA_ConstCString text = nya_http_method_text(cors->methods[index]);
            u64              used = strlen(methods);

            if (index > 0 && used + 2 < sizeof(methods)) {
                methods[used]     = ',';
                methods[used + 1] = ' ';
                methods[used + 2] = '\0';
            }
            (void)snprintf(methods + strlen(methods), sizeof(methods) - strlen(methods), "%s", text);
        }
        if (methods[0] != '\0') _nya_http_cors_add(response, "Access-Control-Allow-Methods", methods);

        if (cors->header_count > 0) {
            char headers[NYA_HTTP_MAX_HEADER_VALUE] = { 0 };
            _nya_http_cors_join(headers, sizeof(headers), cors->headers, cors->header_count);
            if (headers[0] != '\0') _nya_http_cors_add(response, "Access-Control-Allow-Headers", headers);
        }

        if (cors->max_age_s > 0) {
            char age[16] = { 0 };
            (void)snprintf(age, sizeof(age), "%u", cors->max_age_s);
            _nya_http_cors_add(response, "Access-Control-Max-Age", age);
        }
    }

    return NYA_HTTP_STATUS_NO_CONTENT;
}

// PRIVATE API IMPLEMENTATION

b8 _nya_http_cors_method_allowed(const NYA_HttpCors* cors, NYA_HttpMethod method) {
    if (cors == nullptr || cors->methods == nullptr || !nya_http_method_is_valid(method)) return false;

    for (u32 index = 0; index < cors->method_count && index < NYA_HTTP_METHOD_COUNT; index++) {
        if (cors->methods[index] == method) return true;
    }

    return false;
}

void _nya_http_cors_join(char* out, u64 capacity, const NYA_ConstCString* items, u32 count) {
    nya_assert(out != nullptr && capacity > 0);

    out[0] = '\0';

    u64 used = 0;

    for (u32 index = 0; items != nullptr && index < count; index++) {
        NYA_ConstCString item = items[index];
        if (item == nullptr) continue;

        NYA_ConstCString separator = used > 0 ? ", " : "";
        u64              needed    = strlen(separator) + strlen(item);

        // One past the last byte is the terminator, so an item that would not fit whole stops the join rather than landing half in.
        if (used + needed >= capacity) break;

        (void)snprintf(out + used, capacity - used, "%s%s", separator, item);
        used += needed;
    }
}

void _nya_http_cors_add(NYA_HttpResponse* response, NYA_ConstCString name, NYA_ConstCString value) {
    NYA_Error added = nya_http_response_header(response, name, value);
    if (!added.ok) nya_log_warn("The %s CORS header could not be added; the response is correct but not readable cross-origin.", name);
}
