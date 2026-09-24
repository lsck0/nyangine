/**
 * The text-templating engine: interpolation, dotted paths, array indices, if/else, for, filters, the
 * two escapers and the safety they promise, and the caps that refuse a hostile template whole.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/** Renders and asserts success, returning the result as a NUL terminated C string for easy comparison. */
static NYA_ConstCString render(NYA_Arena* arena, NYA_ConstCString template_text, const NYA_Object* ctx, NYA_TemplateEscape mode) {
    NYA_String* out = nullptr;
    NYA_EXPECT(nya_template_render(arena, template_text, ctx, mode, &out));
    nya_assert(out != nullptr);
    return nya_string_to_cstring(arena, out);
}

/** Whether a render fails, without asserting on the failure. */
static b8 render_fails(NYA_Arena* arena, NYA_ConstCString template_text, const NYA_Object* ctx, NYA_TemplateEscape mode) {
    NYA_String* out    = nullptr;
    NYA_Error   result = nya_template_render(arena, template_text, ctx, mode, &out);
    return !result.ok;
}

static NYA_Value str_value(NYA_ConstCString s) { return (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = (char*)s }; }

int main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_template");

    // TEST: interpolation, dotted paths and array indices
    printf("TEST: interpolation and paths\n");
    {
        NYA_Object* user = nya_object_create(arena);
        nya_object_add(user, "name", str_value("Ada"));
        nya_object_add(user, "age", (NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = 36 });

        NYA_ArrayᐸNYA_Valueᐳ* tags = nya_array_create(arena, NYA_Value);
        nya_array_push_back(tags, str_value("math"));
        nya_array_push_back(tags, str_value("logic"));

        NYA_Object* ctx = nya_object_create(arena);
        nya_object_add(ctx, "user", (NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *user });
        nya_object_add(ctx, "tags", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *tags });

        nya_check(strcmp(render(arena, "Hi {{ user.name }}, {{ user.age }}.", ctx, NYA_TEMPLATE_ESCAPE_NONE), "Hi Ada, 36.") == 0, "dotted paths");
        nya_check(strcmp(render(arena, "{{ tags.0 }}/{{ tags.1 }}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "math/logic") == 0, "array index");

        // Undefined paths render empty rather than crashing.
        nya_check(strcmp(render(arena, "[{{ user.missing }}][{{ nope.deep.path }}][{{ tags.9 }}]", ctx, NYA_TEMPLATE_ESCAPE_NONE), "[][][]") == 0, "undefined is empty");
        printf("  PASSED\n");
    }

    // TEST: if / else over a real object
    printf("TEST: if/else\n");
    {
        NYA_Object* ctx = nya_object_create(arena);
        nya_object_add(ctx, "admin", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });
        nya_object_add(ctx, "guest", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = false });
        nya_object_add(ctx, "title", str_value("root"));

        nya_check(strcmp(render(arena, "{% if admin %}yes{% else %}no{% endif %}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "yes") == 0, "if true");
        nya_check(strcmp(render(arena, "{% if guest %}yes{% else %}no{% endif %}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "no") == 0, "if false");
        nya_check(strcmp(render(arena, "{% if missing %}yes{% else %}no{% endif %}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "no") == 0, "undefined is falsy");
        nya_check(strcmp(render(arena, "{% if title %}has:{{ title }}{% endif %}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "has:root") == 0, "non-empty string is truthy");

        // The untaken branch is not evaluated: an interpolation in it never reaches the output.
        nya_check(strcmp(render(arena, "{% if guest %}{{ title }}{% endif %}done", ctx, NYA_TEMPLATE_ESCAPE_NONE), "done") == 0, "untaken branch emits nothing");
        printf("  PASSED\n");
    }

    // TEST: for, item fields, and the loop cursor
    printf("TEST: for\n");
    {
        NYA_ArrayᐸNYA_Valueᐳ* items = nya_array_create(arena, NYA_Value);
        for (u32 i = 0; i < 3; i++) {
            NYA_Object* row = nya_object_create(arena);
            nya_object_add(row, "name", str_value(i == 0 ? "a" : i == 1 ? "b" : "c"));
            nya_array_push_back(items, ((NYA_Value){ .type = NYA_TYPE_OBJECT, .as_object = *row }));
        }

        NYA_Object* ctx = nya_object_create(arena);
        nya_object_add(ctx, "items", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *items });

        nya_check(strcmp(render(arena, "{% for it in items %}{{ it.name }}{% endfor %}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "abc") == 0, "for over object fields");
        nya_check(strcmp(render(arena, "{% for it in items %}{{ forloop.index }}:{{ it.name }} {% endfor %}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "1:a 2:b 3:c ") == 0, "loop index");
        nya_check(strcmp(render(arena, "{% for it in items %}{% if forloop.first %}[{% endif %}{{ it.name }}{% if forloop.last %}]{% endif %}{% endfor %}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "[abc]") == 0, "first/last");

        // An empty list renders the body zero times.
        NYA_ArrayᐸNYA_Valueᐳ* empty = nya_array_create(arena, NYA_Value);
        NYA_Object*           ectx  = nya_object_create(arena);
        nya_object_add(ectx, "items", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *empty });
        nya_check(strcmp(render(arena, "x{% for it in items %}{{ it.name }}{% endfor %}y", ectx, NYA_TEMPLATE_ESCAPE_NONE), "xy") == 0, "empty loop");
        printf("  PASSED\n");
    }

    // TEST: comments
    printf("TEST: comments\n");
    {
        nya_check(strcmp(render(arena, "a{# hidden {{ x }} #}b", nullptr, NYA_TEMPLATE_ESCAPE_NONE), "ab") == 0, "comment removed");
        printf("  PASSED\n");
    }

    // TEST: filters
    printf("TEST: filters\n");
    {
        NYA_Object* ctx = nya_object_create(arena);
        nya_object_add(ctx, "name", str_value("Ada"));
        nya_object_add(ctx, "empty", str_value(""));
        // 2021-01-01T00:00:00Z is 1609459200 seconds; nanoseconds since the epoch.
        nya_object_add(ctx, "at", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 1609459200LL * 1000000000LL });

        nya_check(strcmp(render(arena, "{{ name | upper }}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "ADA") == 0, "upper");
        nya_check(strcmp(render(arena, "{{ name | lower }}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "ada") == 0, "lower");
        nya_check(strcmp(render(arena, "{{ missing | default:\"none\" }}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "none") == 0, "default on undefined");
        nya_check(strcmp(render(arena, "{{ empty | default:\"none\" }}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "none") == 0, "default on empty");
        nya_check(strcmp(render(arena, "{{ name | default:\"none\" }}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "Ada") == 0, "default keeps present value");
        nya_check(strcmp(render(arena, "{{ at | date:\"date\" }}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "2021-01-01") == 0, "date filter");
        // Pipeable: chained filters apply left to right.
        nya_check(strcmp(render(arena, "{{ missing | default:\"ada\" | upper }}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "ADA") == 0, "chained filters");
        printf("  PASSED\n");
    }

    // TEST: HTML autoescaping — injection is neutralised
    printf("TEST: HTML escaping\n");
    {
        NYA_Object* ctx = nya_object_create(arena);
        nya_object_add(ctx, "bio", str_value("<script>alert('x&y')</script>"));

        NYA_ConstCString out = render(arena, "<p>{{ bio }}</p>", ctx, NYA_TEMPLATE_ESCAPE_HTML);
        nya_check(strcmp(out, "<p>&lt;script&gt;alert(&#39;x&amp;y&#39;)&lt;/script&gt;</p>") == 0, "html value escaped");
        // No raw tag from the value survives.
        nya_check(strstr(out, "<script>") == nullptr, "no raw <script> tag survives");

        // The literal template markup (<p>) is trusted and passes through unescaped.
        nya_check(strstr(out, "<p>") != nullptr, "literal markup preserved");

        // raw opts a value out of escaping.
        nya_check(strcmp(render(arena, "{{ bio | raw }}", ctx, NYA_TEMPLATE_ESCAPE_HTML), "<script>alert('x&y')</script>") == 0, "raw opts out");

        // The explicit escape filter escapes even in a NONE render.
        nya_check(strcmp(render(arena, "{{ bio | escape }}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "&lt;script&gt;alert(&#39;x&amp;y&#39;)&lt;/script&gt;") == 0, "escape filter");
        printf("  PASSED\n");
    }

    // TEST: LaTeX autoescaping — command injection is neutralised
    printf("TEST: LaTeX escaping\n");
    {
        NYA_Object* ctx = nya_object_create(arena);
        nya_object_add(ctx, "v", str_value("100% \\begin{document}$x$ #_{~^}&"));

        NYA_ConstCString out = render(arena, "{{ v }}", ctx, NYA_TEMPLATE_ESCAPE_LATEX);
        nya_check(strcmp(out, "100\\% \\textbackslash{}begin\\{document\\}\\$x\\$ \\#\\_\\{\\textasciitilde{}\\textasciicircum{}\\}\\&") == 0, "latex escaped");
        // A value cannot introduce a live backslash command or an unescaped brace group.
        nya_check(strstr(out, "\\begin{") == nullptr, "no live \\begin command survives");
        printf("  PASSED\n");
    }

    // TEST: malformed templates are refused whole
    printf("TEST: refusals\n");
    {
        nya_check(render_fails(arena, "{% if x %}no end", nullptr, NYA_TEMPLATE_ESCAPE_NONE), "unclosed if");
        nya_check(render_fails(arena, "{% for i in xs %}no end", nullptr, NYA_TEMPLATE_ESCAPE_NONE), "unclosed for");
        nya_check(render_fails(arena, "{{ x ", nullptr, NYA_TEMPLATE_ESCAPE_NONE), "unterminated interpolation");
        nya_check(render_fails(arena, "text {% endif %}", nullptr, NYA_TEMPLATE_ESCAPE_NONE), "stray endif");
        nya_check(render_fails(arena, "{% while x %}{% endwhile %}", nullptr, NYA_TEMPLATE_ESCAPE_NONE), "unknown tag");
        nya_check(render_fails(arena, "{{ x | bogus }}", nullptr, NYA_TEMPLATE_ESCAPE_NONE), "unknown filter");
        printf("  PASSED\n");
    }

    // TEST: deep nesting is refused, not a stack overflow
    printf("TEST: deep nesting refused\n");
    {
        NYA_String* deep = nya_string_create(arena);
        for (u32 i = 0; i < NYA_TEMPLATE_MAX_DEPTH + 8; i++) nya_string_extend(deep, "{% if x %}");
        for (u32 i = 0; i < NYA_TEMPLATE_MAX_DEPTH + 8; i++) nya_string_extend(deep, "{% endif %}");

        NYA_Object* ctx = nya_object_create(arena);
        nya_object_add(ctx, "x", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true });
        nya_check(render_fails(arena, nya_string_to_cstring(arena, deep), ctx, NYA_TEMPLATE_ESCAPE_NONE), "deep nesting refused");
        printf("  PASSED\n");
    }

    // TEST: an explosive loop is bounded by the output cap
    printf("TEST: output cap\n");
    {
        // A big list whose body appends a chunk each iteration would blow past the output cap.
        NYA_ArrayᐸNYA_Valueᐳ* big = nya_array_create(arena, NYA_Value);
        for (u32 i = 0; i < 200000; i++) nya_array_push_back(big, str_value("0123456789abcdef0123456789abcdef"));

        NYA_Object* ctx = nya_object_create(arena);
        nya_object_add(ctx, "big", (NYA_Value){ .type = NYA_TYPE_ARRAY, .as_array = *big });
        nya_check(render_fails(arena, "{% for x in big %}{{ x }}{% endfor %}", ctx, NYA_TEMPLATE_ESCAPE_NONE), "output cap refuses the whole render");
        printf("  PASSED\n");
    }

    printf(nya_check_failures() == 0 ? "ALL PASSED\n" : "FAILURES\n");
    return nya_check_failures() == 0 ? 0 : 1;
}
