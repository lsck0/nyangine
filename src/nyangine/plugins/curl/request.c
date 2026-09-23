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
        .raw_body    = nya_string_create(arena),
        .raw_headers = nya_string_create(arena),
    };

    _NYA_RequestSink body_sink   = { .string = out_response->raw_body };
    _NYA_RequestSink header_sink = { .string = out_response->raw_headers };

    curl_easy_setopt(handle, CURLOPT_URL, request.url);
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method_name);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, _nya_request_write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body_sink);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, _nya_request_header_callback);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &header_sink);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, (long)(request.timeout_ms > 0 ? request.timeout_ms : NYA_REQUEST_DEFAULT_TIMEOUT_MS));
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

    // http and https only. Otherwise a redirect or a configured url could reach file:// or scp:// and read
    // local files.
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

    char content_type[256] = { 0 };
    if (nya_response_header(out_response, "content-type", content_type, sizeof(content_type))) {
        out_response->content_type = nya_string_from(arena, content_type);
    }

    if (code != CURLE_OK) {
        return nya_error(_nya_request_kind_from_curl(code), "%s %s failed: %s", method_name, request.url, curl_easy_strerror(code));
    }

    /*
     * Parsed only when it looks like JSON, and a parse failure is not fatal on its own.
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

b8 nya_response_header(const NYA_Response* response, NYA_ConstCString name, char* out_value, u64 capacity) {
    nya_assert(response != nullptr);
    nya_assert(name != nullptr);
    nya_assert(out_value != nullptr);
    nya_assert(capacity > 0);

    out_value[0] = '\0';

    if (response->raw_headers == nullptr) return false;

    const u8* text         = response->raw_headers->items;
    u64       total        = response->raw_headers->length;
    u64       name_length  = strlen(name);
    if (name_length == 0) return false;

    for (u64 line = 0; line < total;) {
        u64 line_end = line;
        while (line_end < total && text[line_end] != '\n') line_end++;

        u64 colon = line;
        while (colon < line_end && text[colon] != ':') colon++;

        // No colon is not a header; skip the line rather than guess where the name ended.
        if (colon == line_end || colon - line != name_length) {
            line = line_end + 1;
            continue;
        }

        b8 matches = true;
        for (u64 i = 0; i < name_length && matches; i++) {
            matches = tolower((int)text[line + i]) == tolower((int)(u8)name[i]);
        }

        if (!matches) {
            line = line_end + 1;
            continue;
        }

        u64 start = colon + 1;
        while (start < line_end && (text[start] == ' ' || text[start] == '\t')) start++;

        u64 stop = line_end;
        while (stop > start && (text[stop - 1] == ' ' || text[stop - 1] == '\t' || text[stop - 1] == '\r')) stop--;

        // Refused rather than truncated: a caller parses these into numbers, and half of a number is a
        // wrong answer where a missing one is a known unknown.
        if (stop - start + 1 > capacity) return false;

        nya_memcpy(out_value, text + start, stop - start);
        out_value[stop - start] = '\0';

        return true;
    }

    return false;
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
     * A status line starts a new response, so anything collected so far belonged to a previous one: a
     * redirect chain and a 100-continue both arrive this way, and only the last reply's headers describe
     * the body the caller gets.
     */
    if (bytes >= 5 && strncmp(data, "HTTP/", 5) == 0) {
        nya_string_clear(sink->string);
        return bytes;
    }

    // The blank line ending the block, which carries nothing.
    u64 end = bytes;
    while (end > 0 && (data[end - 1] == '\r' || data[end - 1] == '\n')) end--;
    if (end == 0) return bytes;

    // Dropped rather than refused: a reply whose headers run past the bound still has a body worth
    // reading, and aborting the transfer over a verbose server would be worse than not seeing the rest.
    if (sink->string->length + end + 1 > NYA_RESPONSE_MAX_HEADER_BYTES) return bytes;

    nya_string_reserve(sink->string, sink->string->length + end + 1);
    for (u64 i = 0; i < end; i++) nya_string_push_back(sink->string, (u8)data[i]);
    nya_string_push_back(sink->string, (u8)'\n');

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
