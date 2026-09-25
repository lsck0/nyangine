/**
 * The social-media embedding metadata: that an NYA_PageMeta becomes the OpenGraph, Twitter Card and
 * description tags in the SSR `<head>`, that every value is escaped so none can break out of its attribute,
 * that an unset field emits no tag and a zeroed value changes nothing, that a `javascript:` URL is refused,
 * and that the oEmbed helper builds a JSON document that parses back with the right fields.
 *
 * It runs headless and with no browser or server: the presenter and the oEmbed builder write into a caller
 * buffer and an arena, the same property that makes the rest of this backend checkable. See ui_present_html.h.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

static NYA_UIHtml html;

/** A page-metadata value whose fields carry the injection characters, so escaping is under test throughout. */
static const NYA_PageMeta META = {
    .title         = "Ada & \"friends\" <tag>",
    .description   = "A blurb with <b>markup</b> & a \"quote\".",
    .canonical_url = "https://example.com/page?a=1&b=2",
    .image_url     = "https://example.com/preview.png",
    .image_alt     = "alt & <text>",
    .site_name     = "nyangine",
    .author_name   = "Ada",
    .type          = "article",
    .twitter_card  = NYA_TWITTER_CARD_SUMMARY_LARGE_IMAGE,
    .locale        = "en_US",
    .oembed_url    = "https://example.com/oembed?url=x",
};

s32 main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

    NYA_Arena* arena = nya_arena_create();
    defer nya_arena_destroy(arena);

    nya_ui_html_init(&html, NYA_UI_HTML_CELL);
    defer nya_ui_html_deinit(&html);

    static char page[NYA_UI_HTML_MAX + 8192];

    // TEST: a null and a zeroed metadata value change nothing about the default page.
    {
        static char without[sizeof(page)];
        static char zeroed[sizeof(page)];

        u32 a = nya_ui_html_document(&html, without, sizeof(without), "title", "");
        u32 b = nya_ui_html_document_meta(&html, zeroed, sizeof(zeroed), "title", "", &(NYA_PageMeta){ 0 });

        nya_check(a > 0 && b > 0, "both documents are written");
        nya_check(nya_string_equals(without, zeroed), "a zeroed NYA_PageMeta emits nothing new — the page is byte-for-byte the old one");
        nya_check(!nya_string_contains(zeroed, "og:"), "and carries no OpenGraph tag");
    }

    // TEST: a filled value emits the description, OpenGraph and Twitter Card tags.
    u32 written = nya_ui_html_document_meta(&html, page, sizeof(page), "nyangine", "", &META);
    nya_check(written > 0, "the document is written");

    nya_check(nya_string_contains(page, "<meta name=\"description\" content=\""), "the standard description tag is emitted");
    nya_check(nya_string_contains(page, "<meta property=\"og:title\" content=\""), "og:title is emitted");
    nya_check(nya_string_contains(page, "<meta property=\"og:description\" content=\""), "og:description is emitted");
    nya_check(nya_string_contains(page, "<meta property=\"og:type\" content=\"article\">"), "og:type carries the value");
    nya_check(nya_string_contains(page, "<meta property=\"og:url\" content=\"https://example.com/page?a=1&amp;b=2\">"), "og:url carries the escaped canonical URL");
    nya_check(nya_string_contains(page, "<meta property=\"og:image\" content=\"https://example.com/preview.png\">"), "og:image carries the image URL");
    nya_check(nya_string_contains(page, "<meta property=\"og:image:alt\" content=\""), "og:image:alt rides along with the image");
    nya_check(nya_string_contains(page, "<meta property=\"og:site_name\" content=\"nyangine\">"), "og:site_name carries the value");
    nya_check(nya_string_contains(page, "<meta property=\"og:locale\" content=\"en_US\">"), "og:locale carries the value");
    nya_check(nya_string_contains(page, "<meta name=\"twitter:card\" content=\"summary_large_image\">"), "twitter:card is the chosen card");
    nya_check(nya_string_contains(page, "<meta name=\"twitter:title\" content=\""), "twitter:title is emitted");
    nya_check(nya_string_contains(page, "<meta name=\"twitter:description\" content=\""), "twitter:description is emitted");
    nya_check(nya_string_contains(page, "<meta name=\"twitter:image\" content=\"https://example.com/preview.png\">"), "twitter:image carries the image URL");
    nya_check(nya_string_contains(page, "<link rel=\"alternate\" type=\"application/json+oembed\" href=\"https://example.com/oembed?url=x\""), "the oEmbed discovery link is emitted");

    // TEST: every value is escaped — no raw injection reaches the head.
    nya_check(!nya_string_contains(page, "<tag>"), "a raw '<' from a field never reaches the page as markup");
    nya_check(!nya_string_contains(page, "<b>markup</b>"), "nor a raw markup blurb");
    nya_check(nya_string_contains(page, "Ada &amp; &quot;friends&quot; &lt;tag&gt;"), "the title's &, \" and <> come out escaped");
    nya_check(nya_string_contains(page, "&lt;b&gt;markup&lt;/b&gt;"), "the description's markup comes out escaped");
    // the `<title>` element is still the caller's, escaped, unchanged.
    nya_check(nya_string_contains(page, "<title>nyangine</title>"), "the existing <title> behaviour is kept");

    // TEST: an unset field emits no tag, even alongside set ones.
    {
        NYA_PageMeta partial = { .title = "Only a title", .type = "website" };
        static char  only[sizeof(page)];

        (void)nya_ui_html_document_meta(&html, only, sizeof(only), "t", "", &partial);

        nya_check(nya_string_contains(only, "<meta property=\"og:title\""), "og:title is emitted when set");
        nya_check(!nya_string_contains(only, "og:description"), "og:description is absent when the field is unset");
        nya_check(!nya_string_contains(only, "og:image"), "og:image is absent when the field is unset");
        nya_check(!nya_string_contains(only, "twitter:card"), "no twitter tag without a chosen card");
        nya_check(!nya_string_contains(only, "og:url"), "no og:url without a canonical URL");
    }

    // TEST: a dangerous URL is refused by the gate and never reaches the head.
    {
        nya_check(nya_ui_page_meta_url_ok("https://example.com/x"), "an https URL passes the gate");
        nya_check(nya_ui_page_meta_url_ok("http://example.com/x"), "an http URL passes the gate");
        nya_check(!nya_ui_page_meta_url_ok("javascript:alert(1)"), "a javascript: URL is refused");
        nya_check(!nya_ui_page_meta_url_ok("data:text/html,x"), "a data: URL is refused");
        nya_check(!nya_ui_page_meta_url_ok(""), "the empty string is refused");
        nya_check(!nya_ui_page_meta_url_ok(nullptr), "null is refused");

        NYA_PageMeta evil = { .title = "t", .canonical_url = "javascript:alert(1)", .image_url = "data:text/html,x" };
        static char  out[sizeof(page)];

        (void)nya_ui_html_document_meta(&html, out, sizeof(out), "t", "", &evil);

        nya_check(!nya_string_contains(out, "javascript:"), "the javascript: canonical URL never reaches og:url");
        nya_check(!nya_string_contains(out, "data:text/html"), "the data: image URL never reaches og:image");
        nya_check(!nya_string_contains(out, "og:url"), "with no valid URL, og:url is not emitted at all");
        nya_check(!nya_string_contains(out, "og:image"), "and og:image is not emitted at all");
    }

    // TEST: the oEmbed helper builds a JSON document that parses back with the fields.
    {
        NYA_Object* document = nullptr;
        NYA_Error   built    = nya_ui_page_meta_oembed(arena, &META, &document);
        nya_check(built.ok && document != nullptr, "the oEmbed document is built");

        NYA_String* json = nya_serialize(arena, document, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);
        nya_check(json != nullptr && json->length > 0, "and serializes to JSON");

        // parse it straight back — a consumer's job — and read the fields off.
        NYA_Object* parsed = nullptr;
        NYA_Error   ok     = nya_deserialize(arena, (const u8*)json->items, json->length, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE, &parsed);
        nya_check(ok.ok && parsed != nullptr, "the JSON parses back with nya_deserialize");

        NYA_Value* version = nya_object_get(parsed, "version");
        NYA_Value* type    = nya_object_get(parsed, "type");
        NYA_Value* title   = nya_object_get(parsed, "title");
        NYA_Value* provider = nya_object_get(parsed, "provider_name");
        NYA_Value* author  = nya_object_get(parsed, "author_name");
        NYA_Value* thumb   = nya_object_get(parsed, "thumbnail_url");

        nya_check(version != nullptr && version->type == NYA_TYPE_STRING && nya_string_equals(version->as_string, "1.0"), "version is \"1.0\"");
        nya_check(type != nullptr && type->type == NYA_TYPE_STRING && nya_string_equals(type->as_string, "link"), "type is \"link\"");
        nya_check(title != nullptr && title->type == NYA_TYPE_STRING, "the title round-trips");
        // the JSON decode has undone the escaping, so the field reads as the original bytes.
        nya_check(title != nullptr && nya_string_equals(title->as_string, "Ada & \"friends\" <tag>"), "and reads back as the original, unescaped");
        nya_check(provider != nullptr && nya_string_equals(provider->as_string, "nyangine"), "provider_name is the site name");
        nya_check(author != nullptr && nya_string_equals(author->as_string, "Ada"), "author_name round-trips");
        nya_check(thumb != nullptr && nya_string_equals(thumb->as_string, "https://example.com/preview.png"), "thumbnail_url is the image URL");
    }

    // TEST: an oEmbed built from a value whose image URL is dangerous carries no thumbnail.
    {
        NYA_PageMeta evil     = { .title = "t", .image_url = "javascript:alert(1)" };
        NYA_Object*  document = nullptr;
        nya_check(nya_ui_page_meta_oembed(arena, &evil, &document).ok, "the document is still built");
        nya_check(nya_object_get(document, "thumbnail_url") == nullptr, "but a javascript: image URL yields no thumbnail_url");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
