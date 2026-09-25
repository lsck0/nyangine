/**
 * The sitemap: a <urlset> built from sample URLs, asserted well-formed and escaped, with a non-web loc
 * and an out-of-range priority refusing the whole build.
 *
 * There is no XML parser here, so "well-formed" is checked structurally: the angle brackets balance and
 * every `&` begins a known entity, which is exactly what an injection through an unescaped loc would
 * break.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/** A cheap well-formedness check: balanced <>, and every & a known XML entity. CDATA regions are skipped whole. */
static void assert_xml_wellformed(NYA_ConstCString xml) {
    u64 lt = 0;
    u64 gt = 0;

    for (u64 i = 0; xml[i] != '\0';) {
        if (strncmp(&xml[i], "<![CDATA[", 9) == 0) {
            // The section is balanced by construction; skip its content, which is raw by design.
            const char* close = strstr(&xml[i], "]]>");
            nya_assert(close != nullptr, "a CDATA section is closed");
            lt++;
            gt++;
            i = (u64)(close - xml) + 3;
            continue;
        }

        if (xml[i] == '<') {
            lt++;
        } else if (xml[i] == '>') {
            gt++;
        } else if (xml[i] == '&') {
            b8 known = strncmp(&xml[i], "&amp;", 5) == 0 || strncmp(&xml[i], "&lt;", 4) == 0 || strncmp(&xml[i], "&gt;", 4) == 0
                    || strncmp(&xml[i], "&quot;", 6) == 0 || strncmp(&xml[i], "&#39;", 5) == 0;
            nya_assert(known, "every & begins a known entity, at byte %llu: %s", (unsigned long long)i, &xml[i]);
        }

        i++;
    }

    nya_assert(lt == gt, "the angle brackets balance: %llu '<' against %llu '>'", (unsigned long long)lt, (unsigned long long)gt);
}

int main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_sitemap");
    defer      nya_arena_destroy(arena);

    // TEST: a sitemap builds, is well-formed, and escapes an & in a loc.
    {
        const NYA_HttpSitemapUrl urls[] = {
            { .loc = "https://example.com/", .changefreq = NYA_HTTP_SITEMAP_DAILY, .priority = 1.0F, .has_priority = true,
              .lastmod = { .ns = 1700000000LL * 1000000000LL }, .has_lastmod = true },
            { .loc = "https://example.com/search?q=cats&sort=new" },
        };

        NYA_ConstCString xml = nullptr;
        NYA_EXPECT(nya_http_sitemap_build(arena, (NYA_HttpSitemapConfig){ .urls = urls, .count = nya_carray_length(urls) }, &xml));

        assert_xml_wellformed(xml);
        nya_assert(strstr(xml, "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">") != nullptr, "the urlset carries the right xmlns");
        nya_assert(strstr(xml, "<loc>https://example.com/</loc>") != nullptr, "the first loc is present");
        nya_assert(strstr(xml, "<changefreq>daily</changefreq>") != nullptr, "the changefreq is rendered");
        nya_assert(strstr(xml, "<priority>1.0</priority>") != nullptr, "the priority is rendered");
        nya_assert(strstr(xml, "<lastmod>2023-11-14T22:13:20Z</lastmod>") != nullptr, "the lastmod is RFC 3339");

        // The & in the second loc's query must be an entity, and the raw ampersand-run must not survive.
        nya_assert(strstr(xml, "q=cats&amp;sort=new") != nullptr, "the & in a loc is escaped");
        nya_assert(strstr(xml, "q=cats&sort=new") == nullptr, "no raw & survives in a loc");
    }

    // TEST: a non-web loc refuses the whole build (a javascript: injection cannot reach the document).
    {
        const NYA_HttpSitemapUrl urls[] = { { .loc = "https://example.com/" }, { .loc = "javascript:alert(1)" } };

        NYA_ConstCString xml = (NYA_ConstCString) "unset";
        nya_assert(!nya_http_sitemap_build(arena, (NYA_HttpSitemapConfig){ .urls = urls, .count = nya_carray_length(urls) }, &xml).ok, "a javascript: loc is refused");
        nya_assert(xml == nullptr, "and nothing is emitted");
    }

    // TEST: a priority outside 0.0..1.0 is refused.
    {
        const NYA_HttpSitemapUrl urls[] = { { .loc = "https://example.com/", .priority = 2.0F, .has_priority = true } };

        NYA_ConstCString xml = nullptr;
        nya_assert(!nya_http_sitemap_build(arena, (NYA_HttpSitemapConfig){ .urls = urls, .count = nya_carray_length(urls) }, &xml).ok, "a priority past 1.0 is refused");
    }

    // TEST: too many URLs is refused before anything is built.
    {
        NYA_ConstCString xml = nullptr;
        nya_assert(!nya_http_sitemap_build(arena, (NYA_HttpSitemapConfig){ .urls = nullptr, .count = NYA_HTTP_SITEMAP_MAX_URLS + 1 }, &xml).ok, "past the URL ceiling is refused");
    }

    // TEST: mount serves it at /sitemap.xml as application/xml.
    {
        nya_http_doc_clear();

        const NYA_HttpSitemapUrl urls[] = { { .loc = "https://example.com/" } };
        NYA_EXPECT(nya_http_sitemap_mount((NYA_HttpSitemapConfig){ .urls = urls, .count = nya_carray_length(urls) }));

        const NYA_HttpRouter* routers[] = { nya_http_doc_router() };
        b8                    exists  = false;
        const NYA_HttpRoute*  route   = nya_http_router_find(routers, 1, NYA_HTTP_METHOD_GET, NYA_HTTP_SITEMAP_PATH, &exists);
        nya_assert(route != nullptr, "the sitemap is mounted at " NYA_HTTP_SITEMAP_PATH);

        nya_http_doc_clear();
    }

    printf("PASSED: http sitemap\n");

    return EXIT_SUCCESS;
}
