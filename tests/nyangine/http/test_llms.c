/**
 * llms.txt: the Markdown document built from sections of links, the strict preset that states restricted
 * use up front, and the escaping and URL rules that keep a link from breaking its own syntax.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

int main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_http_llms");
    defer      nya_arena_destroy(arena);

    const NYA_HttpLlmsLink links[] = {
        { .title = "API reference", .url = "https://example.com/api", .note = "every endpoint" },
        { .title = "Bracket ] test", .url = "https://example.com/b" },
    };
    const NYA_HttpLlmsSection sections[] = { { .heading = "Docs", .links = links, .link_count = nya_carray_length(links) } };

    const NYA_HttpLlmsConfig config = {
        .name          = "Example",
        .summary       = "An example site.",
        .sections      = sections,
        .section_count = nya_carray_length(sections),
    };

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the plain document has the H1, the blockquote, the section and the links.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_ConstCString md = nullptr;
        NYA_EXPECT(nya_http_llms_build(arena, config, &md));

        nya_assert(strstr(md, "# Example\n") != nullptr, "the H1 site name");
        nya_assert(strstr(md, "> An example site.\n") != nullptr, "the blockquote summary");
        nya_assert(strstr(md, "## Docs\n") != nullptr, "the section heading");
        nya_assert(strstr(md, "- [API reference](https://example.com/api): every endpoint\n") != nullptr, "a link with its note");

        // The ] in the second title is Markdown-escaped so it cannot close the link text early.
        nya_assert(strstr(md, "- [Bracket \\] test](https://example.com/b)\n") != nullptr, "a ] in a title is escaped");

        // The plain build has no restricted-use notice.
        nya_assert(strstr(md, NYA_HTTP_LLMS_STRICT_NOTICE) == nullptr, "the plain build makes no restriction claim");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the strict document carries the restricted-use notice, near the top.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_ConstCString md = nullptr;
        NYA_EXPECT(nya_http_llms_strict(arena, config, &md));

        const char* notice = strstr(md, NYA_HTTP_LLMS_STRICT_NOTICE);
        nya_assert(notice != nullptr, "the strict build states restricted use");

        // Before the first section, so a reader meets it first.
        const char* first_section = strstr(md, "## Docs");
        nya_assert(first_section != nullptr && notice < first_section, "the notice comes before the content");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: a non-web URL, a URL with a parenthesis, and a control byte each refuse the build.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        NYA_ConstCString md = (NYA_ConstCString) "unset";

        const NYA_HttpLlmsLink    js[]    = { { .title = "x", .url = "javascript:alert(1)" } };
        const NYA_HttpLlmsSection js_sec[] = { { .heading = "S", .links = js, .link_count = 1 } };
        NYA_HttpLlmsConfig        bad       = config;
        bad.sections                       = js_sec;
        bad.section_count                  = 1;
        nya_assert(!nya_http_llms_build(arena, bad, &md).ok && md == nullptr, "a javascript: URL is refused, nothing emitted");

        const NYA_HttpLlmsLink    paren[]    = { { .title = "x", .url = "https://example.com/a(b)" } };
        const NYA_HttpLlmsSection paren_sec[] = { { .heading = "S", .links = paren, .link_count = 1 } };
        bad.sections                          = paren_sec;
        nya_assert(!nya_http_llms_build(arena, bad, &md).ok, "a parenthesis in a URL is refused");

        NYA_HttpLlmsConfig ctrl = config;
        ctrl.summary            = "line one\nline two";
        nya_assert(!nya_http_llms_build(arena, ctrl, &md).ok, "a newline in the summary is refused");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: both mounts land at /llms.txt as text/markdown.
    // ─────────────────────────────────────────────────────────────────────────────
    {
        const NYA_HttpRouter* routers[] = { nya_http_doc_router() };
        b8                    exists  = false;

        nya_http_doc_clear();
        NYA_EXPECT(nya_http_llms_mount(config));
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_GET, NYA_HTTP_LLMS_PATH, &exists) != nullptr, "the plain mount lands at " NYA_HTTP_LLMS_PATH);

        nya_http_doc_clear();
        NYA_EXPECT(nya_http_llms_mount_strict(config));
        nya_assert(nya_http_router_find(routers, 1, NYA_HTTP_METHOD_GET, NYA_HTTP_LLMS_PATH, &exists) != nullptr, "the strict mount lands there too");

        nya_http_doc_clear();
    }

    printf("PASSED: http llms\n");

    return EXIT_SUCCESS;
}
