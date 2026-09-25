#include "nyangine-core/nyangine.h"

#include "nyangine-build/build.h"

/* TYPES */

/** Why a declaration in a watched function is not in the ring. Written into the companion, so it shows. */
typedef enum {
    _NYA_WATCH_SKIP_ARRAY,
    _NYA_WATCH_SKIP_MANY,
    _NYA_WATCH_SKIP_SHAPE,
    _NYA_WATCH_SKIP_LATE,

    _NYA_WATCH_SKIP_COUNT,
} _NYA_WatchSkip;

NYA_INTERNAL NYA_ConstCString _NYA_WATCH_SKIP_REASON_MAP[_NYA_WATCH_SKIP_COUNT] = {
    [_NYA_WATCH_SKIP_ARRAY] = "an array, which has no single value to print",
    [_NYA_WATCH_SKIP_MANY]  = "more than one name in one declaration",
    [_NYA_WATCH_SKIP_SHAPE] = "a declarator this pass does not read",
    [_NYA_WATCH_SKIP_LATE]  = "declared below nya_watch(), so it is not in scope there",
};

/** One source file with at least one annotated function, and where its companion goes. */
typedef struct {
    /** As the walk handed it over: "./src/gnyame/layers/layer_cube3d_stones.c". */
    char path[NYA_WATCH_MAX_PATH];

    /** "./src/genyarated/watches/gnyame_layers_layer_cube3d_stones_c.h". */
    char companion[NYA_WATCH_MAX_PATH];

    /** What the source has to spell to include it. */
    char include[NYA_WATCH_MAX_PATH];
} _NYA_WatchSource;

/** One local that goes into the ring: its name, and its type exactly as somebody wrote it. */
typedef struct {
    char name[NYA_WATCH_MAX_NAME];
    char type[NYA_WATCH_MAX_NAME];
} _NYA_WatchLocal;

/** One that does not, and why. */
typedef struct {
    char           name[NYA_WATCH_MAX_NAME];
    _NYA_WatchSkip reason;
    u32            line;
} _NYA_WatchSkipped;

typedef struct {
    char name[NYA_WATCH_MAX_NAME];

    /** Index into the set's sources. */
    u32 source;

    /** Where the annotation was written, 1-based. */
    u32 line;

    _NYA_WatchLocal locals[NYA_WATCH_MAX_LOCALS];
    u32             local_count;

    _NYA_WatchSkipped skipped[NYA_WATCH_MAX_SKIPPED];
    u32               skipped_count;
} _NYA_WatchFunction;

typedef struct {
    _NYA_WatchFunction functions[NYA_WATCH_MAX_FUNCTIONS];
    u32                count;

    _NYA_WatchSource sources[NYA_WATCH_MAX_SOURCES];
    u32              source_count;

    /** Everything wrong with the tree, counted rather than thrown, so one run reports all of it. */
    u32 problems;
} _NYA_WatchSet;

/* CONSTANTS */

NYA_INTERNAL const NYA_ConstCString _NYA_WATCH_TREES[] = {
    NYA_WATCH_TREE_ENGINE,
    NYA_WATCH_TREE_GAME,
    NYA_WATCH_TREE_EXAMPLES,
    NYA_WATCH_TREE_TESTS,
};

/**
 * Statements that open with one of these are not declarations, however much the first two tokens look
 * like a type and a name. Everything else that is not a declaration is refused by its shape.
 * */
NYA_INTERNAL const NYA_ConstCString _NYA_WATCH_NOT_A_TYPE[] = {
    "return", "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "goto", "defer", "typedef", "sizeof",
};

/** Longest declarator this pass reads, in tokens. A plain one is three or four. */
#define _NYA_WATCH_DECLARATOR_MAX 16

/* PRIVATE API DECLARATION */

NYA_INTERNAL b8  _nya_watch_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL s32 _nya_watch_compare(const NYA_String* a, const NYA_String* b);

/** Reports one problem against the line somebody wrote, and keeps going so the next one is found too. */
NYA_INTERNAL void _nya_watch_problem(_NYA_WatchSet* set, NYA_ConstCString path, u32 line, NYA_ConstCString format, ...) __attr_fmt_printf(4, 5);

NYA_INTERNAL b8   _nya_watch_token_is(const NYA_Lexer* lexer, u32 index, NYA_ConstCString text) __attr_no_discard;
NYA_INTERNAL b8   _nya_watch_token_symbol(const NYA_Lexer* lexer, u32 index, char symbol) __attr_no_discard;
NYA_INTERNAL void _nya_watch_token_copy(const NYA_Lexer* lexer, u32 index, OUT char* out, u64 capacity);

/** Whether the comment at `index` carries `marker` at the start of one of its lines. */
NYA_INTERNAL b8 _nya_watch_comment_has(const NYA_Lexer* lexer, u32 index, NYA_ConstCString marker) __attr_no_discard;

/** The first token at or after `index` that is not a comment. */
NYA_INTERNAL u32 _nya_watch_skip_comments(const NYA_Lexer* lexer, u32 index) __attr_no_discard;

/** The index just past the `close` matching the `open` at `from`, or zero when nothing closes it. */
NYA_INTERNAL u32 _nya_watch_match(const NYA_Lexer* lexer, u32 from, char open, char close) __attr_no_discard;

/** The source between two tokens, trimmed: the type as somebody spelled it. */
NYA_INTERNAL b8 _nya_watch_spelling(const NYA_Lexer* lexer, u32 from, u32 to, OUT char* out, u64 capacity) __attr_no_discard;

/** The tokens of one declaration up to its initialiser, comments dropped. False when there are too many. */
NYA_INTERNAL b8
_nya_watch_declarator(const NYA_Lexer* lexer, u32 from, u32 to, OUT u32* out_tokens, u32 capacity, OUT u32* out_count) __attr_no_discard;

/**
 * Reads one declaration, whether a parameter or a statement.
 *
 * Returns true when it is one this pass can watch and fills `out_local`; returns false and fills
 * `out_reason` when it is a declaration it cannot; returns false and leaves `out_reason` untouched,
 * with `out_skipped` false, when it is not a declaration at all.
 * */
NYA_INTERNAL b8 _nya_watch_read_declaration(
    const NYA_Lexer*    lexer,
    u32                 from,
    u32                 to,
    OUT _NYA_WatchLocal* out_local,
    OUT _NYA_WatchSkip*  out_reason,
    OUT b8*              out_skipped
) __attr_no_discard;

/** Adds one watched local, or reports that the function has more than the bound allows. */
NYA_INTERNAL void _nya_watch_local_add(_NYA_WatchSet* set, _NYA_WatchFunction* function, NYA_ConstCString path, const _NYA_WatchLocal* local);
NYA_INTERNAL void _nya_watch_skipped_add(_NYA_WatchFunction* function, NYA_ConstCString name, _NYA_WatchSkip reason, u32 line);

/** "./src/gnyame/robots.c" becomes "gnyame_robots_c". */
NYA_INTERNAL void            _nya_watch_mangle(NYA_ConstCString path, OUT char* out, u64 capacity);
NYA_INTERNAL NYA_ConstCString _nya_watch_basename(NYA_ConstCString path) __attr_no_discard;

/** The set's entry for `path`, appending one the first time the file is seen. */
NYA_INTERNAL u32 _nya_watch_source(_NYA_WatchSet* set, NYA_ConstCString path) __attr_no_discard;

/** Everything the scanner does with one file: every annotation in it, and every call site it explains. */
NYA_INTERNAL void _nya_watch_scan_file(_NYA_WatchSet* set, NYA_Arena* arena, NYA_ConstCString path);

/** One annotated function, from the comment that annotated it. Returns the token just past its body. */
NYA_INTERNAL u32 _nya_watch_parse(_NYA_WatchSet* set, NYA_ConstCString path, const NYA_Lexer* lexer, u32 marker, OUT u32* out_call);

/** One source file's companion header. */
NYA_INTERNAL void _nya_watch_emit_companion(const _NYA_WatchSet* set, u32 source, NYA_String* out);

/** Deletes whatever is in the output directory that this run did not write. */
NYA_INTERNAL void _nya_watch_prune(const _NYA_WatchSet* set, NYA_Arena* arena);

/* PUBLIC API IMPLEMENTATION */

void nya_watch_generate(void) {
    NYA_ConstCString inputs[] = {
        NYA_WATCH_TREE_ENGINE, NYA_WATCH_TREE_GAME, NYA_WATCH_TREE_EXAMPLES, NYA_WATCH_TREE_TESTS, "./src/nyangine-build/pp/watch.c", nullptr,
    };
    NYA_ConstCString outputs[] = { NYA_WATCH_OUTPUT_MANIFEST, nullptr };
    if (nya_pp_is_current("generate_watches", inputs, outputs)) return;

    NYA_Arena* arena = nya_arena_create(.name = "watch_generate");
    defer      nya_arena_destroy(arena);

    _NYA_WatchSet* set = nya_arena_alloc(arena, sizeof(_NYA_WatchSet));
    nya_memset(set, 0, sizeof(*set));

    for (u32 tree = 0; tree < nya_carray_length(_NYA_WATCH_TREES); tree++) {
        NYA_ArrayᐸNYA_Stringᐳ* files = nya_array_create(arena, NYA_String);
        NYA_EXPECT(nya_filesystem_walk(arena, _NYA_WATCH_TREES[tree], _nya_watch_collect, files), "while listing %s", _NYA_WATCH_TREES[tree]);

        // Sorted, so the manifest and every companion are a function of the tree rather than of the order the filesystem happened to hand it over.
        nya_array_sort(files, _nya_watch_compare);

        nya_array_foreach (files, file) _nya_watch_scan_file(set, arena, nya_string_to_cstring(arena, file));
    }

    // A source that forgot the include would get an undeclared function for every watched local; it is cheaper to say so here, with the line to add.
    for (u32 i = 0; i < set->source_count; i++) {
        NYA_String* text = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(set->sources[i].path, text), "while reading %s", set->sources[i].path);

        if (nya_string_contains(nya_string_to_cstring(arena, text), set->sources[i].include)) continue;

        _nya_watch_problem(set, set->sources[i].path, 1, "watches a function but does not include what that generates; add #include \"%s\"",
                           set->sources[i].include);
    }

    /* Nothing has been written yet, and nothing is written now: a function this pass could not read would otherwise leave one companion rewritten and the next one as it was. */
    if (set->problems > 0) nya_log_panic("nya_watch_generate: " FMTu32 " problem(s); nothing was written.", set->problems);

    NYA_EXPECT(nya_filesystem_create_directory(NYA_WATCH_OUTPUT_DIRECTORY), "while creating the watch directory");

    for (u32 i = 0; i < set->source_count; i++) {
        NYA_String* companion = nya_string_create(arena);
        _nya_watch_emit_companion(set, i, companion);

        NYA_EXPECT(nya_file_write(set->sources[i].companion, companion), "while writing %s", set->sources[i].companion);
    }

    _nya_watch_prune(set, arena);

    // Last, so its timestamp says the whole run finished. See NYA_WATCH_OUTPUT_MANIFEST.
    NYA_String* manifest = nya_string_create(arena);
    nya_string_extend(manifest,
                      "# GENERATED by src/nyangine-build/pp/watch.c. DO NYAT EDIT.\n"
                      "#\n"
                      "# Every function that asked to be watched, and every local a crash report will print for it.\n"
                      "# A name here is a macro name across the whole tree, so this is the namespace, and the file\n"
                      "# the pass compares its own age against.\n");

    u32 locals = 0;
    for (u32 i = 0; i < set->count; i++) {
        const _NYA_WatchFunction* function = &set->functions[i];

        nya_string_extend_sprintf(manifest, "%s\t%s:" FMTu32 "\n", function->name, set->sources[function->source].path, function->line);

        for (u32 local = 0; local < function->local_count; local++) {
            nya_string_extend_sprintf(manifest, "\t%s %s\n", function->locals[local].type, function->locals[local].name);
            locals++;
        }
    }

    NYA_EXPECT(nya_file_write(NYA_WATCH_OUTPUT_MANIFEST, manifest), "while writing the watch manifest");

    nya_log_info("nya_watch_generate: " FMTu32 " locals in " FMTu32 " functions from " FMTu32 " files into %s.", locals, set->count, set->source_count,
                 NYA_WATCH_OUTPUT_DIRECTORY);
}

/* PRIVATE API IMPLEMENTATION */

b8 _nya_watch_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* files = user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;
    if (!nya_string_ends_with(entry->name, ".c") && !nya_string_ends_with(entry->name, ".h")) return true;

    NYA_String* full = nya_string_from(files->arena, path);
    nya_array_push_back(files, *full);

    return true;
}

s32 _nya_watch_compare(const NYA_String* a, const NYA_String* b) {
    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);

    if (difference != 0) return difference < 0 ? -1 : 1;
    if (a->length == b->length) return 0;

    return a->length < b->length ? -1 : 1;
}

void _nya_watch_problem(_NYA_WatchSet* set, NYA_ConstCString path, u32 line, NYA_ConstCString format, ...) {
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

b8 _nya_watch_token_is(const NYA_Lexer* lexer, u32 index, NYA_ConstCString text) {
    if (index >= lexer->tokens->length) return false;

    NYA_Token token = lexer->tokens->items[index];
    u64       length = strlen(text);

    if (token.type != NYA_TOKEN_IDENT || token.length != length) return false;

    return nya_memcmp(lexer->source + token.source_location, text, length) == 0;
}

b8 _nya_watch_token_symbol(const NYA_Lexer* lexer, u32 index, char symbol) {
    if (index >= lexer->tokens->length) return false;

    NYA_Token token = lexer->tokens->items[index];

    return token.type == NYA_TOKEN_SYMBOL && token.symbol == (u8)symbol;
}

void _nya_watch_token_copy(const NYA_Lexer* lexer, u32 index, OUT char* out, u64 capacity) {
    out[0] = '\0';

    if (index >= lexer->tokens->length) return;

    NYA_Token token  = lexer->tokens->items[index];
    u64       length = token.length < capacity - 1 ? token.length : capacity - 1;

    nya_memcpy(out, lexer->source + token.source_location, length);
    out[length] = '\0';
}

b8 _nya_watch_comment_has(const NYA_Lexer* lexer, u32 index, NYA_ConstCString marker) {
    if (index >= lexer->tokens->length) return false;

    NYA_Token token = lexer->tokens->items[index];

    if (token.type != NYA_TOKEN_COMMENT) return false;

    u64 marker_length = strlen(marker);
    if (token.length < marker_length) return false;

    /* Only at the start of a line inside the comment, so that prose naming the annotation is prose and the annotation is an annotation. */
    for (u64 i = 0; i + marker_length <= token.length; i++) {
        if (nya_memcmp(lexer->source + token.source_location + i, marker, marker_length) != 0) continue;

        b8 at_line_start = true;

        for (u64 back = i; back > 0; back--) {
            u8 previous = (u8)lexer->source[token.source_location + back - 1];

            if (previous == '\n') break;
            if (previous == ' ' || previous == '\t' || previous == '*') continue;

            at_line_start = false;
            break;
        }

        if (at_line_start) return true;
    }

    return false;
}

u32 _nya_watch_skip_comments(const NYA_Lexer* lexer, u32 index) {
    u32 at = index;
    while (at < lexer->tokens->length && lexer->tokens->items[at].type == NYA_TOKEN_COMMENT) at++;

    return at;
}

u32 _nya_watch_match(const NYA_Lexer* lexer, u32 from, char open, char close) {
    nya_assert(_nya_watch_token_symbol(lexer, from, open));

    u32 depth = 0;

    for (u32 at = from; at < lexer->tokens->length; at++) {
        if (_nya_watch_token_symbol(lexer, at, open)) depth++;
        if (!_nya_watch_token_symbol(lexer, at, close)) continue;

        depth--;
        if (depth == 0) return at + 1;
    }

    return 0;
}

b8 _nya_watch_spelling(const NYA_Lexer* lexer, u32 from, u32 to, OUT char* out, u64 capacity) {
    nya_assert(out != nullptr && capacity > 1);

    out[0] = '\0';

    if (from >= to || to > lexer->tokens->length) return false;

    const NYA_Token first = lexer->tokens->items[from];
    const NYA_Token last  = lexer->tokens->items[to - 1];

    u64 start  = first.source_location;
    u64 end    = (u64)last.source_location + last.length;
    u64 length = end - start;

    while (length > 0 && isspace((unsigned char)lexer->source[start])) {
        start++;
        length--;
    }
    while (length > 0 && isspace((unsigned char)lexer->source[start + length - 1])) length--;

    if (length >= capacity) return false;

    nya_memcpy(out, lexer->source + start, length);
    out[length] = '\0';

    return true;
}

b8 _nya_watch_declarator(const NYA_Lexer* lexer, u32 from, u32 to, OUT u32* out_tokens, u32 capacity, OUT u32* out_count) {
    nya_assert(out_tokens != nullptr && out_count != nullptr);

    *out_count = 0;

    u32 depth = 0;

    for (u32 at = from; at < to; at++) {
        const NYA_Token token = lexer->tokens->items[at];

        if (token.type == NYA_TOKEN_COMMENT) continue;

        if (token.type == NYA_TOKEN_SYMBOL) {
            if (token.symbol == '(' || token.symbol == '[' || token.symbol == '{') depth++;

            // A span can begin inside a bracket it never opened, because a compound literal in an `if` condition ends a block the statement scanner was counting. Nothing to close, then.
            if ((token.symbol == ')' || token.symbol == ']' || token.symbol == '}') && depth > 0) depth--;

            // The initialiser is not part of the declarator, and it is where every expression that could look like anything at all lives.
            if (token.symbol == '=' && depth == 0) break;
        }

        if (*out_count >= capacity) return false;

        out_tokens[*out_count] = at;
        *out_count            += 1;
    }

    return true;
}

b8 _nya_watch_read_declaration(
    const NYA_Lexer*     lexer,
    u32                  from,
    u32                  to,
    OUT _NYA_WatchLocal* out_local,
    OUT _NYA_WatchSkip*  out_reason,
    OUT b8*              out_skipped
) {
    nya_assert(out_local != nullptr && out_reason != nullptr && out_skipped != nullptr);

    *out_skipped = false;

    u32 tokens[_NYA_WATCH_DECLARATOR_MAX] = { 0 };
    u32 count                             = 0;

    if (!_nya_watch_declarator(lexer, from, to, tokens, nya_carray_length(tokens), &count)) return false;

    // A type and a name at the very least. One token is an expression statement or a label.
    if (count < 2) return false;

    if (lexer->tokens->items[tokens[0]].type != NYA_TOKEN_IDENT) return false;

    for (u32 i = 0; i < nya_carray_length(_NYA_WATCH_NOT_A_TYPE); i++) {
        if (_nya_watch_token_is(lexer, tokens[0], _NYA_WATCH_NOT_A_TYPE[i])) return false;
    }

    // What makes it a declaration rather than a call or an assignment: a second name, or a pointer star, where an expression would have an operator.
    const b8 declaration = lexer->tokens->items[tokens[1]].type == NYA_TOKEN_IDENT || _nya_watch_token_symbol(lexer, tokens[1], '*');
    if (!declaration) return false;

    *out_skipped = true;

    for (u32 i = 0; i < count; i++) {
        if (lexer->tokens->items[tokens[i]].type == NYA_TOKEN_IDENT) continue;
        if (_nya_watch_token_symbol(lexer, tokens[i], '*')) continue;

        if (_nya_watch_token_symbol(lexer, tokens[i], '[')) {
            *out_reason = _NYA_WATCH_SKIP_ARRAY;
        } else if (_nya_watch_token_symbol(lexer, tokens[i], ',')) {
            *out_reason = _NYA_WATCH_SKIP_MANY;
        } else {
            *out_reason = _NYA_WATCH_SKIP_SHAPE;
        }

        // The name is whatever was written first, which is the only thing that would be recognised.
        _nya_watch_token_copy(lexer, tokens[i > 0 ? i - 1 : 0], out_local->name, sizeof(out_local->name));

        return false;
    }

    if (lexer->tokens->items[tokens[count - 1]].type != NYA_TOKEN_IDENT) {
        *out_reason = _NYA_WATCH_SKIP_SHAPE;
        _nya_watch_token_copy(lexer, tokens[0], out_local->name, sizeof(out_local->name));

        return false;
    }

    _nya_watch_token_copy(lexer, tokens[count - 1], out_local->name, sizeof(out_local->name));

    if (!_nya_watch_spelling(lexer, tokens[0], tokens[count - 1], out_local->type, sizeof(out_local->type))) {
        *out_reason = _NYA_WATCH_SKIP_SHAPE;
        return false;
    }

    *out_skipped = false;

    return true;
}

void _nya_watch_local_add(_NYA_WatchSet* set, _NYA_WatchFunction* function, NYA_ConstCString path, const _NYA_WatchLocal* local) {
    nya_assert(set != nullptr && function != nullptr && local != nullptr);

    if (function->local_count >= NYA_WATCH_MAX_LOCALS) {
        _nya_watch_problem(set, path, function->line, "'%s' watches more than %d locals; raise NYA_WATCH_MAX_LOCALS or watch fewer", function->name,
                           NYA_WATCH_MAX_LOCALS);
        return;
    }

    function->locals[function->local_count] = *local;
    function->local_count++;
}

void _nya_watch_skipped_add(_NYA_WatchFunction* function, NYA_ConstCString name, _NYA_WatchSkip reason, u32 line) {
    nya_assert(function != nullptr);

    // Dropped rather than grown: this list only exists so the companion can say what it left out, and a function with sixteen of them has been told plenty.
    if (function->skipped_count >= NYA_WATCH_MAX_SKIPPED) return;

    _NYA_WatchSkipped* skipped = &function->skipped[function->skipped_count];

    (void)snprintf(skipped->name, sizeof(skipped->name), "%s", name);
    skipped->reason = reason;
    skipped->line   = line;

    function->skipped_count++;
}

void _nya_watch_mangle(NYA_ConstCString path, OUT char* out, u64 capacity) {
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

NYA_ConstCString _nya_watch_basename(NYA_ConstCString path) {
    nya_assert(path != nullptr);

    const char* separator = strrchr(path, '/');
    return separator != nullptr ? separator + 1 : path;
}

u32 _nya_watch_source(_NYA_WatchSet* set, NYA_ConstCString path) {
    nya_assert(set != nullptr && path != nullptr);

    for (u32 i = 0; i < set->source_count; i++) {
        if (nya_string_equals(set->sources[i].path, path)) return i;
    }

    nya_assert(set->source_count < NYA_WATCH_MAX_SOURCES, "more than %d files watch a function; raise NYA_WATCH_MAX_SOURCES", NYA_WATCH_MAX_SOURCES);

    char mangled[NYA_WATCH_MAX_PATH];
    _nya_watch_mangle(path, mangled, sizeof(mangled));

    _NYA_WatchSource* source = &set->sources[set->source_count];

    (void)snprintf(source->path, sizeof(source->path), "%s", path);
    (void)snprintf(source->companion, sizeof(source->companion), "%s/%s.h", NYA_WATCH_OUTPUT_DIRECTORY, mangled);
    (void)snprintf(source->include, sizeof(source->include), "%s%s.h", NYA_WATCH_INCLUDE_PREFIX, mangled);

    set->source_count++;

    return set->source_count - 1;
}

void _nya_watch_scan_file(_NYA_WatchSet* set, NYA_Arena* arena, NYA_ConstCString path) {
    nya_assert(set != nullptr && arena != nullptr && path != nullptr);

    NYA_String* text = nya_string_create(arena);
    NYA_EXPECT(nya_file_read(path, text), "while reading %s", path);

    // Read whole and searched once: almost nothing in the tree is annotated, and lexing every file to find that out would be most of this pass's time.
    NYA_CString contents = nya_string_to_cstring(arena, text);
    if (strstr(contents, NYA_WATCH_MARKER) == nullptr && strstr(contents, NYA_WATCH_CALL "(") == nullptr) return;

    // Character literals, because this is C source: without them the quote in `c == '"'` opens a string that runs to the next quote in the file. UTF-8 identifiers, because the derived container types mangle their names with non-ASCII brackets.
    NYA_Lexer lexer = nya_lexer_create(contents, NYA_LEXER_UTF8_IDENTS | NYA_LEXER_CHAR_LITERALS);
    nya_lexer_run(&lexer);

    defer nya_lexer_destroy(&lexer);

    /* Every call site in the file, so that one left behind by a deleted annotation is reported rather than left to fail as an undeclared macro. A call on a preprocessor line is base_watch.h's own definition, or something wrapping it. */
    u32 calls[NYA_WATCH_MAX_FUNCTIONS] = { 0 };
    u32 call_count                     = 0;
    u32 preprocessor_line              = 0;

    for (u32 index = 0; index < lexer.tokens->length; index++) {
        if (_nya_watch_token_symbol(&lexer, index, '#')) preprocessor_line = lexer.tokens->items[index].line_number;

        if (!_nya_watch_token_is(&lexer, index, NYA_WATCH_CALL)) continue;
        if (lexer.tokens->items[index].line_number == preprocessor_line) continue;
        if (call_count >= nya_carray_length(calls)) continue;

        calls[call_count++] = index;
    }

    u32 answered[NYA_WATCH_MAX_FUNCTIONS] = { 0 };
    u32 answered_count                    = 0;

    for (u32 index = 0; index < lexer.tokens->length; index++) {
        if (!_nya_watch_comment_has(&lexer, index, NYA_WATCH_MARKER)) continue;

        u32 call = 0;
        index    = _nya_watch_parse(set, path, &lexer, index, &call);

        if (call != 0 && answered_count < nya_carray_length(answered)) answered[answered_count++] = call;
    }

    for (u32 i = 0; i < call_count; i++) {
        b8 answered_here = false;
        for (u32 j = 0; j < answered_count; j++) answered_here = answered_here || answered[j] == calls[i];

        if (answered_here) continue;

        _nya_watch_problem(set, path, lexer.tokens->items[calls[i]].line_number,
                           "nya_watch() here is not inside a function annotated // " NYA_WATCH_MARKER "; add the annotation or drop the call");
    }
}

u32 _nya_watch_parse(_NYA_WatchSet* set, NYA_ConstCString path, const NYA_Lexer* lexer, u32 marker, OUT u32* out_call) {
    nya_assert(set != nullptr && path != nullptr && lexer != nullptr && out_call != nullptr);

    *out_call = 0;

    const u32 line = lexer->tokens->items[marker].line_number;

    // Any further comment lines between the annotation and the function are skipped, so the marker may sit at the top of a doc block rather than immediately above the definition.
    u32 cursor = _nya_watch_skip_comments(lexer, marker + 1);

    // The declarator: everything up to the parameter list, whose name is the token in front of it.
    u32 open = cursor;
    while (open < lexer->tokens->length && !_nya_watch_token_symbol(lexer, open, '(')) {
        if (_nya_watch_token_symbol(lexer, open, ';') || _nya_watch_token_symbol(lexer, open, '{')) break;
        open++;
    }

    if (!_nya_watch_token_symbol(lexer, open, '(') || open == cursor) {
        _nya_watch_problem(set, path, line, NYA_WATCH_MARKER " is not above a function definition");
        return marker;
    }

    _NYA_WatchFunction function = { .line = line };
    _nya_watch_token_copy(lexer, open - 1, function.name, sizeof(function.name));

    const u32 parameters_end = _nya_watch_match(lexer, open, '(', ')');
    if (parameters_end == 0) {
        _nya_watch_problem(set, path, line, "'%s' has a parameter list that is never closed", function.name);
        return marker;
    }

    // Past anything between the parameters and the body, which is where an attribute would sit.
    u32 body = parameters_end;
    while (body < lexer->tokens->length && !_nya_watch_token_symbol(lexer, body, '{') && !_nya_watch_token_symbol(lexer, body, ';')) body++;

    if (!_nya_watch_token_symbol(lexer, body, '{')) {
        _nya_watch_problem(set, path, line, "'%s' is declared here, not defined; annotate the definition", function.name);
        return marker;
    }

    const u32 body_end = _nya_watch_match(lexer, body, '{', '}');
    if (body_end == 0) {
        _nya_watch_problem(set, path, line, "'%s' has a body that is never closed", function.name);
        return marker;
    }

    // the call site, which is where the registration goes and therefore what it can reach
    u32 call = 0;
    for (u32 at = body + 1; at < body_end && call == 0; at++) {
        if (_nya_watch_token_is(lexer, at, NYA_WATCH_CALL)) call = at;
    }

    if (call == 0) {
        _nya_watch_problem(set, path, line, "'%s' is annotated " NYA_WATCH_MARKER " but never calls nya_watch(%s); write it below the declarations",
                           function.name, function.name);
        return body_end - 1;
    }

    const u32 tag = _nya_watch_skip_comments(lexer, call + 1) + 1;
    if (!_nya_watch_token_symbol(lexer, call + 1, '(') || !_nya_watch_token_is(lexer, tag, function.name)) {
        char written[NYA_WATCH_MAX_NAME] = { 0 };
        _nya_watch_token_copy(lexer, tag, written, sizeof(written));

        _nya_watch_problem(set, path, lexer->tokens->items[call].line_number, "nya_watch(%s) inside '%s' names another function; write nya_watch(%s)",
                           written, function.name, function.name);
        return body_end - 1;
    }

    *out_call = call;

    // the parameters, which are locals that are always in scope and always initialised
    for (u32 at = open + 1; at < parameters_end - 1;) {
        u32 end   = at;
        u32 depth = 0;

        while (end < parameters_end - 1) {
            if (_nya_watch_token_symbol(lexer, end, '(') || _nya_watch_token_symbol(lexer, end, '[')) depth++;
            if (_nya_watch_token_symbol(lexer, end, ')') || _nya_watch_token_symbol(lexer, end, ']')) depth--;
            if (_nya_watch_token_symbol(lexer, end, ',') && depth == 0) break;
            end++;
        }

        _NYA_WatchLocal local   = { 0 };
        _NYA_WatchSkip  reason  = _NYA_WATCH_SKIP_SHAPE;
        b8              skipped = false;

        if (_nya_watch_read_declaration(lexer, at, end, &local, &reason, &skipped)) {
            _nya_watch_local_add(set, &function, path, &local);
        } else if (skipped) {
            _nya_watch_skipped_add(&function, local.name, reason, lexer->tokens->items[at].line_number);
        }

        at = end + 1;
    }

    // the body's own top level declarations, above the call site
    u32 depth           = 1;
    u32 parenthesis     = 0;
    u32 statement_start = body + 1;

    for (u32 at = body + 1; at < body_end - 1; at++) {
        const b8 statement_end = _nya_watch_token_symbol(lexer, at, ';') && depth == 1 && parenthesis == 0;

        if (_nya_watch_token_symbol(lexer, at, '(')) parenthesis++;
        if (_nya_watch_token_symbol(lexer, at, ')') && parenthesis > 0) parenthesis--;

        // Inside parentheses a brace belongs to a compound literal, not to a block: the statement it sits in is still running, and a `(NYA_UIPanel){ ... }` in an `if` must not end it.
        if (_nya_watch_token_symbol(lexer, at, '{') && parenthesis == 0) {
            depth++;
            statement_start = at + 1;
            continue;
        }
        if (_nya_watch_token_symbol(lexer, at, '}') && parenthesis == 0) {
            depth--;
            statement_start = at + 1;
            continue;
        }

        if (!statement_end) continue;

        _NYA_WatchLocal local   = { 0 };
        _NYA_WatchSkip  reason  = _NYA_WATCH_SKIP_SHAPE;
        b8              skipped = false;

        const b8 above = at < call;
        const b8 read  = _nya_watch_read_declaration(lexer, statement_start, at, &local, &reason, &skipped);

        statement_start = at + 1;

        if (!read && !skipped) continue;

        if (!above) {
            /* Below the call site the local is not in scope where the registration sits, so it cannot be watched at all. Written into the companion rather than logged: the companion is committed, so the omission turns up in the review of the annotation instead of in every later build. */
            _nya_watch_skipped_add(&function, local.name, _NYA_WATCH_SKIP_LATE, lexer->tokens->items[at].line_number);
            continue;
        }

        if (read) {
            _nya_watch_local_add(set, &function, path, &local);
        } else {
            _nya_watch_skipped_add(&function, local.name, reason, lexer->tokens->items[at].line_number);
        }
    }

    if (function.local_count == 0) {
        _nya_watch_problem(set, path, line, "'%s' watches nothing: no parameter and no declaration above nya_watch(%s) can be read", function.name,
                           function.name);
        return body_end - 1;
    }

    // the name is the name of a macro, and there is one namespace of those
    for (u32 i = 0; i < set->count; i++) {
        if (!nya_string_equals(set->functions[i].name, function.name)) continue;

        _nya_watch_problem(set, path, line, "'%s' is already watched at %s:" FMTu32 "; one of them has to be renamed", function.name,
                           set->sources[set->functions[i].source].path, set->functions[i].line);
        return body_end - 1;
    }

    if (set->count >= NYA_WATCH_MAX_FUNCTIONS) {
        _nya_watch_problem(set, path, line, "more than %d watched functions in the tree; raise NYA_WATCH_MAX_FUNCTIONS", NYA_WATCH_MAX_FUNCTIONS);
        return body_end - 1;
    }

    function.source = _nya_watch_source(set, path);

    set->functions[set->count] = function;
    set->count++;

    return body_end - 1;
}

void _nya_watch_emit_companion(const _NYA_WatchSet* set, u32 source, NYA_String* out) {
    nya_assert(set != nullptr && out != nullptr);
    nya_assert(source < set->source_count);

    nya_string_extend_sprintf(out,
                              "/*\n"
                              " * GENERATED by src/nyangine-build/pp/watch.c from the " NYA_WATCH_MARKER " annotations in %s. DO NYAT EDIT.\n"
                              " *\n"
                              " * Included by that file and by nothing else. Each macro is the body of one nya_watch() call: it\n"
                              " * opens a frame, writes down every local it can reach from where the call sits, and ends the\n"
                              " * frame in a defer, so every way out of the function unregisters what points into it.\n"
                              " */\n"
                              "#pragma once\n",
                              set->sources[source].path);

    for (u32 i = 0; i < set->count; i++) {
        const _NYA_WatchFunction* function = &set->functions[i];
        if (function->source != source) continue;

        nya_string_extend_sprintf(out, "\n/* %s (%s:" FMTu32 ") */\n", function->name, set->sources[source].path, function->line);

        for (u32 skipped = 0; skipped < function->skipped_count; skipped++) {
            nya_string_extend_sprintf(out, "/* not watched: %s, %s (line " FMTu32 ") */\n", function->skipped[skipped].name,
                                      _NYA_WATCH_SKIP_REASON_MAP[function->skipped[skipped].reason], function->skipped[skipped].line);
        }

        nya_string_extend_sprintf(out, "#define %s%s()%*sconst u32 _nya_watch_frame = nya_watch_frame_begin();", NYA_WATCH_PREFIX, function->name, 1,
                                  " \\\n    ");

        for (u32 local = 0; local < function->local_count; local++) {
            // sizeof over the type rather than over the name: sizeof of a pointer to a struct reads as a mistake to the linter, and half of what gets watched is one.
            nya_string_extend_sprintf(
                out, " \\\n    nya_watch_record(_nya_watch_frame, \"%s\", \"%s\", \"%s\", _nya_watch_type_of(%s), (u32)sizeof(typeof(%s)), &%s);",
                function->name, function->locals[local].name, function->locals[local].type, function->locals[local].name,
                function->locals[local].name, function->locals[local].name);
        }

        // No trailing semicolon: the one the caller wrote after nya_watch() finishes the defer.
        nya_string_extend_sprintf(out, " \\\n    defer nya_watch_frame_end(_nya_watch_frame)\n");
    }
}

void _nya_watch_prune(const _NYA_WatchSet* set, NYA_Arena* arena) {
    nya_assert(set != nullptr && arena != nullptr);

    NYA_ArrayᐸNYA_Stringᐳ* existing = nya_array_create(arena, NYA_String);
    NYA_EXPECT(nya_filesystem_walk(arena, NYA_WATCH_OUTPUT_DIRECTORY, _nya_watch_collect, existing), "while listing the watch directory");

    nya_array_sort(existing, _nya_watch_compare);

    nya_array_foreach (existing, file) {
        NYA_CString path = nya_string_to_cstring(arena, file);

        // By name: the walk reports a path relative to the tree it was given, which is not the spelling the companions were written under, and one directory cannot hold two files of one name.
        NYA_ConstCString name = _nya_watch_basename(path);

        b8 wanted = false;
        for (u32 i = 0; i < set->source_count; i++) {
            wanted = wanted || nya_string_equals(_nya_watch_basename(set->sources[i].companion), name);
        }

        // An annotation that is gone has to take its macro with it, or the source keeps including a registration for locals that may no longer be there.
        if (wanted) continue;

        NYA_EXPECT(nya_filesystem_delete(path), "while removing the stale companion %s", path);
        nya_log_info("nya_watch_generate: %s is no longer watched; its companion is gone.", path);
    }
}
