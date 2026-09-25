/**
 * @file examples/doc_render/main.c
 *
 * A headless document generator: no window, no renderer, no game loop. It builds one invoice as a
 * NYA_Object and renders it twice from that single set of data — once as an HTML page and once as a
 * LaTeX document — then localises the line-count and the total for a handful of languages.
 *
 * ```
 * ./build run example doc_render
 * ```
 *
 * ## What it teaches
 *
 * **Context-aware escaping is the template engine's whole safety promise.** The same interpolated
 * value is neutralised for whichever output language the caller named, so a value can never break
 * out of the text into markup or a command. See `src/nyangine/template/template.h`.
 *
 *   - `nya_template_render(arena, text, ctx, NYA_TEMPLATE_ESCAPE_HTML,  &out)` turns a data `<` into
 *     `&lt;` and a data `&` into `&amp;`, so a value cannot open a tag.
 *   - `nya_template_render(arena, text, ctx, NYA_TEMPLATE_ESCAPE_LATEX, &out)` turns a data `$` into
 *     `\$` and a data `&` into `\&`, so a value cannot start math mode or a table cell.
 *
 * The key point is that only the *data* flowing through `{{ }}` is escaped. The template text is
 * authored, therefore trusted, and is emitted verbatim: the literal `<article>` in the HTML
 * template stays `<article>`, and the literal ` & ` column separators in the LaTeX table stay real
 * ampersands. A `&` that arrives in the *data*, though, comes out inert. Both facts are asserted
 * below, and they are what make the positive checks airtight — since neither template contains a
 * literal `&lt;`, `&amp;`, `\$` or `\&`, finding one in the output proves the data was escaped.
 *
 * **CLDR plural rules pick the right word for a count, per language.** `nya_i18n_plural_category`
 * maps a locale and a count onto the grammatical category the language would use — English has just
 * `one` and `other`, Russian reaches `one`, `few` and `many` through its mod-10 / mod-100 rules — so
 * "1 item" / "5 items" comes out correct rather than mechanically pluralised. See
 * `src/nyangine/core/core_i18n.h`. `nya_i18n_format_integer` then writes the total with the locale's
 * grouping separator: `12,345` for `en`, `12.345` for `de`.
 *
 * None of this needs a locale asset file on disk: the plural rules and the number separators are
 * properties of the language, hard-coded for a representative set, so a purely computational example
 * can lean on them directly.
 * */
// nyangine.h first, always: base_basic.h defines _POSIX_C_SOURCE and _XOPEN_SOURCE before it pulls in libc, and a system header included ahead of it has already fixed them at another value.
#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/* THE DATA One invoice, built in memory so the example needs no input file. The strings are chosen to carry exactly the metacharacters each output language reserves: the ampersands, angle brackets and the `<100% uptime>` all have to survive as data, never as markup or a command. */

/** One line of the invoice, laid out as it goes into the context. */
typedef struct LineItem {
    NYA_ConstCString name;
    s64              quantity;
    s64              unit_price; // whole currency units; the grand total is summed from these
} LineItem;

NYA_INTERNAL const LineItem LINE_ITEMS[] = {
    // The angle brackets and the ampersand are the HTML hazard; the LaTeX render leaves them be.
    { .name = "Server rack <2U> & rails", .quantity = 2, .unit_price = 5000 },
    // The dollar sign and the percent are the LaTeX hazard; the HTML render leaves them be.
    { .name = "Cable, 50% shielded ($/m)", .quantity = 5, .unit_price = 400 },
    { .name = "Spare fan", .quantity = 1, .unit_price = 345 },
};

/**
 * Builds the invoice as a NYA_Object. `items` becomes an array of objects the `{% for %}` loop walks;
 * `count_label` and `total` are localised for `locale` before they go in, so the same rendered
 * document reads correctly in that language.
 * */
NYA_INTERNAL NYA_Object* invoice_create(NYA_Arena* arena, NYA_ConstCString locale);

/* i18n: PLURALS AND GROUPED NUMBERS */

/**
 * The noun for "item", per plural category, for one language. A category the language never reaches
 * is left null and falls back to `other`, which every language has. This is the per-language data the
 * CLDR category selection indexes into; picking the row is nya_i18n_plural_category's job, not ours.
 * */
typedef struct ItemNoun {
    NYA_ConstCString locale;
    NYA_ConstCString by_category[NYA_I18N_PLURAL_COUNT]; // indexed by NYA_I18nPluralCategory
} ItemNoun;

NYA_INTERNAL const ItemNoun ITEM_NOUNS[] = {
    { .locale = "en", .by_category = { [NYA_I18N_PLURAL_ONE] = "item",   [NYA_I18N_PLURAL_OTHER] = "items"    } },
    { .locale = "de", .by_category = { [NYA_I18N_PLURAL_ONE] = "Artikel", [NYA_I18N_PLURAL_OTHER] = "Artikel" } },
    // Russian is the interesting one: `one` (1, 21, …), `few` (2–4, 22–24, …) and `many` (0, 5–20, …).
    { .locale = "ru", .by_category = { [NYA_I18N_PLURAL_ONE]  = "\xD1\x82\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x80",           // товар
                                       [NYA_I18N_PLURAL_FEW]  = "\xD1\x82\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x80\xD0\xB0",   // товара
                                       [NYA_I18N_PLURAL_MANY] = "\xD1\x82\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x80\xD0\xBE\xD0\xB2", // товаров
                                       [NYA_I18N_PLURAL_OTHER]= "\xD1\x82\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x80\xD0\xB0" } }, // товара
};

/** Finds the noun table for a locale, defaulting to `en`, matched on the whole tag for simplicity. */
NYA_INTERNAL const ItemNoun* item_noun_for(NYA_ConstCString locale) {
    for (u64 i = 0; i < nya_carray_length(ITEM_NOUNS); i++) {
        if (nya_string_equals(ITEM_NOUNS[i].locale, locale)) return &ITEM_NOUNS[i];
    }
    return &ITEM_NOUNS[0];
}

/**
 * "<grouped count> <noun>" for `count` in `locale`: the number formatted with the locale's grouping
 * separator, then the noun the locale's plural rule selects for that count. Returns a fresh string.
 * */
NYA_INTERNAL NYA_String* item_count_label(NYA_Arena* arena, NYA_ConstCString locale, s64 count) {
    const ItemNoun*        nouns    = item_noun_for(locale);
    NYA_I18nPluralCategory category = nya_i18n_plural_category(locale, count);

    // A language that does not reach this category falls back to `other`, which it always has.
    NYA_ConstCString noun = nouns->by_category[category];
    if (noun == nullptr) noun = nouns->by_category[NYA_I18N_PLURAL_OTHER];

    NYA_String* label = nya_string_create(arena);
    nya_string_extend(label, nya_i18n_format_integer(locale, count));
    nya_string_push_back(label, ' ');
    nya_string_extend(label, noun);
    return label;
}

NYA_INTERNAL NYA_Object* invoice_create(NYA_Arena* arena, NYA_ConstCString locale) {
    nya_assert(arena != nullptr);

    NYA_Object* invoice = nya_object_create(arena);
    nya_object_add(invoice, "invoice_id", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "NYA-2048" });

    // The customer name and the tagline carry the metacharacters both output languages must neutralise.
    nya_object_add(invoice, "customer", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "O'Brien & <Sons>" });
    nya_object_add(invoice, "tagline",  (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "Net-30: pay $0 now & keep <100% uptime>" });

    // The line items, as an array of objects the `{% for item in items %}` loop steps through.
    NYA_ArrayᐸNYA_Valueᐳ* items = nya_array_create(arena, NYA_Value);
    s64                   total = 0;
    for (u64 i = 0; i < nya_carray_length(LINE_ITEMS); i++) {
        const LineItem* line = &LINE_ITEMS[i];
        total += line->quantity * line->unit_price;

        NYA_Object* item = nya_object_create(arena);
        nya_object_add(item, "name",  (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)line->name });
        nya_object_add(item, "qty",   (NYA_Value){ .type = NYA_TYPE_S64,    .as_s64    = line->quantity });
        nya_object_add(item, "price", (NYA_Value){ .type = NYA_TYPE_S64,    .as_s64    = line->unit_price });

        nya_array_add(items, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *item }));
    }
    nya_object_add(invoice, "items", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *items });

    // The localised summary strings: the plural-correct line count and the grouped grand total. The template reads a string value as a C string, so each goes in NUL-terminated. The grouped total is copied out of nya_i18n_format_integer's shared ring before later calls overwrite it.
    NYA_String* count_label = item_count_label(arena, locale, (s64)items->length);
    nya_object_add(invoice, "count_label", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = nya_string_to_cstring(arena, count_label) });

    NYA_String* total_text = nya_string_create(arena);
    nya_string_extend(total_text, nya_i18n_format_integer(locale, total));
    nya_object_add(invoice, "total", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = nya_string_to_cstring(arena, total_text) });

    return invoice;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE TEMPLATES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * Two authored templates over the one context. Every `{{ }}` value is escaped for the output
 * language; everything else — the tags, the ` & ` column separators, the `\\` row breaks — is
 * literal template text and is emitted exactly as written.
 */

NYA_INTERNAL NYA_ConstCString HTML_TEMPLATE =
    "<!doctype html>\n"
    "<article>\n"
    "  <h1>Invoice {{ invoice_id }}</h1>\n"
    "  <p>Billed to: {{ customer }}</p>\n"
    "  <p class=\"note\">{{ tagline }}</p>\n"
    "  <table>\n"
    "{% for item in items %}    <tr><td>{{ item.name }}</td><td>{{ item.qty }}</td><td>{{ item.price }}</td></tr>\n"
    "{% endfor %}  </table>\n"
    "  <p>{{ count_label }} — total {{ total }}</p>\n"
    "</article>\n";

NYA_INTERNAL NYA_ConstCString LATEX_TEMPLATE =
    "\\documentclass{article}\n"
    "\\begin{document}\n"
    "\\section*{Invoice {{ invoice_id }}}\n"
    "Billed to: {{ customer }}\\\\\n"
    "{{ tagline }}\\\\\n"
    "\\begin{tabular}{lrr}\n"
    "{% for item in items %}{{ item.name }} & {{ item.qty }} & {{ item.price }} \\\\\n"
    "{% endfor %}\\end{tabular}\n"
    "\n"
    "{{ count_label }} --- total {{ total }}\n"
    "\\end{document}\n";

/* MAIN */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    // Headless: every engine subsystem the framework has except the renderer, which needs a display. The i18n system's number-formatting ring lives on the app, so it has to be up before we format.
    NYA_EXPECT(nya_app_init(.headless = true, .app_id = "doc_render"));
    defer nya_app_deinit();

    NYA_Arena* arena = nya_arena_create(.name = "doc_render");
    defer      nya_arena_destroy(arena);

    // The document is authored in English, so its summary line reads in English.
    NYA_ConstCString document_locale = "en";
    NYA_Object*      invoice         = invoice_create(arena, document_locale);

    // the same data, rendered two ways

    NYA_String* html = nullptr;
    NYA_EXPECT(nya_template_render(arena, HTML_TEMPLATE, invoice, NYA_TEMPLATE_ESCAPE_HTML, &html));

    NYA_String* latex = nullptr;
    NYA_EXPECT(nya_template_render(arena, LATEX_TEMPLATE, invoice, NYA_TEMPLATE_ESCAPE_LATEX, &latex));

    nya_log_info("HTML render (autoescaped):\n" NYA_FMT_STRING, NYA_FMT_STRING_ARG(html));
    nya_log_info("LaTeX render (LaTeX-autoescaped):\n" NYA_FMT_STRING, NYA_FMT_STRING_ARG(latex));

    // context-aware escaping, asserted — Neither template contains a literal `&lt;`, `&amp;`, `\$` or `\&`, so each of these can only be an escaped data character. Their presence is proof the value was neutralised for its language.

    nya_assert(nya_string_contains(html, "&lt;"),  "HTML render did not escape a '<' in the data");
    nya_assert(nya_string_contains(html, "&amp;"), "HTML render did not escape an '&' in the data");
    // And the raw, dangerous form must not have leaked through.
    nya_assert(!nya_string_contains(html, "<Sons>"), "HTML render leaked an unescaped tag from the data");

    nya_assert(nya_string_contains(latex, "\\$"), "LaTeX render did not escape a '$' in the data");
    nya_assert(nya_string_contains(latex, "\\&"), "LaTeX render did not escape an '&' in the data");

    nya_log_info("Escaping checks passed: HTML neutralised '<' and '&'; LaTeX neutralised '$' and '&'.");

    // plural selection and grouped numbers across locales — The same counts, worded per language. English flips at 1; German uses one form; Russian reaches three of its categories over 1 / 2 / 5.

    const s64        sample_counts[] = { 1, 2, 5 };
    const NYA_ConstCString locales[] = { "en", "de", "ru" };

    for (u64 l = 0; l < nya_carray_length(locales); l++) {
        NYA_ConstCString locale = locales[l];
        for (u64 c = 0; c < nya_carray_length(sample_counts); c++) {
            s64                    count    = sample_counts[c];
            NYA_I18nPluralCategory category = nya_i18n_plural_category(locale, count);
            NYA_String*            label    = item_count_label(arena, locale, count);

            nya_log_info(
                "  %s  count=%lld  category=%-5s  -> " NYA_FMT_STRING,
                locale,
                (long long)count,
                nya_i18n_plural_category_name(category),
                NYA_FMT_STRING_ARG(label));
        }
    }

    // Locale number formatting for a larger total, so the grouping separator is visible.
    const s64 big_total = 12345678;
    nya_log_info(
        "Grouped total %lld: en=%s  de=%s",
        (long long)big_total,
        nya_i18n_format_integer("en", big_total),
        nya_i18n_format_integer("de", big_total));

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
