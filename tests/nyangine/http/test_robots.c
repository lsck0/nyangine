/**
 * robots.txt: a body built from user-agent groups, the deny-all strict preset, and the refusals that
 * keep a newline in a field from becoming a directive of its own.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

int main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_robots");
    defer      nya_arena_destroy(arena);

    // TEST: a group builds the lines it should, in order, with the Sitemap at the end.
    {
        const NYA_ConstCString disallow[] = { "/private", "/admin" };
        const NYA_ConstCString allow[]    = { "/public" };

        const NYA_HttpRobotsGroup groups[] = {
            { .user_agent = "*", .allow = allow, .allow_count = 1, .disallow = disallow, .disallow_count = 2, .crawl_delay_s = 10 },
            { .user_agent = "BadBot", .disallow = (const NYA_ConstCString[]){ "/" }, .disallow_count = 1 },
        };

        NYA_ConstCString text = nullptr;
        NYA_EXPECT(nya_http_robots_build(arena, (NYA_HttpRobotsConfig){ .groups = groups, .count = nya_carray_length(groups), .sitemap = "https://example.com/sitemap.xml" }, &text));

        nya_assert(strstr(text, "User-agent: *\n") != nullptr, "the first group's agent");
        nya_assert(strstr(text, "Allow: /public\n") != nullptr, "an Allow line");
        nya_assert(strstr(text, "Disallow: /private\n") != nullptr, "a Disallow line");
        nya_assert(strstr(text, "Crawl-delay: 10\n") != nullptr, "the crawl delay");
        nya_assert(strstr(text, "User-agent: BadBot\n") != nullptr, "the second group");
        nya_assert(strstr(text, "Sitemap: https://example.com/sitemap.xml\n") != nullptr, "the Sitemap line");
    }

    // TEST: the strict preset is deny-all for every crawler.
    {
        NYA_ConstCString strict = nya_http_robots_strict();
        nya_assert(strstr(strict, "User-agent: *") != nullptr, "every crawler");
        nya_assert(strstr(strict, "Disallow: /") != nullptr, "and nothing allowed");
    }

    // TEST: a control byte in any field refuses the build — no line injection.
    {
        const NYA_HttpRobotsGroup ua_inject[] = { { .user_agent = "*\nDisallow: /" } };
        NYA_ConstCString          text          = (NYA_ConstCString) "unset";
        nya_assert(!nya_http_robots_build(arena, (NYA_HttpRobotsConfig){ .groups = ua_inject, .count = 1 }, &text).ok, "a newline in a user-agent is refused");
        nya_assert(text == nullptr, "and nothing is emitted");

        const NYA_HttpRobotsGroup path_inject[] = { { .user_agent = "*", .disallow = (const NYA_ConstCString[]){ "/a\r\nAllow: /" }, .disallow_count = 1 } };
        nya_assert(!nya_http_robots_build(arena, (NYA_HttpRobotsConfig){ .groups = path_inject, .count = 1 }, &text).ok, "a CRLF in a path is refused");
    }

    // TEST: a non-web Sitemap URL refuses the build.
    {
        const NYA_HttpRobotsGroup groups[] = { { .user_agent = "*" } };
        NYA_ConstCString          text       = nullptr;
        nya_assert(!nya_http_robots_build(arena, (NYA_HttpRobotsConfig){ .groups = groups, .count = 1, .sitemap = "javascript:alert(1)" }, &text).ok, "a non-web Sitemap is refused");
    }

    // TEST: both mounts land at /robots.txt.
    {
        const NYA_HttpRouter* routers[] = { nya_http_doc_router() };
        b8                    exists  = false;

        nya_http_doc_clear();
        NYA_EXPECT(nya_http_robots_mount_strict());
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_GET, NYA_HTTP_ROBOTS_PATH, &exists) != nullptr, "the strict mount lands at " NYA_HTTP_ROBOTS_PATH);

        nya_http_doc_clear();
        const NYA_HttpRobotsGroup groups[] = { { .user_agent = "*", .disallow = (const NYA_ConstCString[]){ "/private" }, .disallow_count = 1 } };
        NYA_EXPECT(nya_http_robots_mount((NYA_HttpRobotsConfig){ .groups = groups, .count = 1 }));
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_GET, NYA_HTTP_ROBOTS_PATH, &exists) != nullptr, "the from-rules mount lands there too");

        nya_http_doc_clear();
    }

    printf("PASSED: http robots\n");

    return EXIT_SUCCESS;
}
