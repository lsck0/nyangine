#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_clock_format.h"
#include "nyangine-core/http/http_doc.h"
#include "nyangine-core/http/http_feed.h"

// PRIVATE API DECLARATION

/** The checks both dialects share: the channel's three required fields, the item count, and every link. */
NYA_INTERNAL NYA_Error _nya_http_feed_validate(NYA_HttpFeedConfig config);

/** The feed's date for Atom: the given one, else the newest item's, else `have` is false and Atom refuses. */
NYA_INTERNAL NYA_Instant _nya_http_feed_updated(NYA_HttpFeedConfig config, OUT b8* have);

// PUBLIC API IMPLEMENTATION

NYA_Error nya_http_feed_rss(NYA_Arena* arena, NYA_HttpFeedConfig config, OUT NYA_ConstCString* out) {
    nya_assert(arena != nullptr && out != nullptr);

    *out = nullptr;

    NYA_TRY(_nya_http_feed_validate(config));

    NYA_HttpDoc doc = nya_http_doc_over(arena, NYA_HTTP_DOC_MAX_BYTES);

    nya_http_doc_put(&doc, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    nya_http_doc_put(&doc, "<rss version=\"2.0\">\n  <channel>\n");

    nya_http_doc_put(&doc, "    <title>");
    nya_http_doc_xml_text(&doc, config.title);
    nya_http_doc_put(&doc, "</title>\n    <link>");
    nya_http_doc_xml_text(&doc, config.link);
    nya_http_doc_put(&doc, "</link>\n    <description>");
    nya_http_doc_xml_text(&doc, config.description);
    nya_http_doc_put(&doc, "</description>\n");

    // A feed-level date is optional in RSS; when there is one, it is lastBuildDate in RFC 822.
    b8          have_updated = false;
    NYA_Instant updated      = _nya_http_feed_updated(config, &have_updated);
    if (have_updated) {
        u8 stamp[NYA_RFC9110_LENGTH + 1] = { 0 };
        (void)nya_instant_to_rfc9110(updated, stamp, sizeof(stamp));

        nya_http_doc_put(&doc, "    <lastBuildDate>");
        nya_http_doc_put(&doc, (NYA_ConstCString)stamp);
        nya_http_doc_put(&doc, "</lastBuildDate>\n");
    }

    for (u32 i = 0; i < config.count; i++) {
        const NYA_HttpFeedItem* item = &config.items[i];

        nya_http_doc_put(&doc, "    <item>\n      <title>");
        nya_http_doc_xml_text(&doc, item->title);
        nya_http_doc_put(&doc, "</title>\n      <link>");
        nya_http_doc_xml_text(&doc, item->link);
        nya_http_doc_put(&doc, "</link>\n");

        // guid, or the link when none was given; isPermaLink="false" so a reader treats it as an opaque identity, not a URL to fetch, which is what a caller-supplied guid usually is.
        NYA_ConstCString guid = item->guid != nullptr && item->guid[0] != '\0' ? item->guid : item->link;
        nya_http_doc_put(&doc, "      <guid isPermaLink=\"false\">");
        nya_http_doc_xml_text(&doc, guid);
        nya_http_doc_put(&doc, "</guid>\n");

        if (item->description != nullptr && item->description[0] != '\0') {
            // CDATA, so a description may carry markup; a literal "]]>" in it is split, not left to close early.
            nya_http_doc_put(&doc, "      <description>");
            nya_http_doc_xml_cdata(&doc, item->description);
            nya_http_doc_put(&doc, "</description>\n");
        }

        if (item->has_published) {
            u8 stamp[NYA_RFC9110_LENGTH + 1] = { 0 };
            (void)nya_instant_to_rfc9110(item->published, stamp, sizeof(stamp));

            nya_http_doc_put(&doc, "      <pubDate>");
            nya_http_doc_put(&doc, (NYA_ConstCString)stamp);
            nya_http_doc_put(&doc, "</pubDate>\n");
        }

        nya_http_doc_put(&doc, "    </item>\n");
    }

    nya_http_doc_put(&doc, "  </channel>\n</rss>\n");

    return nya_http_doc_finish(&doc, out);
}

NYA_Error nya_http_feed_atom(NYA_Arena* arena, NYA_HttpFeedConfig config, OUT NYA_ConstCString* out) {
    nya_assert(arena != nullptr && out != nullptr);

    *out = nullptr;

    NYA_TRY(_nya_http_feed_validate(config));

    if (config.self_link != nullptr && config.self_link[0] != '\0' && !nya_http_doc_url_is_web(config.self_link)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the feed self link '%s' is not an http or https URL", config.self_link);
    }

    // Atom requires a feed-level <updated>; with no given one and no dated item there is none to write, so the feed is refused rather than emitted invalid.
    b8          have_updated = false;
    NYA_Instant updated      = _nya_http_feed_updated(config, &have_updated);
    if (!have_updated) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an Atom feed needs an updated date: set has_updated or give an item a date");
    }

    u8 feed_stamp[NYA_RFC3339_LENGTH_MAX + 1] = { 0 };
    (void)nya_instant_to_rfc3339(updated, feed_stamp, sizeof(feed_stamp));

    // The feed's id: its own URL when it has one, else the site link. A stable IRI either way.
    NYA_ConstCString feed_id = config.self_link != nullptr && config.self_link[0] != '\0' ? config.self_link : config.link;

    NYA_HttpDoc doc = nya_http_doc_over(arena, NYA_HTTP_DOC_MAX_BYTES);

    nya_http_doc_put(&doc, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    nya_http_doc_put(&doc, "<feed xmlns=\"http://www.w3.org/2005/Atom\">\n");

    nya_http_doc_put(&doc, "  <title>");
    nya_http_doc_xml_text(&doc, config.title);
    nya_http_doc_put(&doc, "</title>\n  <subtitle>");
    nya_http_doc_xml_text(&doc, config.description);
    nya_http_doc_put(&doc, "</subtitle>\n");

    nya_http_doc_put(&doc, "  <link href=\"");
    nya_http_doc_xml_attr(&doc, config.link);
    nya_http_doc_put(&doc, "\"/>\n");

    if (config.self_link != nullptr && config.self_link[0] != '\0') {
        nya_http_doc_put(&doc, "  <link rel=\"self\" href=\"");
        nya_http_doc_xml_attr(&doc, config.self_link);
        nya_http_doc_put(&doc, "\"/>\n");
    }

    nya_http_doc_put(&doc, "  <id>");
    nya_http_doc_xml_text(&doc, feed_id);
    nya_http_doc_put(&doc, "</id>\n  <updated>");
    nya_http_doc_put(&doc, (NYA_ConstCString)feed_stamp);
    nya_http_doc_put(&doc, "</updated>\n");

    for (u32 i = 0; i < config.count; i++) {
        const NYA_HttpFeedItem* item = &config.items[i];

        nya_http_doc_put(&doc, "  <entry>\n    <title>");
        nya_http_doc_xml_text(&doc, item->title);
        nya_http_doc_put(&doc, "</title>\n    <link href=\"");
        nya_http_doc_xml_attr(&doc, item->link);
        nya_http_doc_put(&doc, "\"/>\n");

        NYA_ConstCString entry_id = item->guid != nullptr && item->guid[0] != '\0' ? item->guid : item->link;
        nya_http_doc_put(&doc, "    <id>");
        nya_http_doc_xml_text(&doc, entry_id);
        nya_http_doc_put(&doc, "</id>\n");

        // An entry needs its own <updated>: its date when it has one, else the feed's, always present here since the feed refused above without one.
        NYA_Instant entry_updated = item->has_published ? item->published : updated;

        u8 entry_stamp[NYA_RFC3339_LENGTH_MAX + 1] = { 0 };
        (void)nya_instant_to_rfc3339(entry_updated, entry_stamp, sizeof(entry_stamp));

        nya_http_doc_put(&doc, "    <updated>");
        nya_http_doc_put(&doc, (NYA_ConstCString)entry_stamp);
        nya_http_doc_put(&doc, "</updated>\n");

        if (item->description != nullptr && item->description[0] != '\0') {
            // type="html" says the escaped text is markup, so a reader unescapes it back to what it was.
            nya_http_doc_put(&doc, "    <summary type=\"html\">");
            nya_http_doc_xml_text(&doc, item->description);
            nya_http_doc_put(&doc, "</summary>\n");
        }

        nya_http_doc_put(&doc, "  </entry>\n");
    }

    nya_http_doc_put(&doc, "</feed>\n");

    return nya_http_doc_finish(&doc, out);
}

NYA_Error nya_http_feed_mount(NYA_HttpFeedConfig config) {
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "feed_mount");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_ConstCString rss = nullptr;
    NYA_TRY(nya_http_feed_rss(&scratch, config, &rss));
    NYA_TRY(nya_http_doc_serve(NYA_HTTP_FEED_PATH, NYA_HTTP_MEDIA_RSS, rss, "The feed: recent items, as RSS 2.0"));

    NYA_ConstCString atom = nullptr;
    NYA_TRY(nya_http_feed_atom(&scratch, config, &atom));

    return nya_http_doc_serve(NYA_HTTP_FEED_ATOM_PATH, NYA_HTTP_MEDIA_ATOM, atom, "The feed: recent items, as Atom 1.0");
}

// PRIVATE API IMPLEMENTATION

NYA_Error _nya_http_feed_validate(NYA_HttpFeedConfig config) {
    if (config.title == nullptr || config.title[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a feed needs a channel title");
    if (config.description == nullptr || config.description[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a feed needs a channel description");

    if (!nya_http_doc_url_is_web(config.link)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the channel link '%s' is not an http or https URL", config.link != nullptr ? config.link : "(null)");
    }

    if (config.count > NYA_HTTP_FEED_MAX_ITEMS) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a feed holds at most %d items, not " FMTu32, NYA_HTTP_FEED_MAX_ITEMS, config.count);
    }

    for (u32 i = 0; i < config.count; i++) {
        const NYA_HttpFeedItem* item = &config.items[i];

        if (item->title == nullptr || item->title[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "feed item " FMTu32 " has no title", i);

        if (!nya_http_doc_url_is_web(item->link)) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "feed item " FMTu32 " link '%s' is not an http or https URL", i, item->link != nullptr ? item->link : "(null)");
        }
    }

    return NYA_OK;
}

NYA_Instant _nya_http_feed_updated(NYA_HttpFeedConfig config, OUT b8* have) {
    if (config.has_updated) {
        *have = true;
        return config.updated;
    }

    // The newest dated item stands in for a feed with no date of its own.
    NYA_Instant newest = { 0 };
    *have              = false;

    for (u32 i = 0; i < config.count; i++) {
        if (!config.items[i].has_published) continue;

        if (!*have || config.items[i].published.ns > newest.ns) newest = config.items[i].published;
        *have = true;
    }

    return newest;
}
