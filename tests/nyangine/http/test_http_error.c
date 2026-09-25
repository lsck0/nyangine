/**
 * The NYA_Error to HTTP answer mapper: which status a kind becomes, whether the body is HTML or JSON by
 * the request's Accept, and that a 5xx never carries what broke inside.
 *
 * Driven through the exchange directly rather than through a socket, the same way test_router.c does:
 * a request and a response are data, and mapping an error to one needs no port.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/** A request with `method` and, when `accept` is not null, that one Accept header. */
static void make_request(OUT NYA_HttpRequest* request, NYA_HttpMethod method, NYA_ConstCString accept) {
    *request = (NYA_HttpRequest){ .method = method, .keep_alive = true };

    if (accept == nullptr) return;

    request->header_count = 1;
    (void)snprintf(request->headers[0].name, sizeof(request->headers[0].name), "accept");
    (void)snprintf(request->headers[0].value, sizeof(request->headers[0].value), "%s", accept);
}

/** Answers `error` into `response`, zeroing the body first so it reads back as a C string. */
static NYA_HttpStatus
run_error(NYA_Arena* arena, const NYA_HttpRequest* request, NYA_HttpResponse* response, NYA_Error error) {
    nya_http_response_reset(response);
    nya_memset(response->body, 0, response->body_capacity);

    NYA_HttpExchange exchange = { .request = request, .response = response, .arena = arena };

    return nya_http_response_error(&exchange, error);
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_error");
    defer      nya_arena_destroy(arena);

    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
    nya_assert(request != nullptr);

    u8 body[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };

    NYA_HttpResponse response = { 0 };
    nya_http_response_create(&response, body, sizeof(body));
    defer nya_http_response_destroy(&response);

    // TEST: each error kind maps to the status its meaning calls for.
    {
        nya_assert(nya_http_status_from_error(NYA_ERROR_NONE) == NYA_HTTP_STATUS_OK);
        nya_assert(nya_http_status_from_error(NYA_ERROR_NOT_FOUND) == NYA_HTTP_STATUS_NOT_FOUND);
        nya_assert(nya_http_status_from_error(NYA_ERROR_PERMISSION_DENIED) == NYA_HTTP_STATUS_FORBIDDEN);
        nya_assert(nya_http_status_from_error(NYA_ERROR_ALREADY_EXISTS) == NYA_HTTP_STATUS_CONFLICT);
        nya_assert(nya_http_status_from_error(NYA_ERROR_INVALID_ARGUMENT) == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(nya_http_status_from_error(NYA_ERROR_PARSE) == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(nya_http_status_from_error(NYA_ERROR_NOT_SUPPORTED) == NYA_HTTP_STATUS_NOT_IMPLEMENTED);
        nya_assert(nya_http_status_from_error(NYA_ERROR_TIMEOUT) == NYA_HTTP_STATUS_SERVICE_UNAVAILABLE);

        // the whole internal group is 500: none of it is anything the caller did.
        nya_assert(nya_http_status_from_error(NYA_ERROR_NOT_OK) == NYA_HTTP_STATUS_INTERNAL_ERROR);
        nya_assert(nya_http_status_from_error(NYA_ERROR_OUT_OF_MEMORY) == NYA_HTTP_STATUS_INTERNAL_ERROR);
        nya_assert(nya_http_status_from_error(NYA_ERROR_IO) == NYA_HTTP_STATUS_INTERNAL_ERROR);
        nya_assert(nya_http_status_from_error(NYA_ERROR_CORRUPT) == NYA_HTTP_STATUS_INTERNAL_ERROR);
    }

    // TEST: an API client, or a wildcard, is answered in JSON, and the body is the NYA_HttpProblem.
    {
        make_request(request, NYA_HTTP_METHOD_QUERY, "application/json");

        NYA_HttpStatus status = run_error(arena, request, &response, nya_error(NYA_ERROR_NOT_FOUND, "no such thing"));

        nya_assert(status == NYA_HTTP_STATUS_NOT_FOUND);
        nya_assert(response.media_type == NYA_HTTP_MEDIA_JSON);

        NYA_Object* problem = nullptr;
        nya_assert(nya_deserialize(arena, response.body, response.body_size, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &problem).ok);
        nya_assert(problem != nullptr);
        nya_assert(nya_object_get(problem, "status") != nullptr);

        // the specific 4xx message is safe and is carried through.
        nya_assert(strstr((const char*)response.body, "no such thing") != nullptr);

        // a client that stated nothing gets JSON too, never a page.
        make_request(request, NYA_HTTP_METHOD_QUERY, nullptr);
        (void)run_error(arena, request, &response, nya_error(NYA_ERROR_INVALID_ARGUMENT));
        nya_assert(response.media_type == NYA_HTTP_MEDIA_JSON);
    }

    // TEST: a browser is answered with an HTML page carrying the status.
    {
        make_request(request, NYA_HTTP_METHOD_GET, "text/html,application/xhtml+xml,*/*;q=0.8");

        NYA_HttpStatus status = run_error(arena, request, &response, nya_error(NYA_ERROR_NOT_FOUND, "no such thing"));

        nya_assert(status == NYA_HTTP_STATUS_NOT_FOUND);
        nya_assert(response.media_type == NYA_HTTP_MEDIA_HTML);
        nya_assert(nya_string_starts_with(nya_string_from(arena, (const char*)response.body), "<!doctype html>"));
        nya_assert(strstr((const char*)response.body, "404") != nullptr);
        nya_assert(strstr((const char*)response.body, "Not Found") != nullptr);
    }

    // TEST: error text on the page is escaped, so it cannot be a tag.
    {
        make_request(request, NYA_HTTP_METHOD_QUERY, "text/html");

        (void)run_error(arena, request, &response, nya_error(NYA_ERROR_INVALID_ARGUMENT, "<script>alert('x')</script>"));

        nya_assert(response.media_type == NYA_HTTP_MEDIA_HTML);
        nya_assert(strstr((const char*)response.body, "<script>") == nullptr, "the raw tag must never reach the page");
        nya_assert(strstr((const char*)response.body, "&lt;script&gt;") != nullptr, "it is there, as text");
    }

    // TEST: a 5xx says no more than its reason phrase, in either format, so an internal detail cannot leak.
    {
        NYA_ConstCString secret = "the sql at /var/db/main.sqlite is corrupt";

        make_request(request, NYA_HTTP_METHOD_QUERY, "application/json");
        NYA_HttpStatus status = run_error(arena, request, &response, nya_error(NYA_ERROR_IO, "%s", secret));

        nya_assert(status == NYA_HTTP_STATUS_INTERNAL_ERROR);
        nya_assert(strstr((const char*)response.body, secret) == nullptr, "a 5xx JSON body must not carry the internal message");
        nya_assert(strstr((const char*)response.body, "Internal Server Error") != nullptr);

        make_request(request, NYA_HTTP_METHOD_QUERY, "text/html");
        (void)run_error(arena, request, &response, nya_error(NYA_ERROR_IO, "%s", secret));
        nya_assert(strstr((const char*)response.body, secret) == nullptr, "nor may a 5xx HTML page");
    }

    nya_log_info("test_http_error: all good.");

    return 0;
}
