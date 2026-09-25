/**
 * The URL parser, which every request target and every websocket URL pays before anything routes.
 *
 * nya_url_parse turns bytes into an NYA_Url in a fixed buffer, allocating nothing, and nya_url_parse_target
 * does the same for the origin-form target off a request line. The percent decoder is the other half: a
 * path is decoded once per request before it is matched. This measures the three over the shapes a server
 * actually sees — an absolute URL with a query, a bare request target, and a path that needs decoding —
 * so the number bounds how fast bytes off a socket become something routable.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

static const char ABSOLUTE_URL[]  = "https://example.test:8443/api/notes/2024?limit=20&order=desc#top";
static const char REQUEST_TARGET[] = "/api/notes/2024?limit=20&order=desc";
static const char ENCODED_PATH[]   = "/files/a%20b/%C3%A9dition/report%2Efinal.pdf";

s32 main(void) {
    nya_bench_begin("URL parse (per input)");

    nya_bench("parse an absolute URL", 1, {
        NYA_Url        url     = { 0 };
        NYA_UrlFailure failure = { 0 };
        NYA_Error      outcome = nya_url_parse(ABSOLUTE_URL, sizeof(ABSOLUTE_URL) - 1, &url, &failure);
        nya_bench_keep(outcome.ok);
    });

    nya_bench("parse a request target", 1, {
        NYA_Url        url     = { 0 };
        NYA_UrlFailure failure = { 0 };
        NYA_Error      outcome = nya_url_parse_target(REQUEST_TARGET, sizeof(REQUEST_TARGET) - 1, &url, &failure);
        nya_bench_keep(outcome.ok);
    });

    // the target parse followed by the path decode: the pair a router runs on the way to a handler.
    nya_bench("parse and decode an encoded path", 1, {
        NYA_Url        url     = { 0 };
        NYA_UrlFailure failure = { 0 };
        char           path[NYA_URL_MAX_BYTES + 1] = { 0 };
        u64            path_length                 = 0;

        NYA_Error parsed = nya_url_parse_target(ENCODED_PATH, sizeof(ENCODED_PATH) - 1, &url, &failure);
        if (parsed.ok) (void)nya_url_path_decode(&url, path, sizeof(path), &path_length);
        nya_bench_keep(path_length);
    });

    return nya_bench_end();
}
