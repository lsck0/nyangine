/**
 * The shared discoverability primitive: the bounded builder and its escapers, the http/https URL gate,
 * and the serve registry the four documents mount through.
 *
 * The escapers are checked by feeding each a value with the specials in it and reading the entities back;
 * the URL gate by handing it the schemes a loc must never carry; the bound by overflowing it and asking
 * for the result, which must be a refusal and not a truncation; and the registry by dispatching a GET
 * through nya_http_doc_router, as test_router.c and test_health.c drive their routes.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

#define NOW_S 1700000000ULL

/** Builds a bare GET request for `path`, as the parser would leave it. From test_health.c. */
static void make_get(OUT NYA_HttpRequest* request, NYA_ConstCString path) {
    *request = (NYA_HttpRequest){ .method = NYA_HTTP_METHOD_GET, .keep_alive = true };

    NYA_UrlFailure failure = { 0 };
    NYA_EXPECT(nya_url_parse_target(path, strlen(path), &request->target, &failure), "while building a request");

    (void)snprintf(request->path, sizeof(request->path), "%.*s", (int)request->target.path.length, request->target.text + request->target.path.offset);
}

/** One dispatch through the discovery router. */
static NYA_HttpStatus dispatch(NYA_Arena* arena, const NYA_HttpRequest* request, NYA_HttpResponse* response) {
    const NYA_HttpRouter* routers[] = { nya_http_doc_router() };

    NYA_HttpExchange exchange = { .request = request, .response = response, .arena = arena, .now_s = NOW_S };

    nya_http_response_reset(response);
    nya_memset(response->body, 0, response->body_capacity);

    return nya_http_router_dispatch(&exchange, routers, nya_carray_length(routers), nullptr, 0);
}

int main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_doc");
    defer      nya_arena_destroy(arena);

    // TEST: xml_text escapes & < >, and nothing else.
    {
        NYA_HttpDoc doc = nya_http_doc_over(arena, NYA_HTTP_DOC_MAX_BYTES);
        nya_http_doc_xml_text(&doc, "a & b < c > d \" e ' f");

        NYA_ConstCString out = nullptr;
        nya_assert(nya_http_doc_finish(&doc, &out).ok, "a well-formed text builds");
        nya_assert(strcmp(out, "a &amp; b &lt; c &gt; d \" e ' f") == 0, "text escapes & < > and leaves the quotes: %s", out);
    }

    // TEST: xml_attr escapes the two quotes as well.
    {
        NYA_HttpDoc doc = nya_http_doc_over(arena, NYA_HTTP_DOC_MAX_BYTES);
        nya_http_doc_xml_attr(&doc, "\"><script>&'");

        NYA_ConstCString out = nullptr;
        nya_assert(nya_http_doc_finish(&doc, &out).ok);
        nya_assert(strcmp(out, "&quot;&gt;&lt;script&gt;&amp;&#39;") == 0, "attr escapes all five: %s", out);
        nya_assert(strstr(out, "<script>") == nullptr, "no raw tag survives the attr escaper");
    }

    // TEST: cdata splits a literal ]]> so it cannot close the section early.
    {
        NYA_HttpDoc doc = nya_http_doc_over(arena, NYA_HTTP_DOC_MAX_BYTES);
        nya_http_doc_xml_cdata(&doc, "before]]>after");

        NYA_ConstCString out = nullptr;
        nya_assert(nya_http_doc_finish(&doc, &out).ok);
        nya_assert(strcmp(out, "<![CDATA[before]]]]><![CDATA[>after]]>") == 0, "the ]]> is split: %s", out);
        nya_assert(strstr(out + 9, "]]>after") == nullptr, "no bare ]]> is left inside the section content");
    }

    // TEST: the URL gate takes http and https, and refuses everything else.
    {
        nya_assert(nya_http_doc_url_is_web("https://example.com/"), "https is web");
        nya_assert(nya_http_doc_url_is_web("http://example.com/a?b=c&d=e"), "http with a query is web");

        nya_assert(!nya_http_doc_url_is_web("javascript:alert(1)"), "javascript: is refused");
        nya_assert(!nya_http_doc_url_is_web("data:text/html,<b>"), "data: is refused");
        nya_assert(!nya_http_doc_url_is_web("ws://example.com/"), "a websocket URL is not web");
        nya_assert(!nya_http_doc_url_is_web("ftp://example.com/"), "ftp is refused");
        nya_assert(!nya_http_doc_url_is_web("/just/a/path"), "a bare path is not an absolute URL");
        nya_assert(!nya_http_doc_url_is_web(""), "the empty string is not a URL");
        nya_assert(!nya_http_doc_url_is_web(nullptr), "null is not a URL");
    }

    // TEST: the bound refuses the whole document rather than truncating it.
    {
        // A doc that fills exactly builds; the next byte past the bound refuses whole.
        NYA_HttpDoc fits = nya_http_doc_over(arena, 8);
        nya_http_doc_put(&fits, "12345678");
        NYA_ConstCString out = nullptr;
        nya_assert(nya_http_doc_finish(&fits, &out).ok && strcmp(out, "12345678") == 0, "exactly the bound builds");

        NYA_HttpDoc over = nya_http_doc_over(arena, 8);
        nya_http_doc_put(&over, "12345678");
        nya_http_doc_put(&over, "9");
        out = (NYA_ConstCString) "unset";
        nya_assert(!nya_http_doc_finish(&over, &out).ok, "one byte past the bound is refused");
        nya_assert(out == nullptr, "and nothing half-built is handed back");
    }

    // TEST: the serve registry mounts a document and a GET answers with its bytes and media.
    {
        nya_http_doc_clear();

        NYA_EXPECT(nya_http_doc_serve("/thing.xml", NYA_HTTP_MEDIA_XML, "<a>hi</a>", "a thing"));
        NYA_EXPECT(nya_http_doc_serve("/other.txt", NYA_HTTP_MEDIA_TEXT, "plain", "another"));

        nya_assert(nya_http_router_check(nya_http_doc_router()).ok, "the discovery table is one the server will serve");

        NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
        u8               body[NYA_HTTP_MAX_RESPONSE_BYTES] = { 0 };
        NYA_HttpResponse response = { 0 };
        nya_http_response_create(&response, body, sizeof(body));
        defer nya_http_response_destroy(&response);

        make_get(request, "/thing.xml");
        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK, "the served document answers 200");
        nya_assert(response.media_type == NYA_HTTP_MEDIA_XML, "with the media type it was served as");
        nya_assert(response.body_size == 9 && memcmp(response.body, "<a>hi</a>", 9) == 0, "and its exact bytes");

        make_get(request, "/other.txt");
        nya_assert(dispatch(arena, request, &response) == NYA_HTTP_STATUS_OK && response.media_type == NYA_HTTP_MEDIA_TEXT, "the second is served too");

        // A duplicate path is refused, and an empty body or a relative path is a caller's mistake.
        nya_assert(nya_http_doc_serve("/thing.xml", NYA_HTTP_MEDIA_XML, "<b/>", "dup").kind == NYA_ERROR_ALREADY_EXISTS, "a path is served once");
        nya_assert(!nya_http_doc_serve("relative", NYA_HTTP_MEDIA_XML, "<b/>", "s").ok, "a served path is absolute");
        nya_assert(!nya_http_doc_serve("/empty", NYA_HTTP_MEDIA_XML, "", "s").ok, "a served document has a body");

        nya_http_doc_clear();
        nya_assert(nya_http_doc_router()->route_count == 0, "clear empties the registry");
    }

    printf("PASSED: http doc\n");

    return EXIT_SUCCESS;
}
