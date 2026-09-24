#include "nyangine/base/base_assert.h"
#include "nyangine/base/base_clock_format.h"
#include "nyangine/http/http_doc.h"
#include "nyangine/http/http_sitemap.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** The changefreq vocabulary as text. NONE is the empty string, which the builder never emits. */
NYA_INTERNAL const NYA_ConstCString _NYA_HTTP_SITEMAP_CHANGEFREQ[NYA_HTTP_SITEMAP_CHANGEFREQ_COUNT] = {
    [NYA_HTTP_SITEMAP_CHANGEFREQ_NONE] = "",
    [NYA_HTTP_SITEMAP_ALWAYS]          = "always",
    [NYA_HTTP_SITEMAP_HOURLY]          = "hourly",
    [NYA_HTTP_SITEMAP_DAILY]           = "daily",
    [NYA_HTTP_SITEMAP_WEEKLY]          = "weekly",
    [NYA_HTTP_SITEMAP_MONTHLY]         = "monthly",
    [NYA_HTTP_SITEMAP_YEARLY]          = "yearly",
    [NYA_HTTP_SITEMAP_NEVER]           = "never",
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_http_sitemap_build(NYA_Arena* arena, NYA_HttpSitemapConfig config, OUT NYA_ConstCString* out) {
    nya_assert(arena != nullptr && out != nullptr);

    *out = nullptr;

    if (config.count > NYA_HTTP_SITEMAP_MAX_URLS) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a sitemap holds at most %d URLs, not " FMTu32, NYA_HTTP_SITEMAP_MAX_URLS, config.count);
    }

    NYA_HttpDoc doc = nya_http_doc_over(arena, NYA_HTTP_DOC_MAX_BYTES);

    nya_http_doc_put(&doc, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    nya_http_doc_put(&doc, "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n");

    for (u32 i = 0; i < config.count; i++) {
        const NYA_HttpSitemapUrl* url = &config.urls[i];

        // The one hard gate: a loc a crawler would not fetch, or one carrying a scheme that is an
        // injection here, refuses the whole build rather than being emitted or silently dropped.
        if (!nya_http_doc_url_is_web(url->loc)) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the loc '%s' is not an http or https URL", url->loc != nullptr ? url->loc : "(null)");
        }

        if (url->has_priority && (url->priority < 0.0F || url->priority > 1.0F)) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a priority is 0.0 to 1.0, not %f", (f64)url->priority);
        }

        nya_http_doc_put(&doc, "  <url>\n    <loc>");
        nya_http_doc_xml_text(&doc, url->loc);
        nya_http_doc_put(&doc, "</loc>\n");

        if (url->has_lastmod) {
            u8 stamp[NYA_RFC3339_LENGTH_MAX + 1] = { 0 };
            (void)nya_instant_to_rfc3339(url->lastmod, stamp, sizeof(stamp));

            nya_http_doc_put(&doc, "    <lastmod>");
            nya_http_doc_put(&doc, (NYA_ConstCString)stamp);
            nya_http_doc_put(&doc, "</lastmod>\n");
        }

        if (url->changefreq != NYA_HTTP_SITEMAP_CHANGEFREQ_NONE && url->changefreq < NYA_HTTP_SITEMAP_CHANGEFREQ_COUNT) {
            nya_http_doc_put(&doc, "    <changefreq>");
            nya_http_doc_put(&doc, _NYA_HTTP_SITEMAP_CHANGEFREQ[url->changefreq]);
            nya_http_doc_put(&doc, "</changefreq>\n");
        }

        if (url->has_priority) nya_http_doc_putf(&doc, "    <priority>%.1f</priority>\n", (f64)url->priority);

        nya_http_doc_put(&doc, "  </url>\n");
    }

    nya_http_doc_put(&doc, "</urlset>\n");

    return nya_http_doc_finish(&doc, out);
}

NYA_Error nya_http_sitemap_mount(NYA_HttpSitemapConfig config) {
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "sitemap_mount");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_ConstCString xml = nullptr;
    NYA_TRY(nya_http_sitemap_build(&scratch, config, &xml));

    // Copied into the registry by serve, so this scratch arena is free to die on the defer above.
    return nya_http_doc_serve(NYA_HTTP_SITEMAP_PATH, NYA_HTTP_MEDIA_XML, xml, "The sitemap: the URLs this site wants crawled");
}
