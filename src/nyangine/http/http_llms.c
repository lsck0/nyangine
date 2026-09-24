#include <string.h>

#include "nyangine/base/base_assert.h"
#include "nyangine/http/http_doc.h"
#include "nyangine/http/http_llms.h"

// PRIVATE API DECLARATION

/** Whether `text` carries no control byte, so it cannot restructure the Markdown with a stray newline. */
NYA_INTERNAL b8 _nya_http_llms_field_safe(NYA_ConstCString text) __attr_no_discard;

/** Appends a link title with Markdown's `[`, `]` and `\` backslash-escaped, so a `]` cannot close the link. */
NYA_INTERNAL void _nya_http_llms_title(NYA_HttpDoc* doc, NYA_ConstCString title);

/** The shared builder behind the plain and strict entry points; `strict` writes the restricted-use notice. */
NYA_INTERNAL NYA_Error _nya_http_llms_build(NYA_Arena* arena, NYA_HttpLlmsConfig config, b8 strict, OUT NYA_ConstCString* out);

// PUBLIC API IMPLEMENTATION

NYA_Error nya_http_llms_build(NYA_Arena* arena, NYA_HttpLlmsConfig config, OUT NYA_ConstCString* out) {
    return _nya_http_llms_build(arena, config, false, out);
}

NYA_Error nya_http_llms_strict(NYA_Arena* arena, NYA_HttpLlmsConfig config, OUT NYA_ConstCString* out) {
    return _nya_http_llms_build(arena, config, true, out);
}

NYA_Error nya_http_llms_mount(NYA_HttpLlmsConfig config) {
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "llms_mount");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_ConstCString text = nullptr;
    NYA_TRY(nya_http_llms_build(&scratch, config, &text));

    return nya_http_doc_serve(NYA_HTTP_LLMS_PATH, NYA_HTTP_MEDIA_MARKDOWN, text, "A guide to this site's key content, for an LLM");
}

NYA_Error nya_http_llms_mount_strict(NYA_HttpLlmsConfig config) {
    NYA_Arena scratch = nya_arena_create_on_stack(.name = "llms_mount_strict");
    defer     nya_arena_destroy_on_stack(&scratch);

    NYA_ConstCString text = nullptr;
    NYA_TRY(nya_http_llms_strict(&scratch, config, &text));

    return nya_http_doc_serve(NYA_HTTP_LLMS_PATH, NYA_HTTP_MEDIA_MARKDOWN, text, "A guide to this site's key content, use restricted");
}

// PRIVATE API IMPLEMENTATION

NYA_Error _nya_http_llms_build(NYA_Arena* arena, NYA_HttpLlmsConfig config, b8 strict, OUT NYA_ConstCString* out) {
    nya_assert(arena != nullptr && out != nullptr);

    *out = nullptr;

    if (config.name == nullptr || config.name[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an llms.txt needs a site name");
    if (!_nya_http_llms_field_safe(config.name)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the site name carries a control byte");

    if (config.summary != nullptr && !_nya_http_llms_field_safe(config.summary)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the summary carries a control byte");
    if (config.notes != nullptr && !_nya_http_llms_field_safe(config.notes)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the notes carry a control byte");

    if (config.section_count > NYA_HTTP_LLMS_MAX_SECTIONS) {
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "an llms.txt holds at most %d sections, not " FMTu32, NYA_HTTP_LLMS_MAX_SECTIONS, config.section_count);
    }

    NYA_HttpDoc doc = nya_http_doc_over(arena, NYA_HTTP_DOC_MAX_BYTES);

    // The H1: the site name.
    nya_http_doc_put(&doc, "# ");
    nya_http_doc_put(&doc, config.name);
    nya_http_doc_put(&doc, "\n\n");

    // The restricted-use notice up front, so a model meets it before the links; its own blockquote above the summary, so it reads as the site's terms rather than a description of the site.
    if (strict) {
        nya_http_doc_put(&doc, "> ");
        nya_http_doc_put(&doc, NYA_HTTP_LLMS_STRICT_NOTICE);
        nya_http_doc_put(&doc, "\n\n");
    }

    if (config.summary != nullptr && config.summary[0] != '\0') {
        nya_http_doc_put(&doc, "> ");
        nya_http_doc_put(&doc, config.summary);
        nya_http_doc_put(&doc, "\n\n");
    }

    if (config.notes != nullptr && config.notes[0] != '\0') {
        nya_http_doc_put(&doc, config.notes);
        nya_http_doc_put(&doc, "\n\n");
    }

    for (u32 s = 0; s < config.section_count; s++) {
        const NYA_HttpLlmsSection* section = &config.sections[s];

        if (section->heading == nullptr || section->heading[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "section " FMTu32 " has no heading", s);
        if (!_nya_http_llms_field_safe(section->heading)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the heading of section " FMTu32 " carries a control byte", s);

        if (section->link_count > NYA_HTTP_LLMS_MAX_LINKS) {
            return nya_error(NYA_ERROR_INVALID_ARGUMENT, "section " FMTu32 " holds at most %d links, not " FMTu32, s, NYA_HTTP_LLMS_MAX_LINKS, section->link_count);
        }

        nya_http_doc_put(&doc, "## ");
        nya_http_doc_put(&doc, section->heading);
        nya_http_doc_put(&doc, "\n\n");

        for (u32 l = 0; l < section->link_count; l++) {
            const NYA_HttpLlmsLink* link = &section->links[l];

            if (link->title == nullptr || link->title[0] == '\0') return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a link in section " FMTu32 " has no title", s);
            if (!_nya_http_llms_field_safe(link->title)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a link title in section " FMTu32 " carries a control byte", s);

            if (!nya_http_doc_url_is_web(link->url)) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a link URL in section " FMTu32 " is not an http or https URL", s);
            }

            // A parenthesis in the URL would end or unbalance the `(url)`, and Markdown has no escape for one inside a link destination, so it's refused rather than emitted broken.
            if (strchr(link->url, '(') != nullptr || strchr(link->url, ')') != nullptr) {
                return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a link URL in section " FMTu32 " carries a parenthesis", s);
            }

            if (link->note != nullptr && !_nya_http_llms_field_safe(link->note)) return nya_error(NYA_ERROR_INVALID_ARGUMENT, "a link note in section " FMTu32 " carries a control byte", s);

            nya_http_doc_put(&doc, "- [");
            _nya_http_llms_title(&doc, link->title);
            nya_http_doc_put(&doc, "](");
            nya_http_doc_put(&doc, link->url);
            nya_http_doc_put(&doc, ")");

            if (link->note != nullptr && link->note[0] != '\0') {
                nya_http_doc_put(&doc, ": ");
                nya_http_doc_put(&doc, link->note);
            }

            nya_http_doc_put(&doc, "\n");
        }

        nya_http_doc_put(&doc, "\n");
    }

    return nya_http_doc_finish(&doc, out);
}

b8 _nya_http_llms_field_safe(NYA_ConstCString text) {
    if (text == nullptr) return true;

    for (u64 i = 0; text[i] != '\0'; i++) {
        if ((unsigned char)text[i] < 0x20) return false;
    }

    return true;
}

void _nya_http_llms_title(NYA_HttpDoc* doc, NYA_ConstCString title) {
    for (u64 i = 0; title[i] != '\0'; i++) {
        switch (title[i]) {
            case '\\': nya_http_doc_put(doc, "\\\\"); break;
            case '[':  nya_http_doc_put(doc, "\\["); break;
            case ']':  nya_http_doc_put(doc, "\\]"); break;
            default: {
                char one[2] = { title[i], '\0' };
                nya_http_doc_put(doc, one);
                break;
            }
        }
    }
}
