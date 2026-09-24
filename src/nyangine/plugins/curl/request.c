#include "nyangine/nyangine.h"

#include <curl/curl.h>

// ───────────────────────────────────── PRIVATE API DECLARATION ─────────────────────────────────────

/** Where a transfer accumulates, since curl hands bytes over in arbitrary sized chunks. */
typedef struct {
    NYA_String* string;
} _NYA_RequestSink;

/**
 * The object as `key=value&key=value`, percent encoded.
 *
 * An error for a value that is an object or an array, because a form body has nowhere to put one: a
 * caller that meant to send nesting meant to send JSON, and flattening it quietly would make a token
 * endpoint answer something unhelpful about a parameter nobody wrote.
 * */
NYA_INTERNAL NYA_Error _nya_request_form_encode(NYA_Arena* arena, const NYA_Object* body, OUT NYA_String** out_encoded) __attr_no_discard;

/** Appends `text` to `out`, percent encoding everything outside RFC 3986's unreserved set. */
NYA_INTERNAL void _nya_request_form_append(NYA_String* out, NYA_ConstCString text);

NYA_INTERNAL u64 _nya_request_write_callback(char* data, u64 size, u64 count, void* user_data);
NYA_INTERNAL u64 _nya_request_header_callback(char* data, u64 size, u64 count, void* user_data);

/** Maps a libcurl result onto the closest NYA_ErrorKind. */
NYA_INTERNAL NYA_ErrorKind _nya_request_kind_from_curl(CURLcode code);

/** Maps a non-2xx HTTP status onto the closest NYA_ErrorKind. */
NYA_INTERNAL NYA_ErrorKind _nya_request_kind_from_status(u32 status);

// curl_global_init exactly once per process, before any handle exists.
NYA_INTERNAL b8 _nya_request_global_ready = false;

__attr_constructor NYA_INTERNAL void _nya_request_global_init(void) {
    CURLcode code            = curl_global_init(CURL_GLOBAL_DEFAULT);
    _nya_request_global_ready = (code == CURLE_OK);

    if (!_nya_request_global_ready) nya_log_error("curl_global_init() failed: %s", curl_easy_strerror(code));
}

__attr_destructor NYA_INTERNAL void _nya_request_global_shutdown(void) {
    if (_nya_request_global_ready) curl_global_cleanup();
}

// ───────────────────────────────────── PUBLIC API IMPLEMENTATION ─────────────────────────────────────

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

NYA_INTERNAL NYA_Error _nya_request_perform_once(NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) {
    nya_assert(arena != nullptr);
    nya_assert(out_response != nullptr);

    // Malformed before anything is attempted; out_response is left alone, since zeroing it would look like a real transfer that returned nothing.
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

    // http and https only, so a redirect or a configured url cannot reach file:// or scp:// and read local files.
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");

    if (request.follow_redirects) {
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 8L);
        // Drops Authorization when a redirect crosses to another host, so a bearer token never reaches a server it was not meant for.
        curl_easy_setopt(handle, CURLOPT_UNRESTRICTED_AUTH, 0L);
    }

    if (request.insecure_skip_tls_verify) {
        // Loud on purpose: this is the one option here that can turn a working request into a silent interception.
        nya_log_warn("TLS verification disabled for '%s'. Never do this outside a local test.", request.url);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    // The body, serialized compactly.
    NYA_CString payload      = nullptr;
    b8          payload_form = request.body_kind == NYA_REQUEST_BODY_FORM;

    if (request.body != nullptr && request.method != NYA_REQUEST_METHOD_GET) {
        NYA_String* serialized = nullptr;

        if (payload_form) {
            NYA_TRY(_nya_request_form_encode(arena, request.body, &serialized));
        } else {
            serialized = nya_serde_json_serialize(arena, request.body, NYA_SERDE_NONE);
        }

        payload = nya_string_to_cstring(arena, serialized);

        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, (long)serialized->length);
    } else if (request.method == NYA_REQUEST_METHOD_POST || request.method == NYA_REQUEST_METHOD_PUT ||
               request.method == NYA_REQUEST_METHOD_PATCH) {
        // A write method with nothing to write still needs a length, or curl waits for a body that never comes and the server times out.
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, 0L);
    }

    struct curl_slist* headers = nullptr;
    defer curl_slist_free_all(headers);

    headers = curl_slist_append(headers, "Accept: application/json");
    if (payload != nullptr) headers = curl_slist_append(headers, payload_form ? "Content-Type: application/x-www-form-urlencoded" : "Content-Type: application/json");

    // Bearer wins over basic when both are set, rather than sending two Authorization headers.
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

    // Parsed only when it looks like JSON, and a parse failure is not fatal on its own.
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

        // Refused rather than truncated: a caller parses these into numbers, and half a number is a wrong answer where a missing one is a known unknown.
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

// ───────────────────────────────────── PRIVATE API IMPLEMENTATION ─────────────────────────────────────

NYA_Error _nya_request_form_encode(NYA_Arena* arena, const NYA_Object* body, NYA_String** out_encoded) {
    *out_encoded = nya_string_create(arena);

    b8 first = true;

    nya_dict_foreach_key (body, key) {
        NYA_Value* value = nya_object_get(body, *key);
        if (value == nullptr) continue;

        if (!first) nya_string_extend(*out_encoded, "&");
        first = false;

        _nya_request_form_append(*out_encoded, *key);
        nya_string_extend(*out_encoded, "=");

        switch (value->type) {
            case NYA_TYPE_STRING: _nya_request_form_append(*out_encoded, value->as_string); break;

            case NYA_TYPE_B8: nya_string_extend(*out_encoded, value->as_b8 ? "true" : "false"); break;

            case NYA_TYPE_S64: nya_string_extend_sprintf(*out_encoded, FMTs64, value->as_s64); break;
            case NYA_TYPE_F64: nya_string_extend_sprintf(*out_encoded, "%g", value->as_f64); break;

            // An object or an array has no form encoding, and neither does a type this does not model.
            default:
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "'%s' cannot go in a form encoded body: it is not a string, a number or a boolean", *key);
        }
    }

    return NYA_OK;
}

void _nya_request_form_append(NYA_String* out, NYA_ConstCString text) {
    static const char HEX[] = "0123456789ABCDEF";

    if (text == nullptr) return;

    for (u64 index = 0; text[index] != '\0'; index++) {
        char character = text[index];

        // RFC 3986's unreserved set stands for itself; everything else is encoded, including the `+ / =` a base64 code_verifier or token carries and a server would misread as structure.
        b8 unreserved = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                        (character >= '0' && character <= '9') || character == '-' || character == '.' || character == '_' || character == '~';

        if (unreserved) {
            nya_string_push_back(out, (u8)character);
            continue;
        }

        u8 byte = (u8)character;

        nya_string_push_back(out, (u8)'%');
        nya_string_push_back(out, (u8)HEX[(byte >> 4) & 0x0FU]);
        nya_string_push_back(out, (u8)HEX[byte & 0x0FU]);
    }
}

u64 _nya_request_write_callback(char* data, u64 size, u64 count, void* user_data) {
    _NYA_RequestSink* sink  = (_NYA_RequestSink*)user_data;
    u64               bytes = size * count;

    // Reserved up front rather than grown per byte: curl hands over 16 KiB at a time, and push_back alone would reallocate through every chunk.
    nya_string_reserve(sink->string, sink->string->length + bytes);
    for (u64 i = 0; i < bytes; i++) nya_string_push_back(sink->string, (u8)data[i]);

    // Returning anything but `bytes` tells curl the sink failed and aborts the transfer.
    return bytes;
}

u64 _nya_request_header_callback(char* data, u64 size, u64 count, void* user_data) {
    _NYA_RequestSink* sink  = (_NYA_RequestSink*)user_data;
    u64               bytes = size * count;

    // A status line starts a new response, so anything collected so far belonged to a previous one (a redirect chain, a 100-continue); only the last reply's headers describe the body.
    if (bytes >= 5 && strncmp(data, "HTTP/", 5) == 0) {
        nya_string_clear(sink->string);
        return bytes;
    }

    // The blank line ending the block, which carries nothing.
    u64 end = bytes;
    while (end > 0 && (data[end - 1] == '\r' || data[end - 1] == '\n')) end--;
    if (end == 0) return bytes;

    // Dropped rather than refused: a reply whose headers overrun the bound still has a body worth reading, so aborting over a verbose server would be worse.
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

        // A failed certificate check is a permission problem, not a transport one: the peer answered, it just is not who it claimed to be.
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

// ───────────────────────────────────── BEING A GOOD CLIENT ─────────────────────────────────────

/** The bucket a request spends from: what it named, or the host it is going to. */
NYA_INTERNAL void _nya_request_rate_key(NYA_Arena* arena, const NYA_Request* request, OUT char* out_key, u64 capacity) {
    out_key[0] = '\0';

    if (request->rate_key != nullptr && request->rate_key[0] != '\0') {
        (void)snprintf(out_key, capacity, "%s", request->rate_key);
        return;
    }

    NYA_Url        url     = { 0 };
    NYA_UrlFailure failure = { 0 };

    // A url this cannot parse is one curl will refuse anyway; the whole string as a key is a bucket of its own, the safe way to be wrong here.
    if (!nya_url_parse(request->url, strlen(request->url), &url, &failure).ok) {
        (void)snprintf(out_key, capacity, "%s", request->url);
        return;
    }

    (void)nya_unused(arena);
    (void)snprintf(out_key, capacity, "%.*s", (s32)url.host.length, request->url + url.host.offset);
}

/** Reads a header as a number of seconds, as `Retry-After` and `X-RateLimit-Reset-After` are written. */
NYA_INTERNAL b8 _nya_request_header_seconds(const NYA_Response* response, NYA_ConstCString name, OUT f64* out_seconds) {
    char value[64] = { 0 };

    if (!nya_response_header(response, name, value, sizeof(value))) return false;
    if (value[0] == '\0') return false;

    char* end     = nullptr;
    f64   seconds = strtod(value, &end);

    // A Retry-After may also be an HTTP date, which this does not read: that needs a trusted clock, and every API that matters sends the seconds form.
    if (end == value || seconds < 0.0 || isnan(seconds)) return false;

    *out_seconds = seconds;

    return true;
}

/** Tells the limiter whatever this reply said about the budget. */
NYA_INTERNAL void _nya_request_rate_learn(NYA_RateLimiter* limiter, NYA_ConstCString key, const NYA_Response* response) {
    if (limiter == nullptr) return;

    f64 seconds = 0.0;

    // A 429 is the server stating the budget outright, so it wins over everything below.
    if (response->status == 429 && _nya_request_header_seconds(response, "retry-after", &seconds)) {
        nya_rate_told(limiter, key, (u64)(seconds * 1000.0));
        return;
    }

    if (response->status == 429) {
        // Refused with no number on it: a second is a guess, but sending again at once is what gets an address blocked.
        nya_rate_told(limiter, key, 1000);
        return;
    }

    // Headers an API publishes on every reply, not just a refused one (Discord, GitHub): reading them keeps the local bucket level with the server's without being refused first.
    char remaining_text[32] = { 0 };

    if (!nya_response_header(response, "x-ratelimit-remaining", remaining_text, sizeof(remaining_text))) return;

    char* end       = nullptr;
    f64   remaining = strtod(remaining_text, &end);

    if (end == remaining_text || remaining < 0.0 || isnan(remaining)) return;

    f64 reset_seconds = 0.0;
    (void)_nya_request_header_seconds(response, "x-ratelimit-reset-after", &reset_seconds);

    nya_rate_observed(limiter, key, remaining, (u64)(reset_seconds * 1000.0));
}

/** Whether this method may be sent again after a failure that is not a 429. See NYA_Request. */
NYA_INTERNAL b8 _nya_request_is_idempotent(NYA_RequestMethod method) {
    switch (method) {
        // Sending one of these twice is sending it once, which is the whole of what idempotent means.
        case NYA_REQUEST_METHOD_GET:
        case NYA_REQUEST_METHOD_PUT:
        case NYA_REQUEST_METHOD_DELETE: return true;

        case NYA_REQUEST_METHOD_POST:
        case NYA_REQUEST_METHOD_PATCH:
        case NYA_REQUEST_METHOD_COUNT:
        default: return false;
    }
}

NYA_Error nya_request_perform(NYA_Arena* arena, NYA_Request request, OUT NYA_Response* out_response) {
    nya_assert(arena != nullptr);
    nya_assert(out_response != nullptr);

    char key[NYA_RATE_MAX_KEY] = { 0 };
    if (request.limiter != nullptr || request.breaker != nullptr) _nya_request_rate_key(arena, &request, key, sizeof(key));

    NYA_Error answer = NYA_OK;

    for (u32 attempt = 0; ; attempt++) {
        // The budget first, every attempt: a retry that ignored the limiter is the one call most likely to be refused going out fastest.
        if (request.limiter != nullptr) (void)nya_rate_wait(request.limiter, key);

        // The breaker second: a known-down dependency fails fast with status zero (the shape a transport failure leaves) and no socket.
        if (request.breaker != nullptr && !nya_circuit_allow(request.breaker, key)) {
            nya_memset(out_response, 0, sizeof(*out_response));
            return nya_error(NYA_ERROR_TIMEOUT, "circuit breaker open for '%s'", key);
        }

        answer = _nya_request_perform_once(arena, request, out_response);

        if (request.limiter != nullptr) _nya_request_rate_learn(request.limiter, key, out_response);

        // Train the breaker: an answered status (4xx included — the dependency is up) is a success, a 5xx or transport failure (status zero) is not.
        if (request.breaker != nullptr) nya_circuit_record(request.breaker, key, out_response->status >= 100 && out_response->status < 500);

        if (answer.ok) return answer;
        if (attempt >= request.retries) return answer;
        if (!nya_retry_is_worthwhile(out_response->status)) return answer;

        // A 429 means the server did not do the thing, so any method is safe to resend; everything else lost the answer, not the request, so resending a POST may charge twice.
        b8 safe = out_response->status == 429 || _nya_request_is_idempotent(request.method) || request.retry_unsafe_methods;

        if (!safe) return answer;

        if (out_response->status == 429) {
            // The limiter already holds what the server said; waiting on it is waiting exactly that long.
            if (request.limiter != nullptr) continue;

            f64 seconds = 0.0;
            u64 wait_ms = _nya_request_header_seconds(out_response, "retry-after", &seconds) ? (u64)(seconds * 1000.0) : 1000;

            nya_os_time_sleep_ms((u32)(wait_ms > NYA_RATE_MAX_WAIT_MS ? NYA_RATE_MAX_WAIT_MS : wait_ms));

            continue;
        }

        nya_os_time_sleep_ms((u32)nya_backoff_ms(attempt));
    }
}
