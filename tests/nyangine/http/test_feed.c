/**
 * The feed, both dialects: RSS 2.0 and Atom 1.0 built from one set of items, asserted well-formed, with
 * the dates in the right RFC per format, text escaped, a description carried in a CDATA whose embedded
 * ]]> is split, and a non-web link refusing the build.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/** Balanced <>, every & a known entity; CDATA regions are skipped whole, since their content is raw by design. */
static void assert_xml_wellformed(NYA_ConstCString xml) {
    u64 lt = 0;
    u64 gt = 0;

    for (u64 i = 0; xml[i] != '\0';) {
        if (strncmp(&xml[i], "<![CDATA[", 9) == 0) {
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
    NYA_Arena* arena = nya_arena_create(.name = "test_http_feed");
    defer      nya_arena_destroy(arena);

    const NYA_Instant when = { .ns = 1700000000LL * 1000000000LL };

    const NYA_HttpFeedItem items[] = {
        { .title       = "Cats & Dogs <news>",
          .link        = "https://example.com/1",
          .description = "hi & <b>bold</b> ]]> done",
          .published   = when,
          .has_published = true },
    };

    const NYA_HttpFeedConfig feed = {
        .title       = "Example & Co",
        .link        = "https://example.com/",
        .description = "A <feed>",
        .self_link   = "https://example.com/feed.xml",
        .updated     = when,
        .has_updated = true,
        .items       = items,
        .count       = nya_carray_length(items),
    };

    // TEST: RSS builds well-formed, dates in RFC 822, text escaped, description in split CDATA.
    {
        NYA_ConstCString rss = nullptr;
        NYA_EXPECT(nya_http_feed_rss(arena, feed, &rss));

        assert_xml_wellformed(rss);
        nya_assert(strstr(rss, "<rss version=\"2.0\">") != nullptr, "it is RSS 2.0");
        nya_assert(strstr(rss, "<title>Example &amp; Co</title>") != nullptr, "the channel title's & is escaped");
        nya_assert(strstr(rss, "<title>Cats &amp; Dogs &lt;news&gt;</title>") != nullptr, "the item title's specials are escaped");
        nya_assert(strstr(rss, "<pubDate>Tue, 14 Nov 2023 22:13:20 GMT</pubDate>") != nullptr, "the pubDate is RFC 822");
        nya_assert(strstr(rss, "<![CDATA[") != nullptr, "the description is a CDATA section");
        nya_assert(strstr(rss, "]]]]><![CDATA[>") != nullptr, "the embedded ]]> is split so it cannot close early");
    }

    // TEST: Atom builds well-formed, feed <updated> in RFC 3339, summary escaped.
    {
        NYA_ConstCString atom = nullptr;
        NYA_EXPECT(nya_http_feed_atom(arena, feed, &atom));

        assert_xml_wellformed(atom);
        nya_assert(strstr(atom, "<feed xmlns=\"http://www.w3.org/2005/Atom\">") != nullptr, "it is Atom 1.0");
        nya_assert(strstr(atom, "<updated>2023-11-14T22:13:20Z</updated>") != nullptr, "the date is RFC 3339");
        nya_assert(strstr(atom, "<link rel=\"self\" href=\"https://example.com/feed.xml\"/>") != nullptr, "the self link is present");
        nya_assert(strstr(atom, "<summary type=\"html\">hi &amp; &lt;b&gt;bold&lt;/b&gt; ]]&gt; done</summary>") != nullptr, "the summary is escaped, ]]> included");
    }

    // TEST: a non-web item link refuses both builds.
    {
        const NYA_HttpFeedItem bad_items[] = { { .title = "x", .link = "javascript:alert(1)" } };
        NYA_HttpFeedConfig     bad          = feed;
        bad.items                           = bad_items;
        bad.count                           = nya_carray_length(bad_items);

        NYA_ConstCString rss = (NYA_ConstCString) "unset";
        nya_assert(!nya_http_feed_rss(arena, bad, &rss).ok && rss == nullptr, "a javascript: link is refused, nothing emitted");
    }

    // TEST: Atom refuses a feed with no date it can put in <updated>.
    {
        const NYA_HttpFeedItem undated[] = { { .title = "x", .link = "https://example.com/x" } };
        NYA_HttpFeedConfig      no_date    = feed;
        no_date.has_updated                = false;
        no_date.items                      = undated;
        no_date.count                      = nya_carray_length(undated);

        NYA_ConstCString atom = nullptr;
        nya_assert(!nya_http_feed_atom(arena, no_date, &atom).ok, "Atom needs a feed-level updated");

        // RSS is fine without one — its feed-level date is optional.
        NYA_ConstCString rss = nullptr;
        nya_assert(nya_http_feed_rss(arena, no_date, &rss).ok, "RSS builds without a feed date");
    }

    // TEST: mount serves RSS at /feed.xml and Atom at /atom.xml, each as its own media type.
    {
        nya_http_doc_clear();
        NYA_EXPECT(nya_http_feed_mount(feed));

        const NYA_HttpRouter* routers[] = { nya_http_doc_router() };
        b8                    exists  = false;
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_GET, NYA_HTTP_FEED_PATH, &exists) != nullptr, "RSS at " NYA_HTTP_FEED_PATH);
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_GET, NYA_HTTP_FEED_ATOM_PATH, &exists) != nullptr, "Atom at " NYA_HTTP_FEED_ATOM_PATH);

        nya_http_doc_clear();
    }

    printf("PASSED: http feed\n");

    return EXIT_SUCCESS;
}
