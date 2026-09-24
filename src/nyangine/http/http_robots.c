#include "nyangine/base/base_assert.h"
#include "nyangine/http/http_doc.h"
#include "nyangine/http/http_robots.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The deny-all preset, spelled once. A comment so a person reading the file sees why it is empty of
 * everything but the one rule, then the rule itself: every crawler, nothing allowed.
 */
NYA_INTERNAL const NYA_ConstCString _NYA_HTTP_ROBOTS_STRICT = "# This site asks not to be crawled.\n"
                                                             "User-agent: *\n"
                                                             "Disallow: /\n";

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Whether `text` is safe on a robots.txt line: no control byte, since the format has no way to escape one. */
NYA_INTERNAL b8 _nya_http_robots_line_safe(NYA_ConstCString text) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_http_robots_build(NYA_Arena* arena, NYA_HttpRobotsConfig config, OUT NYA_ConstCString* out) {
    nya_assert(arena != nullptr && out != nullptr);

    *out = nullptr;

    if (config.groups == nullptr || config.count == 0) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a robots.txt needs at least one group");

    if (config.count > NYA_HTTP_ROBOTS_MAX_GROUPS) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a robots.txt holds at most %d groups, not " FMTu32, NYA_HTTP_ROBOTS_MAX_GROUPS, config.count);
    }

    if (config.sitemap != nullptr && config.sitemap[0] != '\0' && !nya_http_doc_url_is_web(config.sitemap)) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the Sitemap '%s' is not an http or https URL", config.sitemap);
    }

    NYA_HttpDoc doc = nya_http_doc_over(arena, NYA_HTTP_ROBOTS_MAX_BYTES);

    for (u32 g = 0; g < config.count; g++) {
        const NYA_HttpRobotsGroup* group = &config.groups[g];

        if (group->user_agent == nullptr || group->user_agent[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "group " FMTu32 " has no user-agent", g);
        if (!_nya_http_robots_line_safe(group->user_agent)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the user-agent in group " FMTu32 " carries a control byte", g);

        // A blank line separates one group from the next, as the format expects; none before the first.
        if (g > 0) nya_http_doc_put(&doc, "\n");

        nya_http_doc_put(&doc, "User-agent: ");
        nya_http_doc_put(&doc, group->user_agent);
        nya_http_doc_put(&doc, "\n");

        for (u32 a = 0; a < group->allow_count; a++) {
            if (!_nya_http_robots_line_safe(group->allow[a])) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an Allow path in group " FMTu32 " carries a control byte", g);

            nya_http_doc_put(&doc, "Allow: ");
            nya_http_doc_put(&doc, group->allow[a]);
            nya_http_doc_put(&doc, "\n");
        }

        for (u32 d = 0; d < group->disallow_count; d++) {
            if (!_nya_http_robots_line_safe(group->disallow[d])) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a Disallow path in group " FMTu32 " carries a control byte", g);

            nya_http_doc_put(&doc, "Disallow: ");
            nya_http_doc_put(&doc, group->disallow[d]);
            nya_http_doc_put(&doc, "\n");
        }

        if (group->crawl_delay_s > 0) nya_http_doc_putf(&doc, "Crawl-delay: " FMTu32 "\n", group->crawl_delay_s);
    }

    if (config.sitemap != nullptr && config.sitemap[0] != '\0') {
        nya_http_doc_put(&doc, "\nSitemap: ");
        nya_http_doc_put(&doc, config.sitemap);
        nya_http_doc_put(&doc, "\n");
    }

    return nya_http_doc_finish(&doc, out);
}

NYA_ConstCString nya_http_robots_strict(void) {
    return _NYA_HTTP_ROBOTS_STRICT;
}

NYA_Error nya_http_robots_mount(NYA_HttpRobotsConfig config) {
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "robots_mount");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_ConstCString text = nullptr;
    NYA_TRY(nya_http_robots_build(&scratch, config, &text));

    return nya_http_doc_serve(NYA_HTTP_ROBOTS_PATH, NYA_HTTP_MEDIA_TEXT, text, "The crawl rules");
}

NYA_Error nya_http_robots_mount_strict(void) {
    return nya_http_doc_serve(NYA_HTTP_ROBOTS_PATH, NYA_HTTP_MEDIA_TEXT, nya_http_robots_strict(), "The crawl rules: deny all");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_http_robots_line_safe(NYA_ConstCString text) {
    if (text == nullptr) return true;

    for (u64 i = 0; text[i] != '\0'; i++) {
        // Anything below a space is a control byte — a CR or an LF among them — and could split one line
        // into two directives, which is the one thing a format with no escape cannot allow.
        if ((unsigned char)text[i] < 0x20) return false;
    }

    return true;
}
