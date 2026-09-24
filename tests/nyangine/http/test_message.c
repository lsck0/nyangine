/**
 * The request parser and the response writer: the two halves of the wire boundary.
 *
 * Everything here is a pure function over bytes, so every case is a literal and a call. The hostile
 * cases outnumber the well formed ones on purpose; the fuzz target (tests/fuzz/fuzz_http_request.c)
 * covers the shapes nobody thought to write down.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#ifdef NYA_HTTP_COMPRESSION
#include <zlib.h>

/**
 * Inflates a gzip or a zlib stream back to its original bytes, for the round-trip check; returns the
 * number produced, or zero if the stream did not decode. windowBits 15 + 32 lets zlib detect which of
 * the two wrappers it was handed, so this one helper covers both the gzip and the `deflate` coding.
 * */
static u64 zinflate(const u8* data, u64 size, u8* out, u64 out_capacity) {
    z_stream stream = { 0 };
    if (inflateInit2(&stream, 15 + 32) != Z_OK) return 0;

    stream.next_in   = (u8*)data;
    stream.avail_in  = (uInt)size;
    stream.next_out  = out;
    stream.avail_out = (uInt)out_capacity;

    int result   = inflate(&stream, Z_FINISH);
    u64 produced = (u64)stream.total_out;
    (void)inflateEnd(&stream);

    return result == Z_STREAM_END ? produced : 0;
}

/** The value of a response header by name, case-insensitively, or null: what the wire would carry. */
static NYA_ConstCString response_header(const NYA_HttpResponse* response, NYA_ConstCString name) {
    for (u32 index = 0; index < response->header_count; index++) {
        if (_nya_http_equals_ignore_case(response->headers[index].name, strlen(response->headers[index].name), name)) return response->headers[index].value;
    }

    return nullptr;
}
#endif

/** One parse, with the request on the heap: NYA_HttpRequest is twenty kilobytes. */
static NYA_HttpParse parse(NYA_Arena* arena, NYA_ConstCString text, NYA_HttpRequest** out_request, u64* out_consumed, NYA_HttpStatus* out_status) {
    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
    nya_assert(request != nullptr);

    *out_request = request;

    return nya_http_request_parse((const u8*)text, strlen(text), request, out_consumed, out_status);
}

/** The status a piece of text is refused with, or _NONE when it parses or is incomplete. */
static NYA_HttpStatus refusal(NYA_Arena* arena, NYA_ConstCString text) {
    NYA_HttpRequest* request  = nullptr;
    u64              consumed = 0;
    NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

    if (parse(arena, text, &request, &consumed, &status) != NYA_HTTP_PARSE_REFUSED) return NYA_HTTP_STATUS_NONE;

    return status;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_message");
    defer      nya_arena_destroy(arena);

    // TEST: a plain GET, and what the parser promises about it.
    {
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        NYA_ConstCString text = "GET /api/metrics?view=frame HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\n\r\n";

        nya_assert(parse(arena, text, &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);

        nya_assert(request->method == NYA_HTTP_METHOD_GET);
        nya_assert(nya_string_equals(request->path, "/api/metrics"));
        nya_assert(request->target.has_query && request->target.query.length == strlen("view=frame"));
        nya_assert(nya_memcmp(request->target.text + request->target.query.offset, "view=frame", request->target.query.length) == 0);
        nya_assert(request->header_count == 2);
        nya_assert(request->body_size == 0);
        nya_assert(request->keep_alive, "HTTP/1.1 keeps the connection unless it says otherwise");
        nya_assert(consumed == strlen(text));

        // names are folded once, here, so a handler asking for any casing gets the same answer.
        nya_assert(nya_string_equals(nya_http_request_header(request, "host"), "localhost"));
        nya_assert(nya_string_equals(nya_http_request_header(request, "HOST"), "localhost"));
        nya_assert(nya_http_request_header(request, "x-absent") == nullptr);
    }

    // TEST: a body with Content-Length, and a query parameter out of the query.
    {
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        NYA_ConstCString text =
            "POST /api/thing?name=a%20b&flag=1 HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 16\r\n\r\n{\"enabled\":true}";

        nya_assert(parse(arena, text, &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);

        nya_assert(request->method == NYA_HTTP_METHOD_POST);
        nya_assert(request->media_type == NYA_HTTP_MEDIA_JSON);
        nya_assert(request->body_size == 16);
        nya_assert(request->body[16] == '\0', "the body is terminated one past its end");
        nya_assert(nya_string_equals((NYA_ConstCString)request->body, "{\"enabled\":true}"));

        char value[32] = { 0 };

        nya_assert(nya_http_request_query_param(request, "name", value, sizeof(value)));
        nya_assert(nya_string_equals(value, "a b"), "a query parameter is percent-decoded");

        nya_assert(nya_http_request_query_param(request, "flag", value, sizeof(value)));
        nya_assert(nya_string_equals(value, "1"));

        nya_assert(!nya_http_request_query_param(request, "absent", value, sizeof(value)));
        nya_assert(value[0] == '\0', "a missing parameter leaves the buffer empty rather than stale");

        // and the body as the DTO it claims to be, which is the whole conversion in.
        NYA_HttpAccountingDto accounting = { 0 };

        nya_assert(nya_http_request_reflect(request, arena, nya_reflect_of(NYA_HttpAccountingDto), &accounting).ok);
        nya_assert(accounting.enabled);
    }

    // TEST: a QUERY, which is the read verb: safe like a GET and carrying a body.
    {
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        NYA_ConstCString text =
            "QUERY /api/metrics HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 16\r\n\r\n{\"enabled\":true}";

        nya_assert(parse(arena, text, &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);

        nya_assert(request->method == NYA_HTTP_METHOD_QUERY);
        nya_assert(nya_http_method_is_valid(request->method));
        nya_assert(nya_http_method_is_safe(request->method), "a QUERY changes nothing, whatever it carries");
        nya_assert(nya_http_method_allows_body(request->method), "carrying a body is the whole of what it adds");
        nya_assert(nya_string_equals(nya_http_method_text(NYA_HTTP_METHOD_QUERY), "QUERY"));
        nya_assert(nya_http_method_parse("QUERY", 5) == NYA_HTTP_METHOD_QUERY);
        nya_assert(nya_http_method_parse("query", 5) == NYA_HTTP_METHOD_NONE, "a method is case sensitive");

        // the body is a document like any other, and reaches a DTO the same way a POST's does.
        nya_assert(request->media_type == NYA_HTTP_MEDIA_JSON);
        nya_assert(request->body_size == 16);

        NYA_HttpAccountingDto asked = { 0 };
        nya_assert(nya_http_request_reflect(request, arena, nya_reflect_of(NYA_HttpAccountingDto), &asked).ok);
        nya_assert(asked.enabled);

        // and chunked framing is undone for it exactly as it is for a POST.
        nya_assert(
            parse(
                arena,
                "QUERY /api/metrics HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n"
                "8\r\n{\"enable\r\n8\r\nd\":true}\r\n0\r\n\r\n",
                &request,
                &consumed,
                &status
            ) == NYA_HTTP_PARSE_DONE
        );
        nya_assert(request->body_size == 16);

        // a QUERY with no body at all is a read with no parameters, not a malformed request.
        nya_assert(parse(arena, "QUERY /api/metrics HTTP/1.1\r\nHost: x\r\n\r\n", &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_assert(request->body_size == 0);
    }

    // TEST: an HTML form's POST body, read a field at a time.
    {
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        NYA_ConstCString text =
            "POST /login HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 51\r\n\r\n"
            "username=ada&password=a+long+one&note=two%26two%3D4";

        nya_assert(parse(arena, text, &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_assert(request->media_type == NYA_HTTP_MEDIA_FORM, "a form body is named rather than refused");

        char value[64] = { 0 };

        nya_assert(nya_http_request_form_value(request, "username", value, sizeof(value)));
        nya_assert(nya_string_equals(value, "ada"), "a field is read out, got '%s'", value);

        nya_assert(nya_http_request_form_value(request, "password", value, sizeof(value)));
        nya_assert(nya_string_equals(value, "a long one"), "'+' is a space, got '%s'", value);

        // the '&' and '=' inside a value were percent-encoded, so they are not delimiters.
        nya_assert(nya_http_request_form_value(request, "note", value, sizeof(value)));
        nya_assert(nya_string_equals(value, "two&two=4"), "an escaped delimiter is data, got '%s'", value);

        nya_assert(!nya_http_request_form_value(request, "absent", value, sizeof(value)), "a missing field is not there");
        nya_assert(value[0] == '\0', "and leaves the buffer empty rather than stale");

        // a value that does not fit is refused rather than truncated into a different value.
        char tiny[4] = { 0 };
        nya_assert(!nya_http_request_form_value(request, "password", tiny, sizeof(tiny)), "a value too big for the buffer is refused");

        // a JSON body is not a form, whatever a caller asks of it.
        NYA_ConstCString json =
            "POST /login HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: 15\r\n\r\n{\"username\":\"a\"}";

        nya_assert(parse(arena, json, &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_assert(!nya_http_request_form_value(request, "username", value, sizeof(value)), "a JSON body is not read as a form");

        // a malformed escape in a form value is refused, since the value is attacker-controlled.
        NYA_ConstCString broken =
            "POST /login HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 6\r\n\r\nx=%2z%";

        // (Content-Length is deliberately the real body length below; the body is "x=%2z" then a bare '%'.)
        NYA_ConstCString broken_real =
            "POST /login HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 5\r\n\r\nx=%2z";

        (void)broken;
        nya_assert(parse(arena, broken_real, &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_assert(!nya_http_request_form_value(request, "x", value, sizeof(value)), "a malformed escape is refused rather than passed through");
    }

    // TEST: the media types a web bundle is served and read as.
    {
        nya_assert(nya_http_media_type_parse("application/wasm", 16) == NYA_HTTP_MEDIA_WASM, "wasm has a name");
        nya_assert(nya_string_equals(nya_http_media_type_text(NYA_HTTP_MEDIA_WASM), "application/wasm"), "and it is what is written back");

        nya_assert(
            nya_http_media_type_parse("application/x-www-form-urlencoded", 33) == NYA_HTTP_MEDIA_FORM,
            "and a form body's type is recognised with its parameters stripped"
        );
        nya_assert(
            nya_http_media_type_parse("application/x-www-form-urlencoded; charset=utf-8", 48) == NYA_HTTP_MEDIA_FORM,
            "charset and all"
        );
    }

    // TEST: how many Host headers there are decides whether this is one request.
    {
        nya_assert(refusal(arena, "GET /a HTTP/1.1\r\nAccept: */*\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST, "HTTP/1.1 without a Host is not a request");

        nya_assert(
            refusal(arena, "GET /a HTTP/1.1\r\nHost: one.test\r\nHost: two.test\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST,
            "two Hosts are two front ends disagreeing about who the request was for"
        );

        nya_assert(refusal(arena, "GET /a HTTP/1.1\r\nHost: \r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST, "and an empty one names nobody");

        // HTTP/1.0 never had one, and a client still sending 1.0 is not made to grow a header.
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        nya_assert(parse(arena, "GET /a HTTP/1.0\r\n\r\n", &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_assert(!request->keep_alive, "and it closes afterwards, as 1.0 does");
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: a body on a verb that gives one no meaning is refused, not dropped. ─────────────────────────────────────────────────────────────────────────────
    {
        nya_assert(
            refusal(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}") == NYA_HTTP_STATUS_BAD_REQUEST,
            "an intermediary that reads those bytes as a body and a parser that reads them as the next request is smuggling"
        );

        nya_assert(refusal(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "HEAD /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "OPTIONS /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}") == NYA_HTTP_STATUS_BAD_REQUEST);

        // an announced empty body is not a body, and plenty of clients send one.
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        nya_assert(parse(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n", &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_assert(request->body_size == 0);

        // the verbs a body means something on, which is the set the router is written in plus PATCH.
        nya_assert(nya_http_method_allows_body(NYA_HTTP_METHOD_POST));
        nya_assert(nya_http_method_allows_body(NYA_HTTP_METHOD_PUT));
        nya_assert(nya_http_method_allows_body(NYA_HTTP_METHOD_DELETE));
        nya_assert(!nya_http_method_allows_body(NYA_HTTP_METHOD_GET));

        // safe is about changing nothing, not about carrying nothing.
        nya_assert(nya_http_method_is_safe(NYA_HTTP_METHOD_GET) && nya_http_method_is_safe(NYA_HTTP_METHOD_HEAD));
        nya_assert(!nya_http_method_is_safe(NYA_HTTP_METHOD_POST) && !nya_http_method_is_safe(NYA_HTTP_METHOD_PUT));
        nya_assert(!nya_http_method_is_safe(NYA_HTTP_METHOD_DELETE));
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: a chunked body is dechunked before a handler ever sees it. ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        NYA_ConstCString text = "POST /x HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n"
                                "8\r\n{\"enable\r\n8\r\nd\":true}\r\n0\r\n\r\n";

        nya_assert(parse(arena, text, &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);

        nya_assert(request->body_size == 16);
        nya_assert(nya_string_equals((NYA_ConstCString)request->body, "{\"enabled\":true}"));
        nya_assert(consumed == strlen(text), "a chunked body consumes its framing too");
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: two requests in one read, which is what pipelining looks like. ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        NYA_ConstCString first = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
        NYA_ConstCString text  = "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n";

        nya_assert(parse(arena, text, &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);

        nya_assert(nya_string_equals(request->path, "/a"));
        nya_assert(consumed == strlen(first), "the first request consumes exactly itself");
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: a request that has not finished arriving is not an error. ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        nya_assert(parse(arena, "GET /a HTTP/1.1\r\nHost:", &request, &consumed, &status) == NYA_HTTP_PARSE_INCOMPLETE);
        nya_assert(consumed == 0);
        nya_assert(status == NYA_HTTP_STATUS_NONE);

        // a head that is complete but a body that is not.
        nya_assert(parse(arena, "POST /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\nshort", &request, &consumed, &status) == NYA_HTTP_PARSE_INCOMPLETE);

        // and a chunked body that stops mid chunk.
        nya_assert(
            parse(arena, "POST /a HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n8\r\nfour", &request, &consumed, &status) == NYA_HTTP_PARSE_INCOMPLETE
        );
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: a path cannot climb out of the root, however it is spelled. ─────────────────────────────────────────────────────────────────────────────
    {
        nya_assert(refusal(arena, "GET /../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "GET /a/../b HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "GET /a/./b HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);

        // the encoded forms, which is why the check is after decoding rather than before.
        nya_assert(refusal(arena, "GET /%2e%2e/etc HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "GET /a%2f%2e%2e%2fb HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);

        // a NUL in the middle of a path would truncate it for anything treating it as a C string.
        nya_assert(refusal(arena, "GET /a%00b HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);

        // a half written escape decodes to nothing rather than to a zero byte.
        nya_assert(refusal(arena, "GET /a%2 HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "GET /a%zz HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);

        // and a legal escape still decodes.
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        nya_assert(parse(arena, "GET /api/%6det%72ics HTTP/1.1\r\nHost: x\r\n\r\n", &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_assert(nya_string_equals(request->path, "/api/metrics"));
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: the target goes through base_url, so its rules are the server's rules. ─────────────────────────────────────────────────────────────────────────────
    {
        // an encoded '/' would move a segment boundary after the router's checks ran.
        nya_assert(refusal(arena, "GET /api/a%2Fb HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);

        // a path starting "//" is a host to anything that echoes it into a Location header.
        nya_assert(refusal(arena, "GET //evil.example/ HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);

        // no client sends a fragment, and a raw byte outside RFC 3986 is not a target.
        nya_assert(refusal(arena, "GET /a#b HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "GET /a?x={} HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "GET * HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);

        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;
        char             value[32] = { 0 };

        nya_assert(parse(arena, "GET /s?q=a+b%2Bc&page=2&page=3 HTTP/1.1\r\nHost: x\r\n\r\n", &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);

        nya_assert(nya_http_request_query_param(request, "q", value, sizeof(value)));
        nya_assert(nya_string_equals(value, "a b+c"), "'+' is a space in a query and %%2B is a plus");

        // which of two values a proxy and a handler each pick is the pollution bug; neither is answered.
        nya_assert(!nya_http_request_query_param(request, "page", value, sizeof(value)));
        nya_assert(value[0] == '\0');
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: the framing a smuggler wants is refused rather than preferred. ─────────────────────────────────────────────────────────────────────────────
    {
        nya_assert(
            refusal(arena, "POST /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST,
            "a request carrying both framings has no safe interpretation"
        );

        nya_assert(
            refusal(arena, "POST /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello") == NYA_HTTP_STATUS_BAD_REQUEST,
            "two Content-Lengths is the same disagreement in one header"
        );

        nya_assert(
            refusal(arena, "POST /a HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip\r\n\r\n") == NYA_HTTP_STATUS_NOT_IMPLEMENTED,
            "a coding this server cannot undo is not a body it may read raw"
        );

        // an obsolete folded header could fold anything into anything.
        nya_assert(refusal(arena, "GET /a HTTP/1.1\r\nHost: x\r\n Content-Length: 5\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: every bound refuses with the status that names it. ─────────────────────────────────────────────────────────────────────────────
    {
        nya_assert(
            refusal(arena, "POST /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 99999\r\n\r\n") == NYA_HTTP_STATUS_PAYLOAD_TOO_LARGE,
            "a body over the bound is refused before a byte of it is read"
        );

        nya_assert(
            refusal(arena, "POST /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 99999999999999999999999\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST,
            "a length that does not fit a u64 is not a length"
        );

        nya_assert(refusal(arena, "POST /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: -1\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "POST /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0x10\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);

        // a head that never ends.
        NYA_String* long_head = nya_string_create(arena);
        nya_string_extend(long_head, "GET /a HTTP/1.1\r\nHost: localhost\r\n");
        while (long_head->length < NYA_HTTP_MAX_HEAD_BYTES + 64) nya_string_extend(long_head, "X-Padding: 0123456789abcdef\r\n");

        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        nya_assert(
            nya_http_request_parse(
                (const u8*)long_head->items,
                long_head->length,
                nya_arena_alloc(arena, sizeof(NYA_HttpRequest)),
                &consumed,
                &status
            ) == NYA_HTTP_PARSE_REFUSED
        );
        nya_assert(status == NYA_HTTP_STATUS_HEADERS_TOO_LARGE);

        // a path longer than the buffer that holds it.
        NYA_String* long_path = nya_string_create(arena);
        nya_string_extend(long_path, "GET /");
        while (long_path->length < NYA_HTTP_MAX_PATH + 16) nya_string_push_back(long_path, 'a');
        nya_string_extend(long_path, " HTTP/1.1\r\nHost: x\r\n\r\n");

        nya_assert(
            nya_http_request_parse(
                (const u8*)long_path->items,
                long_path->length,
                nya_arena_alloc(arena, sizeof(NYA_HttpRequest)),
                &consumed,
                &status
            ) == NYA_HTTP_PARSE_REFUSED
        );
        nya_assert(status == NYA_HTTP_STATUS_URI_TOO_LONG);

        nya_unused(request);
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: a request line that is not one. ─────────────────────────────────────────────────────────────────────────────
    {
        nya_assert(refusal(arena, "BREW /coffee HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_NOT_IMPLEMENTED);
        nya_assert(refusal(arena, "get /a HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_NOT_IMPLEMENTED, "a method is case sensitive");

        nya_assert(refusal(arena, "GET /a HTTP/9.9\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_HTTP_VERSION);
        nya_assert(refusal(arena, "GET /a\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "GET  /a HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);

        // the absolute and authority forms are a proxy's business and this is not a proxy.
        nya_assert(refusal(arena, "GET http://elsewhere/a HTTP/1.1\r\nHost: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "CONNECT elsewhere:443 HTTP/1.1\r\nHost: localhost\r\n\r\n") == NYA_HTTP_STATUS_NOT_IMPLEMENTED);

        nya_assert(refusal(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\nNo colon here\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\n: empty name\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
        nya_assert(refusal(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\nBad Name: x\r\n\r\n") == NYA_HTTP_STATUS_BAD_REQUEST);
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: an HTTP/1.0 request, and what Connection does either way. ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        nya_assert(parse(arena, "GET /a HTTP/1.0\r\nHost: x\r\n\r\n", &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_assert(!request->keep_alive, "HTTP/1.0 closes unless it asks not to");

        nya_assert(parse(arena, "GET /a HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_assert(request->keep_alive);

        nya_assert(parse(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_assert(!request->keep_alive);
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: a body with no Content-Type is not JSON, whatever it looks like. ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        nya_assert(parse(arena, "POST /a HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}", &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);

        NYA_Object* document = nullptr;
        nya_assert(!nya_http_request_json(request, arena, &document).ok, "a body this server was not told the type of is not parsed as one");

        // and a type it does not speak is the same refusal.
        nya_assert(
            parse(arena, "POST /a HTTP/1.1\r\nHost: localhost\r\nContent-Type: text/xml\r\nContent-Length: 2\r\n\r\n{}", &request, &consumed, &status) ==
            NYA_HTTP_PARSE_DONE
        );
        nya_assert(request->media_type == NYA_HTTP_MEDIA_OTHER);
        nya_assert(!nya_http_request_json(request, arena, &document).ok);

        // parameters after the essence do not change which type it is.
        nya_assert(
            parse(
                arena,
                "POST /a HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json ; charset=utf-8\r\nContent-Length: 2\r\n\r\n{}",
                &request,
                &consumed,
                &status
            ) == NYA_HTTP_PARSE_DONE
        );
        nya_assert(request->media_type == NYA_HTTP_MEDIA_JSON);
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: writing a response. ─────────────────────────────────────────────────────────────────────────────
    {
        u8 body[64] = { 0 };

        NYA_HttpResponse response = { 0 };
        nya_http_response_create(&response, body, sizeof(body));
        defer nya_http_response_destroy(&response);

        nya_assert(nya_http_response_text(&response, "hello", NYA_HTTP_MEDIA_TEXT).ok);
        nya_assert(response.body_size == 5);

        u8  head[NYA_HTTP_MAX_RESPONSE_HEAD_BYTES] = { 0 };
        u64 head_size                              = 0;

        // RFC 9110's own example moment, so the Date line is a fixed string rather than whatever now is.
        NYA_Instant date = { .ns = 784'111'777LL * NYA_NS_PER_SECOND };

        nya_assert(nya_http_response_head(&response, NYA_HTTP_STATUS_OK, true, date, head, sizeof(head), &head_size).ok);

        NYA_String* rendered = nya_string_from(arena, (NYA_ConstCString)head);

        nya_assert(nya_string_starts_with(rendered, "HTTP/1.1 200 OK\r\n"));
        nya_assert(nya_string_contains(rendered, "Content-Length: 5\r\n"));
        nya_assert(nya_string_contains(rendered, "Content-Type: text/plain; charset=utf-8\r\n"));
        nya_assert(nya_string_contains(rendered, "Connection: keep-alive\r\n"));
        nya_assert(nya_string_contains(rendered, "Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n"), "every answer carries its Date");
        nya_assert(nya_string_ends_with(rendered, "\r\n\r\n"), "the head ends with the blank line");

        // the security headers are on every answer, without a route or a layer having to ask.
        nya_assert(nya_string_contains(rendered, "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'"), "the strict CSP is the default");
        nya_assert(nya_string_contains(rendered, "X-Content-Type-Options: nosniff\r\n"));
        nya_assert(nya_string_contains(rendered, "Referrer-Policy: no-referrer\r\n"));
        nya_assert(nya_string_contains(rendered, "Cross-Origin-Opener-Policy: same-origin\r\n"));
        nya_assert(nya_string_contains(rendered, "Cross-Origin-Embedder-Policy: require-corp\r\n"));
        nya_assert(nya_string_contains(rendered, "Cross-Origin-Resource-Policy: same-origin\r\n"));

        // a body larger than the buffer is the handler's mistake and is refused rather than truncated.
        u8 oversized[128] = { 0 };
        nya_assert(!nya_http_response_bytes(&response, oversized, sizeof(oversized), NYA_HTTP_MEDIA_TEXT).ok);

        // a header that would end the head early is response splitting, and is refused rather than stripped.
        nya_assert(!nya_http_response_header(&response, "X-Bad", "a\r\nInjected: yes").ok);
        nya_assert(!nya_http_response_header(&response, "Bad Name", "x").ok);
        nya_assert(nya_http_response_header(&response, "X-Fine", "yes").ok);

        nya_assert(nya_http_response_head(&response, NYA_HTTP_STATUS_OK, false, date, head, sizeof(head), &head_size).ok);

        rendered = nya_string_from(arena, (NYA_ConstCString)head);
        nya_assert(nya_string_contains(rendered, "X-Fine: yes\r\n"));
        nya_assert(nya_string_count(rendered, "Content-Security-Policy") == 1, "one policy, the default, while nothing replaced it");
        nya_assert(nya_string_contains(rendered, "Connection: close\r\n"));
        nya_assert(!nya_string_contains(rendered, "Injected"));

        // a response that sets its own header of the same name, in any case, replaces the default rather than adding a second.
        nya_assert(nya_http_response_header(&response, "content-security-policy", "default-src 'self'").ok);
        nya_assert(nya_http_response_head(&response, NYA_HTTP_STATUS_OK, false, date, head, sizeof(head), &head_size).ok);
        rendered = nya_string_from(arena, (NYA_ConstCString)head);
        nya_assert(nya_string_count(rendered, "Content-Security-Policy") == 0 && nya_string_contains(rendered, "content-security-policy: default-src 'self'"),
                   "the response's own policy replaced the default");

        // reset empties it without unbinding, which is what a layer replacing an answer does.
        nya_http_response_reset(&response);
        nya_assert(response.body_size == 0 && response.header_count == 0);

        // a document answered as JSON whatever was negotiated, the way a fixed JSON endpoint answers.
        NYA_Object* document = nya_object_create(arena);
        nya_object_add(document, "ok", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });
        nya_assert(nya_http_response_json(&response, arena, document).ok);
        nya_assert(response.media_type == NYA_HTTP_MEDIA_JSON, "the media type is JSON");
        nya_assert(response.body_size > 0 && nya_memcmp(body, "{", 1) == 0, "and the body is a JSON object");
        nya_http_response_reset(&response);
        nya_assert(nya_http_response_text(&response, "again", NYA_HTTP_MEDIA_TEXT).ok);

        // and a destroyed response refuses to write rather than scribbling on a buffer that has moved on.
        nya_http_response_destroy(&response);
        nya_assert(!nya_http_response_text(&response, "no", NYA_HTTP_MEDIA_TEXT).ok);
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: application/nya as a body type, in both directions ─────────────────────────────────────────────────────────────────────────────
    {
        /* The native format is what two nyangine programs talk in: it parses substantially faster than JSON and carries the same NYA_Object. JSON stays the answer for everyone else, which is the half worth testing hardest — an integration that has never heard of this engine must not be handed a body it cannot read. */
        NYA_HttpRequest* request  = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        NYA_ConstCString native = "POST /a HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/nya\r\nContent-Length: 30\r\n\r\n"
                                  "nya 2 0\n{ name: string \"ny\"; }\n";

        nya_assert(parse(arena, native, &request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_check(request->media_type == NYA_HTTP_MEDIA_NYA, "application/nya is recognised, got %d", (int)request->media_type);

        NYA_Object* document = nullptr;
        nya_check(nya_http_request_document(request, arena, &document).ok, "and parses as a document");

        if (document != nullptr) {
            NYA_Value* name = nya_object_get(document, "name");
            nya_check(name != nullptr && name->type == NYA_TYPE_STRING, "with its fields in it");
        }

        // The JSON-only entry point refuses it by name rather than parsing it by accident.
        NYA_Object* refused = nullptr;
        nya_check(!nya_http_request_json(request, arena, &refused).ok, "nya_http_request_json refuses a native body");

        // ── what the caller is answered in ──
        NYA_HttpRequest* asking = nullptr;

        nya_assert(parse(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\nAccept: application/nya\r\n\r\n", &asking, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_check(nya_http_request_accepts(asking) == NYA_HTTP_MEDIA_NYA, "a caller that names it is answered in it");

        nya_assert(parse(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\n\r\n", &asking, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_check(nya_http_request_accepts(asking) == NYA_HTTP_MEDIA_JSON, "one that names JSON gets JSON");

        /* The two that decide whether this is safe to turn on. A browser sends the wildcard and has never heard of this format, and a caller with no Accept at all has said nothing — neither is a statement that the native format can be read. */
        nya_assert(parse(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\nAccept: */*\r\n\r\n", &asking, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_check(nya_http_request_accepts(asking) == NYA_HTTP_MEDIA_JSON, "a wildcard is not a request for the native format");

        nya_assert(parse(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\n\r\n", &asking, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_check(nya_http_request_accepts(asking) == NYA_HTTP_MEDIA_JSON, "and neither is no Accept header at all");

        printf("  PASSED\n");
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: application/nya-binary, a DTO out and the same DTO back in. ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_HttpRequest* asking   = nullptr;
        u64              consumed = 0;
        NYA_HttpStatus   status   = NYA_HTTP_STATUS_NONE;

        // named first because its name contains the text form's: this is the case that ordering is for.
        nya_assert(parse(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\nAccept: application/nya-binary\r\n\r\n", &asking, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_check(nya_http_request_accepts(asking) == NYA_HTTP_MEDIA_NYA_BINARY, "a caller that names the binary form is answered in it");

        nya_assert(parse(arena, "GET /a HTTP/1.1\r\nHost: localhost\r\nAccept: application/nya\r\n\r\n", &asking, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_check(nya_http_request_accepts(asking) == NYA_HTTP_MEDIA_NYA, "and one that names the text form never gets bytes");

        nya_check(nya_string_equals(nya_http_media_type_text(NYA_HTTP_MEDIA_NYA_BINARY), "application/nya-binary"), "bytes carry no charset");

        u8               body[256] = { 0 };
        NYA_HttpResponse response  = { 0 };
        nya_http_response_create(&response, body, sizeof(body));
        defer nya_http_response_destroy(&response);

        NYA_HttpAccountingDto sent = { .enabled = true };
        nya_assert(nya_http_response_reflect_as(&response, arena, nya_reflect_of(NYA_HttpAccountingDto), &sent, NYA_HTTP_MEDIA_NYA_BINARY).ok);
        nya_check(response.media_type == NYA_HTTP_MEDIA_NYA_BINARY, "the answer is labelled as what it is");

        // the answer, sent back as a request body: what a client built from the same headers would do.
        NYA_String* wire = nya_string_sprintf(
            arena,
            "PUT /a HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/nya-binary\r\nContent-Length: " FMTu64 "\r\n\r\n",
            response.body_size
        );
        for (u64 i = 0; i < response.body_size; i++) nya_string_push_back(wire, response.body[i]);

        NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
        nya_assert(nya_http_request_parse(wire->items, wire->length, request, &consumed, &status) == NYA_HTTP_PARSE_DONE);
        nya_check(request->media_type == NYA_HTTP_MEDIA_NYA_BINARY, "application/nya-binary is recognised, got %d", (int)request->media_type);

        NYA_HttpAccountingDto received = { 0 };
        nya_check(nya_http_request_reflect(request, arena, nya_reflect_of(NYA_HttpAccountingDto), &received).ok, "the same DTO reads it back");
        nya_check(received.enabled, "with its field in it");

        // another DTO's layout is refused, not read into the wrong fields, and so is reading it untyped.
        NYA_HttpProblem problem = { 0 };
        nya_check(!nya_http_request_reflect(request, arena, nya_reflect_of(NYA_HttpProblem), &problem).ok, "a different layout is refused");

        NYA_Object* untyped = nullptr;
        nya_check(!nya_http_request_document(request, arena, &untyped).ok, "a typed body is not an untyped document");
        nya_check(!nya_http_request_json(request, arena, &untyped).ok, "and not JSON");

        printf("  PASSED\n");
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: an address loses its host part, and anything else becomes "unknown". ─────────────────────────────────────────────────────────────────────────────
    {
        static const NYA_ConstCString CASES[][2] = {
            { "203.0.113.7",             "203.0.113.0/24" },
            { "0.0.0.0",                 "0.0.0.0/24" },
            { "255.255.255.255",         "255.255.255.0/24" },
            { "2001:db8:1:2::5",         "2001:db8:1::/48" },
            { "2001:DB8:0:0:0:0:0:1",    "2001:db8:0::/48" },
            { "::1",                     "0:0:0::/48" },
            { "fe80::1%eth0",            "fe80:0:0::/48" },
            { "::ffff:198.51.100.23",    "198.51.100.0/24" },
            { "",                        "unknown" },
            { "unknown",                 "unknown" },
            { "256.1.1.1",               "unknown" },
            { "1.2.3",                   "unknown" },
            { "1.2.3.4.5",               "unknown" },
            { "01.2.3.4",                "unknown" },
            { "1::2::3",                 "unknown" },
            { "1:2:3:4:5:6:7:8:9",       "unknown" },
            { "1:2:3:4:5:6:7",           "unknown" },
            { "12345::1",                "unknown" },
            { ":1:2:3:4:5:6:7",          "unknown" },
            { "1:2:3:4:5:6:7:",          "unknown" },
            { "1.2.3.4\r\nInjected: yes", "unknown" },
        };

        for (u32 index = 0; index < nya_carray_length(CASES); index++) {
            char out[NYA_HTTP_MAX_ADDRESS] = { 0 };
            nya_http_address_truncate(CASES[index][0], out, sizeof(out));
            nya_check(strcmp(out, CASES[index][1]) == 0, "'%s' became '%s', not '%s'", CASES[index][0], out, CASES[index][1]);
        }
    }

    // ───────────────────────────────────────────────────────────────────────────── TEST: a response with a request id says so in its head, and reset keeps it. ─────────────────────────────────────────────────────────────────────────────
    {
        u8 body[16] = { 0 };

        NYA_HttpResponse response = { 0 };
        nya_http_response_create(&response, body, sizeof(body));
        defer nya_http_response_destroy(&response);

        (void)snprintf(response.request_id, sizeof(response.request_id), "0123456789abcdef");
        nya_http_response_reset(&response);

        u8  head[NYA_HTTP_MAX_RESPONSE_HEAD_BYTES] = { 0 };
        u64 head_size                              = 0;
        nya_assert(nya_http_response_head(&response, NYA_HTTP_STATUS_OK, true, (NYA_Instant){ 0 }, head, sizeof(head), &head_size).ok);
        nya_check(strstr((const char*)head, "X-Request-Id: 0123456789abcdef\r\n") != nullptr, "got:\n%s", (const char*)head);
    }

#ifdef NYA_HTTP_COMPRESSION
    // ───────────────────────────────────────────────────────────────────────────── TEST: response compression — negotiated, correct, and reversible. ─────────────────────────────────────────────────────────────────────────────
    {
        // A body that is text and above the threshold and has the repetition deflate lives on, so it both qualifies and actually shrinks. Kept in one place; every case below answers with it.
        u8 payload[512] = { 0 };
        for (u64 index = 0; index < sizeof(payload); index++) payload[index] = (u8)("nyangine compresses well "[index % 25]);

        u8 body[4096] = { 0 };

        NYA_HttpResponse response = { 0 };
        nya_http_response_create(&response, body, sizeof(body));
        defer nya_http_response_destroy(&response);

        // ── gzip: the client asks, the body is worth it, and it round-trips ──
        nya_assert(nya_http_response_bytes(&response, payload, sizeof(payload), NYA_HTTP_MEDIA_TEXT).ok);
        nya_assert(nya_http_response_compress(&response, arena, "gzip"), "a compressible body with gzip accepted is compressed");

        nya_assert(response.body_size < sizeof(payload), "the compressed body is smaller than the original");
        nya_assert(response.body[0] == 0x1f && response.body[1] == 0x8b, "a gzip stream opens with its magic bytes");
        nya_assert(nya_string_equals(response_header(&response, "Content-Encoding"), "gzip"));
        nya_assert(nya_string_equals(response_header(&response, "Vary"), "Accept-Encoding"), "a shared cache is told the body varies on the ask");
        nya_assert(response.media_type == NYA_HTTP_MEDIA_TEXT, "a coding is not a type; Content-Type is untouched");

        // the whole point: decompressing gives back exactly what went in.
        u8  restored[sizeof(payload)] = { 0 };
        u64 restored_size             = zinflate(response.body, response.body_size, restored, sizeof(restored));
        nya_assert(restored_size == sizeof(payload), "the gzip body inflates to the original length");
        nya_assert(nya_memcmp(restored, payload, sizeof(payload)) == 0, "and to the original bytes");

        // and the head announces the compressed length, not the original.
        u8          head[NYA_HTTP_MAX_RESPONSE_HEAD_BYTES] = { 0 };
        u64         head_size                              = 0;
        NYA_Instant date                                   = { 0 };
        nya_assert(nya_http_response_head(&response, NYA_HTTP_STATUS_OK, true, date, head, sizeof(head), &head_size).ok);

        NYA_String* rendered = nya_string_from(arena, (NYA_ConstCString)head);

        char length_line[64] = { 0 };
        (void)snprintf(length_line, sizeof(length_line), "Content-Length: %llu\r\n", (unsigned long long)response.body_size);
        nya_assert(nya_string_contains(rendered, length_line), "Content-Length is the compressed size");
        nya_assert(nya_string_contains(rendered, "Content-Encoding: gzip\r\n"));
        nya_assert(nya_string_contains(rendered, "Vary: Accept-Encoding\r\n"));

        // ── no Accept-Encoding: nothing is done, and the body is the plain bytes ──
        nya_http_response_reset(&response);
        nya_assert(nya_http_response_bytes(&response, payload, sizeof(payload), NYA_HTTP_MEDIA_TEXT).ok);
        nya_assert(!nya_http_response_compress(&response, arena, nullptr), "no Accept-Encoding leaves the body alone");
        nya_assert(response.body_size == sizeof(payload));
        nya_assert(response_header(&response, "Content-Encoding") == nullptr, "and adds no encoding header");
        nya_assert(response_header(&response, "Vary") == nullptr);

        // ── the `deflate` coding, also reversible ──
        nya_http_response_reset(&response);
        nya_assert(nya_http_response_bytes(&response, payload, sizeof(payload), NYA_HTTP_MEDIA_TEXT).ok);
        nya_assert(nya_http_response_compress(&response, arena, "deflate"));
        nya_assert(nya_string_equals(response_header(&response, "Content-Encoding"), "deflate"));
        u64 deflate_restored = zinflate(response.body, response.body_size, restored, sizeof(restored));
        nya_assert(deflate_restored == sizeof(payload) && nya_memcmp(restored, payload, sizeof(payload)) == 0, "the deflate body round-trips too");

        // ── brotli wins when the client offers it: `br` in the list beats gzip on this server's order ──
        nya_http_response_reset(&response);
        nya_assert(nya_http_response_bytes(&response, payload, sizeof(payload), NYA_HTTP_MEDIA_TEXT).ok);
        nya_assert(nya_http_response_compress(&response, arena, "gzip, deflate, br"));
        nya_assert(nya_string_equals(response_header(&response, "Content-Encoding"), "br"), "br is preferred when offered at an equal weight");
        nya_assert(response.body_size < sizeof(payload), "and it shrinks the body");

        // ── q-values are honoured: a coding ruled out by ;q=0 is not chosen ──
        nya_http_response_reset(&response);
        nya_assert(nya_http_response_bytes(&response, payload, sizeof(payload), NYA_HTTP_MEDIA_TEXT).ok);
        nya_assert(nya_http_response_compress(&response, arena, "br;q=0, gzip;q=0, deflate"), "the one coding left is used");
        nya_assert(nya_string_equals(response_header(&response, "Content-Encoding"), "deflate"), "gzip and br were ruled out by ;q=0");

        nya_http_response_reset(&response);
        nya_assert(nya_http_response_bytes(&response, payload, sizeof(payload), NYA_HTTP_MEDIA_TEXT).ok);
        nya_assert(!nya_http_response_compress(&response, arena, "gzip;q=0"), "the only offered coding was refused, so nothing is done");
        nya_assert(response_header(&response, "Content-Encoding") == nullptr);

        // ── an already-compressed type is left alone, whatever the client asked ──
        nya_http_response_reset(&response);
        nya_assert(nya_http_response_bytes(&response, payload, sizeof(payload), NYA_HTTP_MEDIA_PNG).ok);
        nya_assert(!nya_http_response_compress(&response, arena, "gzip"), "a png is bytes already packed; gzip would only grow it");
        nya_assert(response.body_size == sizeof(payload) && response_header(&response, "Content-Encoding") == nullptr);

        // ── a body under the threshold is not worth the framing ──
        nya_http_response_reset(&response);
        nya_assert(nya_http_response_bytes(&response, payload, 32, NYA_HTTP_MEDIA_TEXT).ok);
        nya_assert(!nya_http_response_compress(&response, arena, "gzip"), "a tiny body is left uncompressed");
        nya_assert(response.body_size == 32);

        // ── never a second coding: a handler that already set Content-Encoding owns the body ──
        nya_http_response_reset(&response);
        nya_assert(nya_http_response_bytes(&response, payload, sizeof(payload), NYA_HTTP_MEDIA_TEXT).ok);
        nya_assert(nya_http_response_header(&response, "Content-Encoding", "gzip").ok);
        nya_assert(!nya_http_response_compress(&response, arena, "gzip"), "a body that already declares a coding is not re-encoded");
        nya_assert(response.body_size == sizeof(payload), "and its bytes are untouched");
    }
#endif

    printf("PASSED: http message\n");

    return nya_check_failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
