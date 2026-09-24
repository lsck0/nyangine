/**
 * Cookies in and out.
 *
 * The parser's cases are written as a table of headers that must be refused, because refusing is what
 * this parser is for: every one of them is a spelling some other parser accepts, and a session is
 * stolen in the gap between two readers that read the same bytes differently.
 **/

#include <string.h>

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** Builds a request carrying one `Cookie` header, which is what the reader is written over. */
static void with_cookie_header(NYA_HttpRequest* request, NYA_ConstCString value) {
    *request = (NYA_HttpRequest){ .method = NYA_HTTP_METHOD_GET };

    (void)snprintf(request->path, sizeof(request->path), "%s", "/");
    (void)snprintf(request->headers[0].name, sizeof(request->headers[0].name), "%s", "cookie");
    (void)snprintf(request->headers[0].value, sizeof(request->headers[0].value), "%s", value);

    request->header_count = 1;
}

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_cookie");
    defer      nya_arena_destroy(arena);

    NYA_HttpRequest* request = nya_arena_alloc(arena, sizeof(NYA_HttpRequest));
    nya_assert(request != nullptr);

    // TEST: a well formed header reads back exactly, in order.
    {
        with_cookie_header(request, "__Host-session=abc.123; theme=dark; __Secure-csrf=Zm9v");

        nya_check(nya_http_cookie_count(request) == 3, "three pairs, got %u", nya_http_cookie_count(request));

        NYA_HttpCookieValue value = { 0 };
        nya_check(nya_http_cookie_read(request, "__Host-session", &value), "the session cookie is there");
        nya_check(value.size == 7 && memcmp(value.text, "abc.123", 7) == 0, "got '%.*s'", (int)value.size, value.text);

        nya_check(nya_http_cookie_read(request, "theme", &value) && value.size == 4, "the second pair is there");
        nya_check(nya_http_cookie_read(request, "__Secure-csrf", &value) && value.size == 4, "the third pair is there");

        // exact, because RFC 6265 names are case sensitive and a browser treats those as two cookies.
        nya_check(!nya_http_cookie_read(request, "__Host-Session", &value), "a name differing in case is another name");
        nya_check(!nya_http_cookie_read(request, "session", &value), "a suffix of a name is not that name");
        nya_check(!nya_http_cookie_read(request, "themes", &value), "a longer name is not that name");

        NYA_HttpCookieValue name = { 0 };
        nya_check(nya_http_cookie_at(request, 1, &name, &value), "the walk reaches the second pair");
        nya_check(name.size == 5 && memcmp(name.text, "theme", 5) == 0, "in the order the browser sent them");
        nya_check(!nya_http_cookie_at(request, 3, &name, &value), "and stops at the count");
    }

    // TEST: a value may be empty, which is how a browser reports a cleared cookie.
    {
        with_cookie_header(request, "session=");

        NYA_HttpCookieValue value = { .text = "x", .size = 1 };
        nya_check(nya_http_cookie_read(request, "session", &value), "an empty value is a value");
        nya_check(value.size == 0, "and it is empty, got %llu bytes", (unsigned long long)value.size);
    }

    // TEST: every header this parser refuses, and it refuses the whole header.
    {
        static const NYA_ConstCString REFUSED[] = {
            "",                        // nothing at all
            "session",                 // no '='
            "=abc",                    // no name
            "session=a; ",             // a separator with nothing after it
            "session=a;theme=b",       // no space after the separator: two readers, two answers
            "session=a;  theme=b",     // two spaces, same reason
            "session=a; theme=b;",     // a trailing separator
            " session=a",              // leading space
            "session=a b",             // a space inside a value
            "session=\"quoted\"",      // a quoted value, which is legal in the grammar and ambiguous here
            "session=a,b",             // a comma, which some parsers read as another pair
            "session=a\\b",            // a backslash, which some parsers unescape
            "sess ion=a",              // a space inside a name
            "sess;ion=a",              // a separator inside a name
            "session=a; session=b",    // the same name twice
            "session=a\x7f",           // a DEL byte
            "session=\x01",            // a control byte
            "session=a\tb",            // a tab
            "sessi\x80n=a",            // a byte past ASCII in a name
            "session=caf\xc3\xa9",     // and in a value, where UTF-8 has to be encoded first
        };

        for (u32 index = 0; index < nya_carray_length(REFUSED); index++) {
            with_cookie_header(request, REFUSED[index]);

            NYA_HttpCookieValue value = { 0 };
            nya_check(!nya_http_cookie_read(request, "session", &value), "'%s' was read", REFUSED[index]);
            nya_check(nya_http_cookie_count(request) == 0, "'%s' reported pairs", REFUSED[index]);
        }

        // a header with no cookie header at all is not a refusal, it is an absence.
        *request = (NYA_HttpRequest){ .method = NYA_HTTP_METHOD_GET };

        NYA_HttpCookieValue value = { 0 };
        nya_check(!nya_http_cookie_read(request, "session", &value), "no header, no cookie");
        nya_check(nya_http_cookie_count(request) == 0, "and no count");
    }

    // TEST: the bounds, at the edge and one past it.
    {
        char header[NYA_HTTP_MAX_HEADER_VALUE] = { 0 };

        u64 length = 0;
        for (u32 index = 0; index < NYA_HTTP_MAX_COOKIES; index++) {
            length += (u64)snprintf(header + length, sizeof(header) - length, "%sc%u=%u", index > 0 ? "; " : "", index, index);
        }

        NYA_HttpCookieValue names[NYA_HTTP_MAX_COOKIES]  = { 0 };
        NYA_HttpCookieValue values[NYA_HTTP_MAX_COOKIES] = { 0 };
        u32                 count                        = 0;

        nya_check(nya_http_cookie_parse(header, length, names, values, &count), "the bound itself is allowed");
        nya_check(count == NYA_HTTP_MAX_COOKIES, "got %u", count);

        length += (u64)snprintf(header + length, sizeof(header) - length, "; one=more");

        nya_check(!nya_http_cookie_parse(header, length, names, values, &count), "one past the bound is refused");
        nya_check(count == 0, "and reads nothing, since the pair that matters may be the last one");
    }

    // TEST: writing one, and the attributes that go with it.
    {
        u8 body[16] = { 0 };

        NYA_HttpResponse response = { 0 };
        nya_http_response_create(&response, body, sizeof(body));
        defer nya_http_response_destroy(&response);

        nya_check(nya_http_response_cookie(&response,
                                           &(NYA_HttpCookie){
                                               .name      = "__Host-session",
                                               .value     = "token",
                                               .max_age_s = 900,
                                               .http_only = true,
                                               .secure    = true,
                                               .same_site = NYA_HTTP_SAME_SITE_STRICT,
                                           })
                      .ok,
                  "a __Host- cookie that keeps its side of the bargain");

        nya_check(response.header_count == 1, "one header, got %u", response.header_count);
        nya_check(strcmp(response.headers[0].name, "Set-Cookie") == 0, "under the right name");
        nya_check(strcmp(response.headers[0].value, "__Host-session=token; Path=/; SameSite=Strict; Max-Age=900; Secure; HttpOnly") == 0,
                  "got '%s'", response.headers[0].value);

        nya_http_response_reset(&response);

        // clearing is the same cookie with an expiry of zero, which is what a browser matches on.
        nya_check(nya_http_response_cookie_clear(&response, "__Host-session", "/", true).ok, "clearing writes a header");
        nya_check(strstr(response.headers[0].value, "Max-Age=0") != nullptr, "with an expiry of zero: '%s'", response.headers[0].value);
        nya_check(strstr(response.headers[0].value, "__Host-session=;") != nullptr, "and an empty value: '%s'", response.headers[0].value);

        nya_http_response_reset(&response);

        // a session cookie has no Max-Age at all, which is a negative one here.
        nya_check(nya_http_response_cookie(&response, &(NYA_HttpCookie){ .name = "theme", .value = "dark", .max_age_s = -1 }).ok, "a plain cookie");
        nya_check(strstr(response.headers[0].value, "Max-Age") == nullptr, "carries no expiry: '%s'", response.headers[0].value);
        nya_check(strstr(response.headers[0].value, "Secure") == nullptr, "and nothing it did not ask for: '%s'", response.headers[0].value);
    }

    // TEST: a prefix a browser enforces is enforced here, where it can still be seen.
    {
        u8 body[16] = { 0 };

        NYA_HttpResponse response = { 0 };
        nya_http_response_create(&response, body, sizeof(body));
        defer nya_http_response_destroy(&response);

        nya_check(!nya_http_response_cookie(&response, &(NYA_HttpCookie){ .name = "__Host-session", .value = "t" }).ok,
                  "__Host- without Secure is refused");
        nya_check(!nya_http_response_cookie(&response, &(NYA_HttpCookie){ .name = "__Secure-x", .value = "t" }).ok,
                  "__Secure- without Secure is refused");
        nya_check(!nya_http_response_cookie(&response, &(NYA_HttpCookie){ .name = "__Host-x", .value = "t", .secure = true, .path = "/api" }).ok,
                  "__Host- with a path is refused");
        nya_check(
            !nya_http_response_cookie(&response, &(NYA_HttpCookie){ .name = "__Host-x", .value = "t", .secure = true, .domain = "example.com" }).ok,
            "__Host- with a domain is refused");
        nya_check(!nya_http_response_cookie(&response,
                                            &(NYA_HttpCookie){ .name = "x", .value = "t", .same_site = NYA_HTTP_SAME_SITE_NONE })
                       .ok,
                  "SameSite=None without Secure is refused, since a browser refuses it too");

        // and the values a caller must encode first rather than have quoted for them.
        nya_check(!nya_http_response_cookie(&response, &(NYA_HttpCookie){ .name = "x", .value = "a b" }).ok, "a space in a value is refused");
        nya_check(!nya_http_response_cookie(&response, &(NYA_HttpCookie){ .name = "x", .value = "a;b" }).ok, "a separator in a value is refused");
        nya_check(!nya_http_response_cookie(&response, &(NYA_HttpCookie){ .name = "a b", .value = "t" }).ok, "a name that is not a token is refused");
        nya_check(!nya_http_response_cookie(&response, &(NYA_HttpCookie){ .name = "x", .value = "t", .path = "/a;b" }).ok,
                  "a path that could end the header early is refused");

        nya_check(response.header_count == 0, "and none of them wrote a header, got %u", response.header_count);
    }

    printf("PASSED: http cookie\n");

    return nya_check_failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
