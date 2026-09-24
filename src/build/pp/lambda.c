#include "nyangine/nyangine.h"

#include "build/build.h"

/* TYPES */

/** One source file that wrote at least one lambda, and where its companion goes. */
typedef struct {
    /** As the walk handed it over: "./src/gnyame/layers/layer_pause_menu.c". */
    char path[NYA_LAMBDA_MAX_PATH];

    /** "./src/genyarated/lambdas/gnyame_layers_layer_pause_menu_c.h". */
    char companion[NYA_LAMBDA_MAX_PATH];

    /** What the source has to spell to include it. */
    char include[NYA_LAMBDA_MAX_PATH];
} _NYA_LambdaSource;

/** One call site, read but not judged: everything but the tag is handed to the compiler as written. */
typedef struct {
    char tag[NYA_LAMBDA_MAX_NAME];
    char return_type[NYA_LAMBDA_MAX_NAME];

    /** The text between the parentheses of the parameter list, verbatim. */
    char parameters[NYA_LAMBDA_MAX_PARAMETERS];

    /** The braces and everything between them, verbatim, indentation and all. See the emitter. */
    char body[NYA_LAMBDA_MAX_BODY];

    /** Index into the set's sources. */
    u32 source;

    /** Where `nya_lambda` was written, and where the body's opening brace is. Both 1-based. */
    u32 line;
    u32 body_line;
} _NYA_Lambda;

typedef struct {
    _NYA_Lambda       lambdas[NYA_LAMBDA_MAX_LAMBDAS];
    u32               count;
    _NYA_LambdaSource sources[NYA_LAMBDA_MAX_SOURCES];
    u32               source_count;

    /** Everything wrong with the tree, counted rather than thrown, so one run reports all of it. */
    u32 problems;
} _NYA_LambdaSet;

/* CONSTANTS */

NYA_INTERNAL const NYA_ConstCString _NYA_LAMBDA_TREES[] = {
    NYA_LAMBDA_TREE_ENGINE,
    NYA_LAMBDA_TREE_GAME,
    NYA_LAMBDA_TREE_EXAMPLES,
    NYA_LAMBDA_TREE_TESTS,
};

/* PRIVATE API DECLARATION */

NYA_INTERNAL b8  _nya_lambda_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL s32 _nya_lambda_compare(const NYA_String* a, const NYA_String* b);

/** Reports one problem against the line somebody wrote, and keeps going so the next one is found too. */
NYA_INTERNAL void _nya_lambda_problem(_NYA_LambdaSet* set, NYA_ConstCString path, u32 line, NYA_ConstCString format, ...) __attr_fmt_printf(4, 5);

/**
 * The offset just past the lexical unit starting at `from`: a comment, a string, a character literal,
 * or one plain character. Adds the newlines it crossed to `out_line`.
 *
 * Everything that walks the text goes through this, which is why a `nya_lambda` inside a comment or a
 * string is not a call site and a brace inside either does not close a body.
 * */
NYA_INTERNAL u64 _nya_lambda_step(NYA_ConstCString text, u64 length, u64 from, OUT u32* out_line) __attr_no_discard;

/** The offset just past the `close` matching the `open` at `from`, or zero when nothing closes it. */
NYA_INTERNAL u64 _nya_lambda_match(NYA_ConstCString text, u64 length, u64 from, char open, char close, OUT u32* out_line) __attr_no_discard;

/** The next comma at depth zero in `[from, to)`, or `to`. */
NYA_INTERNAL u64 _nya_lambda_comma(NYA_ConstCString text, u64 from, u64 to) __attr_no_discard;

/** First offset in `[from, to)` that is not whitespace or a comment, or `to`. */
NYA_INTERNAL u64 _nya_lambda_skip_space(NYA_ConstCString text, u64 from, u64 to, OUT u32* out_line) __attr_no_discard;

/** Copies `[from, to)` with its ends trimmed. False when it does not fit, which is a problem to report. */
NYA_INTERNAL b8 _nya_lambda_copy(NYA_ConstCString text, u64 from, u64 to, OUT char* out, u64 capacity) __attr_no_discard;

/** Whether the text is a plain identifier, which is all a tag may be: it is pasted onto a function name. */
NYA_INTERNAL b8 _nya_lambda_is_identifier(NYA_ConstCString text) __attr_no_discard;

/** "./src/gnyame/layers/layer_pause_menu.c" becomes "gnyame_layers_layer_pause_menu_c". */
NYA_INTERNAL void _nya_lambda_mangle(NYA_ConstCString path, OUT char* out, u64 capacity);

/** Everything after the last separator, which is how two paths for one file are compared here. */
NYA_INTERNAL NYA_ConstCString _nya_lambda_basename(NYA_ConstCString path) __attr_no_discard;

/** The set's entry for `path`, appending one the first time the file is seen. */
NYA_INTERNAL u32 _nya_lambda_source(_NYA_LambdaSet* set, NYA_ConstCString path) __attr_no_discard;

/** Everything the scanner does with one file: find every call site in it and read each one. */
NYA_INTERNAL void _nya_lambda_scan_file(_NYA_LambdaSet* set, NYA_Arena* arena, NYA_ConstCString path);

/** One call site, from the `(` after the marker. Returns the offset just past its `)`. */
NYA_INTERNAL u64 _nya_lambda_parse(_NYA_LambdaSet* set, NYA_ConstCString path, NYA_ConstCString text, u64 length, u64 open, u32 line);

/** One source file's companion header. */
NYA_INTERNAL void _nya_lambda_emit_companion(const _NYA_LambdaSet* set, u32 source, NYA_String* out);

/** Deletes whatever is in the output directory that this run did not write. */
NYA_INTERNAL void _nya_lambda_prune(const _NYA_LambdaSet* set, NYA_Arena* arena);

/* PUBLIC API IMPLEMENTATION */

void nya_lambda_generate(void) {
    NYA_ConstCString inputs[] = {
        NYA_LAMBDA_TREE_ENGINE, NYA_LAMBDA_TREE_GAME, NYA_LAMBDA_TREE_EXAMPLES, NYA_LAMBDA_TREE_TESTS, "./src/build/pp/lambda.c", nullptr,
    };
    NYA_ConstCString outputs[] = { NYA_LAMBDA_OUTPUT_MANIFEST, nullptr };
    if (nya_pp_is_current("generate_lambdas", inputs, outputs)) return;

    NYA_Arena* arena = nya_arena_create(.name = "lambda_generate");
    defer      nya_arena_destroy(arena);

    _NYA_LambdaSet* set = nya_arena_alloc(arena, sizeof(_NYA_LambdaSet));
    nya_memset(set, 0, sizeof(*set));

    for (u32 tree = 0; tree < nya_carray_length(_NYA_LAMBDA_TREES); tree++) {
        NYA_ArrayᐸNYA_Stringᐳ* files = nya_array_create(arena, NYA_String);
        NYA_EXPECT(nya_filesystem_walk(arena, _NYA_LAMBDA_TREES[tree], _nya_lambda_collect, files), "while listing %s", _NYA_LAMBDA_TREES[tree]);

        // Sorted, so the manifest and every companion are a function of the tree rather than of the order the filesystem happened to hand it over.
        nya_array_sort(files, _nya_lambda_compare);

        nya_array_foreach (files, file) _nya_lambda_scan_file(set, arena, nya_string_to_cstring(arena, file));
    }

    // Every source that wrote a lambda has to include what was written for it, and it is cheaper to say so here, with the line to add, than to let the compiler report an undeclared function.
    for (u32 i = 0; i < set->source_count; i++) {
        NYA_String* text = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(set->sources[i].path, text), "while reading %s", set->sources[i].path);

        if (nya_string_contains(nya_string_to_cstring(arena, text), set->sources[i].include)) continue;

        _nya_lambda_problem(set, set->sources[i].path, 1, "writes a nya_lambda but does not include what it generates; add #include \"%s\"",
                            set->sources[i].include);
    }

    /* Nothing has been written yet, and nothing is written now: a body that does not parse would otherwise leave one companion rewritten and the next one as it was. */
    if (set->problems > 0) nya_log_panic("nya_lambda_generate: " FMTu32 " problem(s); nothing was written.", set->problems);

    NYA_EXPECT(nya_filesystem_create_directory(NYA_LAMBDA_OUTPUT_DIRECTORY), "while creating the lambda directory");

    for (u32 i = 0; i < set->source_count; i++) {
        NYA_String* companion = nya_string_create(arena);
        _nya_lambda_emit_companion(set, i, companion);

        NYA_EXPECT(nya_file_write(set->sources[i].companion, companion), "while writing %s", set->sources[i].companion);
    }

    _nya_lambda_prune(set, arena);

    // Last, so its timestamp says the whole run finished. See NYA_LAMBDA_OUTPUT_MANIFEST.
    NYA_String* manifest = nya_string_create(arena);
    nya_string_extend(manifest,
                      "# GENERATED by src/build/pp/lambda.c. DO NYAT EDIT.\n"
                      "#\n"
                      "# Every nya_lambda in the tree and where it was written. A tag is one name across all of it,\n"
                      "# so this is the whole namespace, and the file the pass compares its own age against.\n");

    for (u32 i = 0; i < set->count; i++) {
        const _NYA_Lambda* lambda = &set->lambdas[i];
        nya_string_extend_sprintf(manifest, "%s\t%s:" FMTu32 "\n", lambda->tag, set->sources[lambda->source].path, lambda->line);
    }

    NYA_EXPECT(nya_file_write(NYA_LAMBDA_OUTPUT_MANIFEST, manifest), "while writing the lambda manifest");

    nya_log_info("nya_lambda_generate: " FMTu32 " lambdas from " FMTu32 " files into %s.", set->count, set->source_count,
                 NYA_LAMBDA_OUTPUT_DIRECTORY);
}

/* PRIVATE API IMPLEMENTATION */

b8 _nya_lambda_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* files = user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;
    if (!nya_string_ends_with(entry->name, ".c") && !nya_string_ends_with(entry->name, ".h")) return true;

    NYA_String* full = nya_string_from(files->arena, path);
    nya_array_push_back(files, *full);

    return true;
}

s32 _nya_lambda_compare(const NYA_String* a, const NYA_String* b) {
    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);

    if (difference != 0) return difference < 0 ? -1 : 1;
    if (a->length == b->length) return 0;

    return a->length < b->length ? -1 : 1;
}

void _nya_lambda_problem(_NYA_LambdaSet* set, NYA_ConstCString path, u32 line, NYA_ConstCString format, ...) {
    nya_assert(set != nullptr && path != nullptr && format != nullptr);

    char detail[512];

    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(detail, sizeof(detail), format, arguments);
    va_end(arguments);

    // The form an editor and a terminal both make clickable, as the lint rules report in.
    nya_log_error("%s:" FMTu32 ": %s", path, line, detail);

    set->problems++;
}

u64 _nya_lambda_step(NYA_ConstCString text, u64 length, u64 from, OUT u32* out_line) {
    nya_assert(text != nullptr && out_line != nullptr);
    nya_assert(from < length);

    if (text[from] == '\n') {
        *out_line += 1;
        return from + 1;
    }

    if (text[from] == '/' && from + 1 < length && text[from + 1] == '/') {
        u64 at = from + 2;
        while (at < length && text[at] != '\n') at++;
        return at;
    }

    if (text[from] == '/' && from + 1 < length && text[from + 1] == '*') {
        u64 at = from + 2;
        while (at + 1 < length && !(text[at] == '*' && text[at + 1] == '/')) {
            if (text[at] == '\n') *out_line += 1;
            at++;
        }
        return at + 1 < length ? at + 2 : length;
    }

    if (text[from] == '"' || text[from] == '\'') {
        char quote = text[from];
        u64  at    = from + 1;

        while (at < length && text[at] != quote) {
            if (text[at] == '\n') *out_line += 1;
            // A literal may not span lines unescaped, but an escaped quote or backslash must not end it.
            at += text[at] == '\\' && at + 1 < length ? 2 : 1;
        }

        return at < length ? at + 1 : length;
    }

    return from + 1;
}

u64 _nya_lambda_match(NYA_ConstCString text, u64 length, u64 from, char open, char close, OUT u32* out_line) {
    nya_assert(text != nullptr && out_line != nullptr);
    nya_assert(from < length && text[from] == open);

    u32 depth = 0;
    u64 at    = from;

    while (at < length) {
        if (text[at] == open) {
            depth++;
        } else if (text[at] == close) {
            depth--;
            if (depth == 0) return at + 1;
        }

        at = _nya_lambda_step(text, length, at, out_line);
    }

    return 0;
}

u64 _nya_lambda_comma(NYA_ConstCString text, u64 from, u64 to) {
    nya_assert(text != nullptr);

    u32 depth   = 0;
    u32 ignored = 0;
    u64 at      = from;

    while (at < to) {
        char current = text[at];

        if (current == '(' || current == '[' || current == '{') depth++;
        if (current == ')' || current == ']' || current == '}') depth--;
        if (current == ',' && depth == 0) return at;

        at = _nya_lambda_step(text, to, at, &ignored);
    }

    return to;
}

u64 _nya_lambda_skip_space(NYA_ConstCString text, u64 from, u64 to, OUT u32* out_line) {
    nya_assert(text != nullptr && out_line != nullptr);

    u64 at = from;

    while (at < to) {
        b8 comment = text[at] == '/' && at + 1 < to && (text[at + 1] == '/' || text[at + 1] == '*');
        if (!isspace((unsigned char)text[at]) && !comment) return at;

        at = _nya_lambda_step(text, to, at, out_line);
    }

    return to;
}

b8 _nya_lambda_copy(NYA_ConstCString text, u64 from, u64 to, OUT char* out, u64 capacity) {
    nya_assert(text != nullptr && out != nullptr && capacity > 1);

    while (from < to && isspace((unsigned char)text[from])) from++;
    while (to > from && isspace((unsigned char)text[to - 1])) to--;

    u64 length = to - from;
    if (length >= capacity) return false;

    nya_memcpy(out, text + from, length);
    out[length] = '\0';

    return true;
}

b8 _nya_lambda_is_identifier(NYA_ConstCString text) {
    nya_assert(text != nullptr);

    if (text[0] == '\0' || isdigit((unsigned char)text[0])) return false;

    for (u64 i = 0; text[i] != '\0'; i++) {
        if (!isalnum((unsigned char)text[i]) && text[i] != '_') return false;
    }

    return true;
}

void _nya_lambda_mangle(NYA_ConstCString path, OUT char* out, u64 capacity) {
    nya_assert(path != nullptr && out != nullptr && capacity > 1);

    NYA_ConstCString rest = path;
    if (strncmp(rest, "./", 2) == 0) rest += 2;

    // The engine and the game are the common case and nothing else in the tree is called `src`, so dropping it keeps the names readable without making two of them collide.
    if (strncmp(rest, "src/", 4) == 0) rest += 4;

    u64 used = 0;
    for (u64 i = 0; rest[i] != '\0' && used + 1 < capacity; i++) {
        b8 plain  = isalnum((unsigned char)rest[i]) != 0;
        out[used] = plain ? rest[i] : '_';
        used++;
    }

    out[used] = '\0';
}

NYA_ConstCString _nya_lambda_basename(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    const char* separator = strrchr(path, '/');
    return separator != nullptr ? separator + 1 : path;
}

u32 _nya_lambda_source(_NYA_LambdaSet* set, NYA_ConstCString path) {
    nya_assert(set != nullptr && path != nullptr);

    for (u32 i = 0; i < set->source_count; i++) {
        if (nya_string_equals(set->sources[i].path, path)) return i;
    }

    nya_assert(set->source_count < NYA_LAMBDA_MAX_SOURCES, "more than %d files write lambdas; raise NYA_LAMBDA_MAX_SOURCES",
               NYA_LAMBDA_MAX_SOURCES);

    char mangled[NYA_LAMBDA_MAX_PATH];
    _nya_lambda_mangle(path, mangled, sizeof(mangled));

    _NYA_LambdaSource* source = &set->sources[set->source_count];

    (void)snprintf(source->path, sizeof(source->path), "%s", path);
    (void)snprintf(source->companion, sizeof(source->companion), "%s/%s.h", NYA_LAMBDA_OUTPUT_DIRECTORY, mangled);
    (void)snprintf(source->include, sizeof(source->include), "%s%s.h", NYA_LAMBDA_INCLUDE_PREFIX, mangled);

    set->source_count++;

    return set->source_count - 1;
}

void _nya_lambda_scan_file(_NYA_LambdaSet* set, NYA_Arena* arena, NYA_ConstCString path) {
    nya_assert(set != nullptr && arena != nullptr && path != nullptr);

    NYA_String* text = nya_string_create(arena);
    NYA_EXPECT(nya_file_read(path, text), "while reading %s", path);

    // Read whole and searched once: almost nothing in the tree writes a lambda, and walking every file character by character to find out would be most of this pass's time.
    NYA_CString contents = nya_string_to_cstring(arena, text);
    if (strstr(contents, NYA_LAMBDA_MARKER) == nullptr) return;

    u64 length = text->length;
    u64 at     = 0;
    u32 line   = 1;
    b8  fresh  = true;

    while (at < length) {
        /* A preprocessor line is skipped whole, continuations included: base_lambda.h's own `#define` names the marker, and so would anything that wrapped it. */
        if (fresh && contents[at] == '#') {
            while (at < length && contents[at] != '\n') {
                if (contents[at] == '\\' && at + 1 < length && contents[at + 1] == '\n') {
                    line++;
                    at++;
                }
                at++;
            }
            continue;
        }

        if (!isspace((unsigned char)contents[at])) fresh = false;
        if (contents[at] == '\n') fresh = true;

        b8 boundary = at == 0 || (!isalnum((unsigned char)contents[at - 1]) && contents[at - 1] != '_');
        if (!boundary || strncmp(&contents[at], NYA_LAMBDA_MARKER, strlen(NYA_LAMBDA_MARKER)) != 0) {
            at = _nya_lambda_step(contents, length, at, &line);
            continue;
        }

        u32 scratch = line;
        u64 open    = _nya_lambda_skip_space(contents, at + strlen(NYA_LAMBDA_MARKER), length, &scratch);

        if (open >= length || contents[open] != '(') {
            at = _nya_lambda_step(contents, length, at, &line);
            continue;
        }

        u64 marker = at;

        at    = _nya_lambda_parse(set, path, contents, length, open, line);
        fresh = false;

        // The line count is rebuilt over the call site rather than threaded out of the parser, which reads the same span several times and would count some of it twice.
        for (u64 i = marker; i < at; i++) {
            if (contents[i] == '\n') line++;
        }
    }
}

u64 _nya_lambda_parse(_NYA_LambdaSet* set, NYA_ConstCString path, NYA_ConstCString text, u64 length, u64 open, u32 line) {
    nya_assert(set != nullptr && path != nullptr && text != nullptr);

    u32 ignored = 0;
    u64 end     = _nya_lambda_match(text, length, open, '(', ')', &ignored);

    if (end == 0) {
        _nya_lambda_problem(set, path, line, "nya_lambda( is never closed");
        return length;
    }

    // Everything between the parentheses, which is what the macro is handed and drops.
    u64 from = open + 1;
    u64 to   = end - 1;

    _NYA_Lambda lambda = { .line = line };

    // ── the tag ─────────────────────────────────────────────────────────────────────────────────
    u64 comma = _nya_lambda_comma(text, from, to);
    if (comma == to) {
        _nya_lambda_problem(set, path, line, "nya_lambda takes a tag, a return type, a parameter list and a body");
        return end;
    }

    if (!_nya_lambda_copy(text, from, comma, lambda.tag, sizeof(lambda.tag)) || !_nya_lambda_is_identifier(lambda.tag)) {
        _nya_lambda_problem(set, path, line, "the tag must be a plain name, because the function is called %s<tag>", NYA_LAMBDA_PREFIX);
        return end;
    }

    // ── the return type ─────────────────────────────────────────────────────────────────────────
    from  = comma + 1;
    comma = _nya_lambda_comma(text, from, to);

    if (comma == to || !_nya_lambda_copy(text, from, comma, lambda.return_type, sizeof(lambda.return_type)) || lambda.return_type[0] == '\0') {
        _nya_lambda_problem(set, path, line, "'%s' has no return type; write `void` when it returns nothing", lambda.tag);
        return end;
    }

    // ── the parameter list, parenthesised so its commas are its own ─────────────────────────────
    from = _nya_lambda_skip_space(text, comma + 1, to, &ignored);

    if (from >= to || text[from] != '(') {
        _nya_lambda_problem(set, path, line, "'%s' needs its parameters in parentheses, as in (void* data)", lambda.tag);
        return end;
    }

    u64 parameters_end = _nya_lambda_match(text, to, from, '(', ')', &ignored);

    if (parameters_end == 0 || !_nya_lambda_copy(text, from + 1, parameters_end - 1, lambda.parameters, sizeof(lambda.parameters))) {
        _nya_lambda_problem(set, path, line, "'%s' has a parameter list this pass cannot read, or one longer than %d characters", lambda.tag,
                            NYA_LAMBDA_MAX_PARAMETERS);
        return end;
    }

    // ── the body, taken with its braces and not looked into ─────────────────────────────────────
    from = _nya_lambda_skip_space(text, parameters_end, to, &ignored);

    if (from >= to || text[from] != ',') {
        _nya_lambda_problem(set, path, line, "'%s' is missing the comma between its parameters and its body", lambda.tag);
        return end;
    }

    u32 body_line = line;
    for (u64 i = open; i < from; i++) {
        if (text[i] == '\n') body_line++;
    }

    from = _nya_lambda_skip_space(text, from + 1, to, &body_line);

    if (from >= to || text[from] != '{') {
        _nya_lambda_problem(set, path, line, "'%s' needs a braced body, as in { nya_log_info(\"hello\"); }", lambda.tag);
        return end;
    }

    u64 body_end = _nya_lambda_match(text, to, from, '{', '}', &ignored);

    if (body_end == 0 || !_nya_lambda_copy(text, from, body_end, lambda.body, sizeof(lambda.body))) {
        _nya_lambda_problem(set, path, line, "'%s' has an unclosed body, or one longer than %d characters; call a function instead", lambda.tag,
                            NYA_LAMBDA_MAX_BODY);
        return end;
    }

    if (_nya_lambda_skip_space(text, body_end, to, &ignored) != to) {
        _nya_lambda_problem(set, path, line, "'%s' has something after its body; the body is the last argument", lambda.tag);
        return end;
    }

    lambda.body_line = body_line;

    // ── the tag is the name of a function, and there is one namespace of those ──────────────────
    for (u32 i = 0; i < set->count; i++) {
        if (!nya_string_equals(set->lambdas[i].tag, lambda.tag)) continue;

        _nya_lambda_problem(set, path, line, "'%s' is already the tag at %s:" FMTu32 "; rename one of them", lambda.tag,
                            set->sources[set->lambdas[i].source].path, set->lambdas[i].line);
        return end;
    }

    if (set->count >= NYA_LAMBDA_MAX_LAMBDAS) {
        _nya_lambda_problem(set, path, line, "more than %d lambdas in the tree; raise NYA_LAMBDA_MAX_LAMBDAS", NYA_LAMBDA_MAX_LAMBDAS);
        return end;
    }

    lambda.source = _nya_lambda_source(set, path);

    set->lambdas[set->count] = lambda;
    set->count++;

    return end;
}

void _nya_lambda_emit_companion(const _NYA_LambdaSet* set, u32 source, NYA_String* out) {
    nya_assert(set != nullptr && out != nullptr);
    nya_assert(source < set->source_count);

    nya_string_extend_sprintf(out,
                              "/*\n"
                              " * GENERATED by src/build/pp/lambda.c from the nya_lambda bodies in %s. DO NYAT EDIT.\n"
                              " *\n"
                              " * Included by that file and by nothing else: every function here is static, and each body was\n"
                              " * written against whatever is in scope where the include sits. The bodies are copied through\n"
                              " * character for character, so the #line above each one makes a diagnostic name the line and the\n"
                              " * column somebody actually typed.\n"
                              " */\n"
                              "#pragma once\n",
                              set->sources[source].path);

    for (u32 i = 0; i < set->count; i++) {
        const _NYA_Lambda* lambda = &set->lambdas[i];
        if (lambda->source != source) continue;

        nya_string_extend_sprintf(out, "\n#line " FMTu32 " \"%s\"\n", lambda->body_line, set->sources[source].path);
        nya_string_extend_sprintf(out, "NYA_INTERNAL %s %s%s(%s) %s\n", lambda->return_type, NYA_LAMBDA_PREFIX, lambda->tag, lambda->parameters,
                                  lambda->body);
    }
}

void _nya_lambda_prune(const _NYA_LambdaSet* set, NYA_Arena* arena) {
    nya_assert(set != nullptr && arena != nullptr);

    NYA_ArrayᐸNYA_Stringᐳ* existing = nya_array_create(arena, NYA_String);
    NYA_EXPECT(nya_filesystem_walk(arena, NYA_LAMBDA_OUTPUT_DIRECTORY, _nya_lambda_collect, existing), "while listing the lambda directory");

    nya_array_sort(existing, _nya_lambda_compare);

    nya_array_foreach (existing, file) {
        NYA_CString path = nya_string_to_cstring(arena, file);

        // By name: the walk reports a path relative to the tree it was given, which is not the spelling the companions were written under, and one directory cannot hold two files of one name.
        NYA_ConstCString name = _nya_lambda_basename(path);

        b8 wanted = false;
        for (u32 i = 0; i < set->source_count; i++) {
            wanted = wanted || nya_string_equals(_nya_lambda_basename(set->sources[i].companion), name);
        }

        // A call site that is gone has to take its function with it: a companion left behind would keep compiling into the file that no longer asks for it.
        if (wanted) continue;

        NYA_EXPECT(nya_filesystem_delete(path), "while removing the stale companion %s", path);
        nya_log_info("nya_lambda_generate: %s no longer has lambdas; its companion is gone.", path);
    }
}
