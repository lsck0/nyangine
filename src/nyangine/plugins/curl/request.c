#include "nyangine/nyangine.h"

#include <curl/curl.h>

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Where a transfer accumulates, since curl hands bytes over in arbitrary sized chunks. */
typedef struct {
    NYA_String* string;
} _NYA_RequestSink;

NYA_INTERNAL u64 _nya_request_write_callback(char* data, u64 size, u64 count, void* user_data);
NYA_INTERNAL u64 _nya_request_header_callback(char* data, u64 size, u64 count, void* user_data);

/** Maps a libcurl result onto the closest NYA_ErrorKind. */
NYA_INTERNAL NYA_ErrorKind _nya_request_kind_from_curl(CURLcode code);

/** Maps a non-2xx HTTP status onto the closest NYA_ErrorKind. */
NYA_INTERNAL NYA_ErrorKind _nya_request_kind_from_status(u32 status);

/*
 * curl_global_init exactly once per process, before any handle exists.
 *
 * A constructor rather than an init function the caller must remember: this module has no other
 * setup, and requiring nya_request_init would put a trap in front of the one thing it does. The
 * matching cleanup is a destructor for symmetry — it releases the global TLS and DNS state that
 * would otherwise be reported as a leak on exit.
 */
NYA_INTERNAL b8 _nya_request_global_ready = false;

__attr_constructor NYA_INTERNAL void _nya_request_global_init(void) {
    CURLcode code            = curl_global_init(CURL_GLOBAL_DEFAULT);
    _nya_request_global_ready = (code == CURLE_OK);

    if (!_nya_request_global_ready) nya_log_error("curl_global_init() failed: %s", curl_easy_strerror(code));
}

__attr_destructor NYA_INTERNAL void _nya_request_global_shutdown(void) {
    if (_nya_request_global_ready) curl_global_cleanup();
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_ConstCString nya_request_method_name(NYA_RequestMethod method) {
    switch (method) {
        case NYA_REQUEST_METHOD_GET:    return "GET";
        case NYA_REQUEST_METHOD_POST:   return "POST";
        case NYA_REQUEST_METHOD_PUT:    return "PUT";
        case NYA_REQUEST_METHOD_PATCH:  return "PATCH";
        case NYA_REQUEST_METHOD_DELETE: return "DELETE";
        case NYA_REQUEST_METHOD_COUNT:
        default:                        return nullptr;
    }
}

b8 nya_request_status_is_success(u32 status) {
    return status >= 200 && status < 300;
}

NYA_Error nya_request_perform(NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) {
    nya_assert(arena != nullptr);
    nya_assert(out_response != nullptr);

    // Malformed before anything is attempted. out_response is deliberately left alone here: there
    // was never a response to describe, and zeroing it would be indistinguishable from a real
    // transfer that returned nothing.
    if (request.url == nullptr || request.url[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "request url is empty");

    NYA_ConstCString method_name = nya_request_method_name(request.method);
    if (method_name == nullptr) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "unknown request method %d", (int)request.method);

    if (!_nya_request_global_ready) return nya_error(NYA_ERROR_NOT_OK, "libcurl was not initialized, see the earlier error");

    CURL* handle = curl_easy_init();
    if (handle == nullptr) return nya_error(NYA_ERROR_OUT_OF_MEMORY, "curl_easy_init() failed");
    defer curl_easy_cleanup(handle);

    *out_response = (NYA_Response){
        .raw_body = nya_string_create(arena),
    };

    NYA_String* content_type = nya_string_create(arena);

    _NYA_RequestSink body_sink   = { .string = out_response->raw_body };
    _NYA_RequestSink header_sink = { .string = content_type };

    curl_easy_setopt(handle, CURLOPT_URL, request.url);
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method_name);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, _nya_request_write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body_sink);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, _nya_request_header_callback);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &header_sink);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, (long)(request.timeout_ms > 0 ? request.timeout_ms : NYA_REQUEST_DEFAULT_TIMEOUT_MS));
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

    // http and https only. Without this a redirect — or a url from a config file — can reach file://
    // and scp://, which turns "fetch the leaderboard" into an arbitrary local file read.
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

    if (request.follow_redirects) {
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 8L);
        // Drops Authorization when a redirect crosses to another host, which is how a bearer token
        // ends up at a server that was never supposed to see it.
        curl_easy_setopt(handle, CURLOPT_UNRESTRICTED_AUTH, 0L);
    }

    if (request.insecure_skip_tls_verify) {
        // Loud on purpose. This is the one option here that can turn a working request into a
        // silent interception, so it does not get to happen quietly.
        nya_log_warn("TLS verification disabled for '%s'. Never do this outside a local test.", request.url);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    /*
     * The body, serialized compactly.
     *
     * Skipped for GET, which has no body in any REST API worth talking to. Held in `payload` rather
     * than passed inline because CURLOPT_POSTFIELDS does not copy — curl reads the pointer during
     * the transfer, so the string has to outlive the setopt call.
     */
    NYA_CString payload = nullptr;
    if (request.body != nullptr && request.method != NYA_REQUEST_METHOD_GET) {
        NYA_String* serialized = nya_serde_json_serialize(arena, request.body, NYA_SERDE_NONE);
        payload                = nya_string_to_cstring(arena, serialized);

        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, (long)serialized->length);
    } else if (request.method == NYA_REQUEST_METHOD_POST || request.method == NYA_REQUEST_METHOD_PUT ||
               request.method == NYA_REQUEST_METHOD_PATCH) {
        // A write method with nothing to write still needs a length, or curl waits for a body that
        // is never coming and the server times out the request.
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, 0L);
    }

    struct curl_slist* headers = nullptr;
    defer curl_slist_free_all(headers);

    headers = curl_slist_append(headers, "Accept: application/json");
    if (payload != nullptr) headers = curl_slist_append(headers, "Content-Type: application/json");

    // Bearer wins over basic when both are set, rather than sending two Authorization headers and
    // letting the server pick.
    if (request.bearer_token != nullptr && request.bearer_token[0] != '\0') {
        NYA_String* auth = nya_string_sprintf(arena, "Authorization: Bearer %s", request.bearer_token);
        headers          = curl_slist_append(headers, nya_string_to_cstring(arena, auth));
    } else if (request.basic_auth.user != nullptr) {
        curl_easy_setopt(handle, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(handle, CURLOPT_USERNAME, request.basic_auth.user);
        curl_easy_setopt(handle, CURLOPT_PASSWORD, request.basic_auth.password != nullptr ? request.basic_auth.password : "");
    }

    // Last, so a caller can override any of the above by naming the same header.
    for (u32 i = 0; i < NYA_REQUEST_MAX_HEADERS; i++) {
        if (request.headers[i].name == nullptr) break;

        NYA_String* header = nya_string_sprintf(
            arena, "%s: %s", request.headers[i].name, request.headers[i].value != nullptr ? request.headers[i].value : ""
        );
        headers = curl_slist_append(headers, nya_string_to_cstring(arena, header));
    }

    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

    CURLcode code = curl_easy_perform(handle);

    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    out_response->status = (u32)status;

    if (content_type->length > 0) out_response->content_type = content_type;

    if (code != CURLE_OK) {
        return nya_error(_nya_request_kind_from_curl(code), "%s %s failed: %s", method_name, request.url, curl_easy_strerror(code));
    }

    /*
     * Parsed only when it looks like JSON, and a parse failure is not fatal on its own.
     *
     * An error page is very often HTML, and a 204 has no body at all. Neither is a reason to lose
     * the status the caller came for, so `body` simply stays null and `raw_body` keeps what
     * arrived.
     */
    if (out_response->raw_body->length > 0) {
        b8 looks_like_json = out_response->content_type == nullptr ||
                             nya_string_contains(out_response->content_type, "json") ||
                             nya_string_contains(out_response->content_type, "JSON");

        if (looks_like_json) {
            NYA_Object* parsed = nullptr;
            NYA_Error   result = nya_serde_json_deserialize(arena, out_response->raw_body->items, out_response->raw_body->length, NYA_SERDE_NONE, &parsed);

            if (result.ok) {
                out_response->body = parsed;
            } else {
                nya_log_debug("Response from %s was not parseable as JSON: %s", request.url, (NYA_ConstCString)result.message);
            }
        }
    }

    if (!nya_request_status_is_success(out_response->status)) {
        return nya_error(
            _nya_request_kind_from_status(out_response->status), "%s %s returned %u", method_name, request.url, out_response->status
        );
    }

    return NYA_OK;
}

NYA_Error nya_request_get(NYA_Arena* arena, NYA_ConstCString url, OUT NYA_Response* out_response) {
    return nya_request_perform(arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_GET, .url = url }, out_response);
}

NYA_Error nya_request_post(NYA_Arena* arena, NYA_ConstCString url, const NYA_Object* body, OUT NYA_Response* out_response) {
    return nya_request_perform(arena, (NYA_Request){ .method = NYA_REQUEST_METHOD_POST, .url = url, .body = body }, out_response);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u64 _nya_request_write_callback(char* data, u64 size, u64 count, void* user_data) {
    _NYA_RequestSink* sink  = (_NYA_RequestSink*)user_data;
    u64               bytes = size * count;

    // Reserved up front rather than grown per byte: curl hands over 16 KiB at a time by default,
    // and push_back alone would reallocate its way through every chunk.
    nya_string_reserve(sink->string, sink->string->length + bytes);
    for (u64 i = 0; i < bytes; i++) nya_string_push_back(sink->string, (u8)data[i]);

    // Returning anything but `bytes` tells curl the sink failed and aborts the transfer.
    return bytes;
}

u64 _nya_request_header_callback(char* data, u64 size, u64 count, void* user_data) {
    _NYA_RequestSink* sink  = (_NYA_RequestSink*)user_data;
    u64               bytes = size * count;

    /*
     * Only Content-Type is kept, and only its value.
     *
     * curl delivers one header per call including the status line and the blank terminator, so this
     * runs for everything and picks out the one header the response struct exposes. Matched case
     * insensitively because header names are, and servers disagree about the capitalisation.
     */
    /*
     * A status line starts a new response, so anything collected so far belonged to a previous one.
     *
     * With CURLOPT_FOLLOWLOCATION curl delivers the headers of *every* hop, not just the final one,
     * and this appended unconditionally — so a request that redirected came back with the content
     * types of each response concatenated ("text/htmlapplication/json"). Clearing here keeps the
     * last response's value, which is the one the body actually came with.
     *
     * Reasoned from curl's documented callback behaviour rather than reproduced: the test suite has
     * no redirecting server to point at.
     */
    if (bytes >= 5 && strncmp(data, "HTTP/", 5) == 0) {
        nya_string_clear(sink->string);
        return bytes;
    }

    NYA_ConstCString prefix        = "content-type:";
    u64              prefix_length = strlen(prefix);
    if (bytes <= prefix_length) return bytes;

    for (u64 i = 0; i < prefix_length; i++) {
        char lowered = (char)((data[i] >= 'A' && data[i] <= 'Z') ? data[i] + ('a' - 'A') : data[i]);
        if (lowered != prefix[i]) return bytes;
    }

    u64 start = prefix_length;
    while (start < bytes && (data[start] == ' ' || data[start] == '\t')) start++;

    u64 end = bytes;
    while (end > start && (data[end - 1] == '\r' || data[end - 1] == '\n')) end--;

    nya_string_reserve(sink->string, end - start);
    for (u64 i = start; i < end; i++) nya_string_push_back(sink->string, (u8)data[i]);

    return bytes;
}

NYA_ErrorKind _nya_request_kind_from_curl(CURLcode code) {
    switch (code) {
        case CURLE_OPERATION_TIMEDOUT: return NYA_ERROR_TIMEOUT;

        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_CONNECT:     return NYA_ERROR_NOT_FOUND;

        case CURLE_OUT_OF_MEMORY:       return NYA_ERROR_OUT_OF_MEMORY;

        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_NOT_BUILT_IN:        return NYA_ERROR_NOT_SUPPORTED;

        case CURLE_URL_MALFORMAT:       return NYA_ERROR_INVALID_ARGUMENT;

        // A failed certificate check is a permission problem rather than a transport one: the peer
        // answered, it just is not who it claimed to be.
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CONNECT_ERROR:   return NYA_ERROR_PERMISSION_DENIED;

        default:                        return NYA_ERROR_IO;
    }
}

NYA_ErrorKind _nya_request_kind_from_status(u32 status) {
    switch (status) {
        case 400: return NYA_ERROR_INVALID_ARGUMENT;
        case 401:
        case 403: return NYA_ERROR_PERMISSION_DENIED;
        case 404:
        case 410: return NYA_ERROR_NOT_FOUND;
        case 408: return NYA_ERROR_TIMEOUT;
        case 409: return NYA_ERROR_ALREADY_EXISTS;
        case 501: return NYA_ERROR_NOT_SUPPORTED;
        case 504: return NYA_ERROR_TIMEOUT;
        default:  return NYA_ERROR_NOT_OK;
    }
}
