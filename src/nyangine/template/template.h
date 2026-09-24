/**
 * @file template.h
 *
 * A small, safe text-templating engine: it renders an authored template against a NYA_Object of data
 * and hands back text. Good for HTML pages, LaTeX documents, emails, config files — anywhere a fixed
 * shape is filled with values that arrive at runtime.
 *
 * It is deliberately not a language. There is no arbitrary expression, no function call, no way for a
 * template — or the data it renders — to reach out of the object it was given. What it does have:
 *
 *   - Interpolation:  `{{ user.name }}`, `{{ items.0.title }}`  — dotted paths and array indices.
 *   - Conditionals:   `{% if user.admin %}...{% else %}...{% endif %}`
 *   - Loops:          `{% for item in items %}{{ item.name }} #{{ forloop.index }}{% endfor %}`
 *   - Comments:       `{# not rendered #}`
 *   - Filters:        `{{ name | upper }}`, `{{ bio | default:"—" }}`, `{{ at | date }}`
 *
 * ```c
 * NYA_Object* ctx = nya_object_create(arena);
 * nya_object_add(ctx, "name", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "<script>" });
 *
 * NYA_String* html = nullptr;
 * NYA_TRY(nya_template_render(arena, "Hi {{ name }}", ctx, NYA_TEMPLATE_ESCAPE_HTML, &html));
 * // html is "Hi &lt;script&gt;" — the value cannot open a tag.
 * ```
 *
 * ## The safety it promises
 *
 * Interpolated values are escaped for the output language by default, so a value can never break out
 * of the text into markup or a command. The literal template text is authored, so it is trusted and
 * emitted verbatim; only the data flowing through `{{ }}` is escaped. A value opts out of escaping
 * only by explicitly asking for `| raw`, which is documented as dangerous and is the one place an
 * author takes responsibility for what a value contains.
 *
 * Output size, loop iterations and block nesting are all capped. A hostile or accidentally huge
 * template is refused whole — it returns an error and no partial string — rather than exhausting
 * memory or the stack. An undefined path is the empty string, never a crash.
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_string.h"

// ───────────────────────────────────── CONSTANTS ─────────────────────────────────────

/** The largest rendered output, in bytes (4 MiB). A template that would exceed it is refused whole. */
#define NYA_TEMPLATE_MAX_OUTPUT 4194304U

/** The deepest `if`/`for` nesting. Also the most loop variables in scope at once. Refused past it. */
#define NYA_TEMPLATE_MAX_DEPTH 64U

/** The total number of loop iterations one render may run, across every loop. Refused past it. */
#define NYA_TEMPLATE_MAX_ITERATIONS 1048576U

/** The longest single path segment (a key or an index) the resolver will look up. */
#define NYA_TEMPLATE_MAX_SEGMENT 128U

// ───────────────────────────────────── TYPES ─────────────────────────────────────

/**
 * Which language interpolated values are escaped for. The choice is the core safety control: it is
 * made once, by the caller who knows what the output is, and every `{{ }}` value is escaped for it.
 * */
typedef enum NYA_TemplateEscape {
    /** No escaping. For plain text or a format with no metacharacters. Values pass through as written. */
    NYA_TEMPLATE_ESCAPE_NONE,

    /** HTML/XML: `& < > " '` become entities, so a value can never open a tag or an attribute. */
    NYA_TEMPLATE_ESCAPE_HTML,

    /** LaTeX: `& % $ # _ { } ~ ^ \` are neutralised, so a value can never become a command or a group. */
    NYA_TEMPLATE_ESCAPE_LATEX,
} NYA_TemplateEscape;

// ───────────────────────────────────── FUNCTIONS ─────────────────────────────────────

/**
 * Renders `template_text` against `context` and writes a freshly allocated result to `*out_string`.
 *
 * `context` is the data the template sees; `nullptr` is an empty context, in which every path is
 * undefined. `mode` picks the escaper for interpolated values. On success `*out_string` holds the
 * rendered text; on any failure — an unbalanced tag, output over the cap, nesting or loops over their
 * caps — it returns the error and leaves `*out_string` untouched, so a partial render is never handed
 * back.
 * */
NYA_API NYA_Error nya_template_render(
    NYA_Arena* arena, NYA_ConstCString template_text, const NYA_Object* context, NYA_TemplateEscape mode, OUT NYA_String** out_string)
    __attr_no_discard;
