/**
 * The HTTP request parser, the first thing every request pays before a route ever runs.
 *
 * nya_http_request_parse turns a raw byte buffer into an NYA_HttpRequest, allocating nothing and
 * reading nothing past the buffer, so the measurement is pure parse throughput over two shapes: a
 * plain GET with the headers a browser sends, and a POST carrying a small JSON body with a
 * Content-Length. Throughput here bounds how fast a worker can turn bytes off a socket into work.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

static const char GET_REQUEST[] =
    "GET /api/notes?limit=20&order=desc HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "User-Agent: Mozilla/5.0 (bench)\r\n"
    "Accept: application/json\r\n"
    "Accept-Encoding: gzip, deflate, br\r\n"
    "Cookie: __Host-session=abcdef0123456789abcdef0123456789\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

static const char POST_REQUEST[] =
    "POST /api/notes HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 41\r\n"
    "Cookie: __Host-session=abcdef0123456789abcdef0123456789\r\n"
    "\r\n"
    "{\"title\":\"a note\",\"body\":\"with some text\"}";

s32 main(void) {
    nya_bench_begin("HTTP request parse (per request)");

    nya_bench("parse a GET with headers", 1, {
        NYA_HttpRequest request = { 0 };
        u64            consumed = 0;
        NYA_HttpStatus status   = 0;
        NYA_HttpParse  outcome  = nya_http_request_parse((const u8*)GET_REQUEST, sizeof(GET_REQUEST) - 1, &request, &consumed, &status);
        nya_bench_keep(outcome);
    });

    nya_bench("parse a POST with a JSON body", 1, {
        NYA_HttpRequest request = { 0 };
        u64            consumed = 0;
        NYA_HttpStatus status   = 0;
        NYA_HttpParse  outcome  = nya_http_request_parse((const u8*)POST_REQUEST, sizeof(POST_REQUEST) - 1, &request, &consumed, &status);
        nya_bench_keep(outcome);
    });

    return nya_bench_end();
}
