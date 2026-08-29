#include "nyangine/nyangine.h"

#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum {
    _NYA_REFLECT_DECL_STRUCT,
    _NYA_REFLECT_DECL_UNION,
    _NYA_REFLECT_DECL_ENUM,
} _NYA_ReflectDeclKind;

typedef struct {
    char name[NYA_REFLECT_MAX_NAME];

    /** The type as written, minus pointers and array brackets: "f32", "NYA_Color". */
    char type_spelling[NYA_REFLECT_MAX_NAME];

    /** The text between the brackets, verbatim, or empty. Emitted as-is for the compiler to evaluate. */
    char array_extent[NYA_REFLECT_MAX_NAME];

    /** From `@enum(X)` or `@flags(X)`: describe this field as that enum rather than as its integer. */
    char enum_override[NYA_REFLECT_MAX_NAME];

    u32 pointer_depth;

    NYA_ConstCString hint;
} _NYA_ReflectFieldDecl;

typedef struct {
    char name[NYA_REFLECT_MAX_NAME];
    char value_expression[NYA_REFLECT_MAX_NAME];
    b8   has_value;
} _NYA_ReflectVariantDecl;

typedef struct {
    char                 name[NYA_REFLECT_MAX_NAME];
    _NYA_ReflectDeclKind kind;

    _NYA_ReflectFieldDecl fields[NYA_REFLECT_MAX_FIELDS];
    u32                   field_count;

    _NYA_ReflectVariantDecl variants[NYA_REFLECT_MAX_VARIANTS];
    u32                     variant_count;

    b8 is_bitflags;

    char tag_field[NYA_REFLECT_MAX_NAME];
    char on_apply[NYA_REFLECT_MAX_NAME];

    char source_file[512];
} _NYA_ReflectTypeDecl;

typedef struct {
    _NYA_ReflectTypeDecl* types;
    u32                   type_count;

    NYA_Arena* arena;
} _NYA_ReflectSet;

NYA_INTERNAL b8   _nya_reflect_collect_sources(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL void _nya_reflect_scan_file(_NYA_ReflectSet* set, NYA_ConstCString path);
NYA_INTERNAL b8   _nya_reflect_token_is(const NYA_Lexer* lexer, u32 index, NYA_ConstCString text);
NYA_INTERNAL void _nya_reflect_token_copy(const NYA_Lexer* lexer, u32 index, OUT char* out, u64 capacity);
NYA_INTERNAL b8   _nya_reflect_comment_has(const NYA_Lexer* lexer, u32 index, NYA_ConstCString marker);
NYA_INTERNAL b8   _nya_reflect_annotation_argument(const NYA_Lexer* lexer, u32 index, NYA_ConstCString marker, OUT char* out, u64 capacity);
NYA_INTERNAL NYA_ConstCString _nya_reflect_hint_from_comment(const NYA_Lexer* lexer, u32 index);
NYA_INTERNAL NYA_ConstCString _nya_reflect_builtin_symbol(NYA_ConstCString spelling);
NYA_INTERNAL b8   _nya_reflect_is_known(const _NYA_ReflectSet* set, NYA_ConstCString name);
NYA_INTERNAL s32  _nya_reflect_compare_paths(const NYA_String* a, const NYA_String* b);
NYA_INTERNAL void _nya_reflect_emit_builtins(NYA_String* out);
NYA_INTERNAL void _nya_reflect_emit_type(const _NYA_ReflectSet* set, NYA_String* out, const _NYA_ReflectTypeDecl* decl);
NYA_INTERNAL u32  _nya_reflect_parse_members(_NYA_ReflectTypeDecl* decl, const NYA_Lexer* lexer, u32 start, NYA_ConstCString path);
NYA_INTERNAL u32  _nya_reflect_parse_variants(_NYA_ReflectTypeDecl* decl, const NYA_Lexer* lexer, u32 start, NYA_ConstCString path);
NYA_INTERNAL NYA_ConstCString _nya_reflect_field_symbol(const _NYA_ReflectSet* set, const _NYA_ReflectFieldDecl* field, OUT char* buffer, u64 capacity);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_reflection_generate(void) {
    NYA_ConstCString inputs[] = {
        NYA_REFLECT_ENGINE_DIRECTORY,
        NYA_REFLECT_GAME_DIRECTORY,
        "./src/build/pp/reflection.c",
        nullptr,
    };
    NYA_ConstCString outputs[] = { NYA_REFLECT_OUTPUT_HEADER, NYA_REFLECT_OUTPUT_SOURCE, nullptr };
    if (nya_pp_is_current("generate_reflection", inputs, outputs)) return;

    NYA_Arena* arena = nya_arena_create(.name = "reflection_generate");
    defer      nya_arena_destroy(arena);

    _NYA_ReflectSet set = {
        .arena = arena,
        .types = nya_arena_alloc(arena, sizeof(_NYA_ReflectTypeDecl) * NYA_REFLECT_MAX_TYPES),
    };

    nya_memset(set.types, 0, sizeof(_NYA_ReflectTypeDecl) * NYA_REFLECT_MAX_TYPES);

    /*
     * Sources are collected and sorted before any of them is read.
     *
     * The filesystem walks in whatever order it likes, and a generated file that reorders itself
     * between machines is a diff nobody can review and a rebuild nobody asked for.
     */
    NYA_ArrayᐸNYA_Stringᐳ* sources = nya_array_create(arena, NYA_String);

    NYA_EXPECT(nya_filesystem_walk(arena, NYA_REFLECT_ENGINE_DIRECTORY, _nya_reflect_collect_sources, sources));
    NYA_EXPECT(nya_filesystem_walk(arena, NYA_REFLECT_GAME_DIRECTORY, _nya_reflect_collect_sources, sources));

    nya_array_sort(sources, _nya_reflect_compare_paths);

    nya_array_foreach (sources, source) {
        _nya_reflect_scan_file(&set, nya_string_to_cstring(arena, source));
    }

    nya_log_info("nya_reflection_generate: %u annotated types across %llu headers.", set.type_count,
             (unsigned long long)sources->length);

    // ── the header ──────────────────────────────────────────────────────────────────────────────
    NYA_String* header = nya_string_create(arena);

    nya_string_extend(header, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n#pragma once\n\n");
    nya_string_extend(header, "#include \"nyangine/base/base_reflection.h\"\n\n");
    nya_string_extend(header,
                      "/*\n"
                      " * Generated by src/build/reflection.c from the @reflect annotations in the tree.\n"
                      " *\n"
                      " * Reach one of these through nya_reflect_of(TypeName) rather than by naming the symbol: the\n"
                      " * macro is what makes a misspelling a link error instead of a null at runtime.\n"
                      " */\n\n");

    for (u32 i = 0; i < set.type_count; i++) {
        nya_string_extend_sprintf(header, "extern const NYA_TypeReflection _NYA_REFLECT_%s;\n", set.types[i].name);
    }

    nya_string_extend(header, "\n/** Every annotated type, for an editor that needs to enumerate them. */\n");
    nya_string_extend_sprintf(header, "#define NYA_REFLECT_TYPE_COUNT %u\n\n", set.type_count);
    nya_string_extend(header, "extern const NYA_TypeReflection* const NYA_REFLECT_TYPES[NYA_REFLECT_TYPE_COUNT];\n\n");
    nya_string_extend(header, "/** The type called `name`, or null. Linear: this is an editor path, not a hot one. */\n");
    nya_string_extend(header, "NYA_API const NYA_TypeReflection* nya_reflect_find(NYA_ConstCString name) __attr_no_discard;\n");

    NYA_EXPECT(nya_file_write(NYA_REFLECT_OUTPUT_HEADER, header), "while writing the generated reflection header");

    // ── the source ──────────────────────────────────────────────────────────────────────────────
    NYA_String* out = nya_string_create(arena);

    nya_string_extend(out, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(out, "#include \"nyangine/nyangine.h\"\n\n");
    nya_string_extend(out,
                      "/*\n"
                      " * Every size and offset below is an expression rather than a number, so the compiler that is\n"
                      " * already compiling these structs is what computes the layout. See src/build/reflection.h.\n"
                      " */\n\n");

    _nya_reflect_emit_builtins(out);

    for (u32 i = 0; i < set.type_count; i++) {
        _nya_reflect_emit_type(&set, out, &set.types[i]);
    }

    // ── the table and the lookup ────────────────────────────────────────────────────────────────
    nya_string_extend(out, "const NYA_TypeReflection* const NYA_REFLECT_TYPES[NYA_REFLECT_TYPE_COUNT] = {\n");
    for (u32 i = 0; i < set.type_count; i++) {
        nya_string_extend_sprintf(out, "    &_NYA_REFLECT_%s,\n", set.types[i].name);
    }
    nya_string_extend(out, "};\n\n");

    nya_string_extend(out,
                      "const NYA_TypeReflection* nya_reflect_find(NYA_ConstCString name) {\n"
                      "    if (name == nullptr) return nullptr;\n\n"
                      "    for (u32 i = 0; i < NYA_REFLECT_TYPE_COUNT; i++) {\n"
                      "        if (nya_string_equals(NYA_REFLECT_TYPES[i]->name, name)) return NYA_REFLECT_TYPES[i];\n"
                      "    }\n\n"
                      "    return nullptr;\n"
                      "}\n");

    NYA_EXPECT(nya_file_write(NYA_REFLECT_OUTPUT_SOURCE, out), "while writing the generated reflection source");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 _nya_reflect_compare_paths(const NYA_String* a, const NYA_String* b) {
    u64 shortest = a->length < b->length ? a->length : b->length;

    for (u64 i = 0; i < shortest; i++) {
        if (a->items[i] != b->items[i]) return a->items[i] < b->items[i] ? -1 : 1;
    }

    if (a->length == b->length) return 0;

    return a->length < b->length ? -1 : 1;
}

b8 _nya_reflect_collect_sources(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* sources = user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;

    // Headers only. A declaration lives in a header by this codebase's convention, and scanning the
    // .c files as well would double the work to find nothing.
    if (!nya_string_ends_with(entry->name, ".h")) return true;

    // `path` is already the full path to this file, not the directory holding it. See _nya_asset_collect.
    NYA_String* full = nya_string_from(sources->arena, path);

    nya_array_push_back(sources, *full);

    return true;
}

b8 _nya_reflect_token_is(const NYA_Lexer* lexer, u32 index, NYA_ConstCString text) {
    if (index >= lexer->tokens->length) return false;

    NYA_Token token = lexer->tokens->items[index];

    u64 length = strlen(text);

    if (token.length != length) return false;

    return nya_memcmp(lexer->source + token.source_location, text, length) == 0;
}

void _nya_reflect_token_copy(const NYA_Lexer* lexer, u32 index, OUT char* out, u64 capacity) {
    out[0] = '\0';

    if (index >= lexer->tokens->length) return;

    NYA_Token token = lexer->tokens->items[index];

    u64 length = token.length < capacity - 1 ? token.length : capacity - 1;

    nya_memcpy(out, lexer->source + token.source_location, length);
    out[length] = '\0';
}

/** Whether the comment token at `index` contains `marker` anywhere in its body. */
b8 _nya_reflect_comment_has(const NYA_Lexer* lexer, u32 index, NYA_ConstCString marker) {
    if (index >= lexer->tokens->length) return false;

    NYA_Token token = lexer->tokens->items[index];

    if (token.type != NYA_TOKEN_COMMENT) return false;

    u64 marker_length = strlen(marker);

    if (token.length < marker_length) return false;

    /*
     * Searched, but only at the start of a line within the comment.
     *
     * Searching anywhere would make this file's own documentation an annotation — base_reflection.h
     * explains what `@reflect` is, and a prose mention of it is not a declaration of one. Requiring
     * the marker to open its line separates "this type is annotated" from "this paragraph is about
     * annotations", while still letting the marker sit inside a doc comment that already says other
     * things on other lines.
     */
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

/** The text inside `marker(...)` in the comment at `index`, if present. */
b8 _nya_reflect_annotation_argument(const NYA_Lexer* lexer, u32 index, NYA_ConstCString marker, OUT char* out, u64 capacity) {
    out[0] = '\0';

    if (index >= lexer->tokens->length) return false;

    NYA_Token token = lexer->tokens->items[index];

    if (token.type != NYA_TOKEN_COMMENT) return false;

    u64 marker_length = strlen(marker);

    for (u64 i = 0; i + marker_length <= token.length; i++) {
        if (nya_memcmp(lexer->source + token.source_location + i, marker, marker_length) != 0) continue;

        u64 cursor = i + marker_length;

        while (cursor < token.length && lexer->source[token.source_location + cursor] == ' ') cursor++;
        if (cursor >= token.length || lexer->source[token.source_location + cursor] != '(') return false;

        cursor++;

        u64 written = 0;

        while (cursor < token.length && lexer->source[token.source_location + cursor] != ')' && written < capacity - 1) {
            out[written] = lexer->source[token.source_location + cursor];
            written++;
            cursor++;
        }

        out[written] = '\0';

        return written > 0;
    }

    return false;
}

NYA_ConstCString _nya_reflect_hint_from_comment(const NYA_Lexer* lexer, u32 index) {
    char argument[NYA_REFLECT_MAX_NAME] = { 0 };

    if (!_nya_reflect_annotation_argument(lexer, index, "@hint", argument, sizeof(argument))) return "NYA_HINT_NONE";

    if (nya_string_equals(argument, "position")) return "NYA_HINT_POSITION";
    if (nya_string_equals(argument, "scale")) return "NYA_HINT_SCALE";
    if (nya_string_equals(argument, "euler")) return "NYA_HINT_EULER";
    if (nya_string_equals(argument, "color")) return "NYA_HINT_COLOR";
    if (nya_string_equals(argument, "asset")) return "NYA_HINT_ASSET";
    if (nya_string_equals(argument, "bitflags")) return "NYA_HINT_BITFLAGS";

    nya_log_warn("nya_reflection_generate: unknown @hint(%s), treated as none.", argument);

    return "NYA_HINT_NONE";
}

/**
 * The reflection symbol for a builtin spelling, or null if it is not one.
 *
 * The vectors are here rather than synthesised because they are typedefs of a clang attribute rather
 * than of a struct — nothing in the source says "three floats" in a form a parser could read, so the
 * list is written out once. See NYA_REFLECT_VECTOR on why their size is not three times four.
 * */
NYA_ConstCString _nya_reflect_builtin_symbol(NYA_ConstCString spelling) {
    static const struct {
        NYA_ConstCString spelling;
        NYA_ConstCString symbol;
    } BUILTINS[] = {
        { "b8", "_NYA_REFLECT_b8" },       { "b16", "_NYA_REFLECT_b16" },   { "b32", "_NYA_REFLECT_b32" },
        { "b64", "_NYA_REFLECT_b64" },     { "u8", "_NYA_REFLECT_u8" },     { "u16", "_NYA_REFLECT_u16" },
        { "u32", "_NYA_REFLECT_u32" },     { "u64", "_NYA_REFLECT_u64" },   { "s8", "_NYA_REFLECT_s8" },
        { "s16", "_NYA_REFLECT_s16" },     { "s32", "_NYA_REFLECT_s32" },   { "s64", "_NYA_REFLECT_s64" },
        { "f32", "_NYA_REFLECT_f32" },     { "f64", "_NYA_REFLECT_f64" },   { "char", "_NYA_REFLECT_char" },
        { "f32x2", "_NYA_REFLECT_f32x2" }, { "f32x3", "_NYA_REFLECT_f32x3" }, { "f32x4", "_NYA_REFLECT_f32x4" },
        { "NYA_ConstCString", "_NYA_REFLECT_string" },
        { "NYA_CString", "_NYA_REFLECT_string" },
    };

    for (u64 i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++) {
        if (nya_string_equals(BUILTINS[i].spelling, spelling)) return BUILTINS[i].symbol;
    }

    return nullptr;
}

b8 _nya_reflect_is_known(const _NYA_ReflectSet* set, NYA_ConstCString name) {
    for (u32 i = 0; i < set->type_count; i++) {
        if (nya_string_equals(set->types[i].name, name)) return true;
    }

    return false;
}

/** The fixed prelude: one reflection per primitive the tree actually uses, plus the vectors. */
void _nya_reflect_emit_builtins(NYA_String* out) {
    static const struct {
        NYA_ConstCString name;
        NYA_ConstCString primitive;
    } PRIMITIVES[] = {
        { "b8", "NYA_TYPE_B8" },     { "b16", "NYA_TYPE_B16" }, { "b32", "NYA_TYPE_B32" }, { "b64", "NYA_TYPE_B64" },
        { "u8", "NYA_TYPE_U8" },     { "u16", "NYA_TYPE_U16" }, { "u32", "NYA_TYPE_U32" }, { "u64", "NYA_TYPE_U64" },
        { "s8", "NYA_TYPE_S8" },     { "s16", "NYA_TYPE_S16" }, { "s32", "NYA_TYPE_S32" }, { "s64", "NYA_TYPE_S64" },
        { "f32", "NYA_TYPE_F32" },   { "f64", "NYA_TYPE_F64" },  { "char", "NYA_TYPE_CHAR" },
    };

    nya_string_extend(out, "/* ── primitives ── */\n\n");

    for (u64 i = 0; i < sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0]); i++) {
        nya_string_extend_sprintf(out,
                                  "const NYA_TypeReflection _NYA_REFLECT_%s = { .name = \"%s\", .kind = NYA_REFLECT_PRIMITIVE, "
                                  ".size = sizeof(%s), .alignment = alignof(%s), .primitive = %s };\n",
                                  PRIMITIVES[i].name, PRIMITIVES[i].name, PRIMITIVES[i].name, PRIMITIVES[i].name,
                                  PRIMITIVES[i].primitive);
    }

    // A string field is a pointer to characters the struct does not own, which NYA_Value already has
    // a case for, so it is a primitive here rather than a pointer to one.
    nya_string_extend(out,
                      "\nconst NYA_TypeReflection _NYA_REFLECT_string = { .name = \"string\", .kind = NYA_REFLECT_PRIMITIVE, "
                      ".size = sizeof(char*), .alignment = alignof(char*), .primitive = NYA_TYPE_STRING };\n\n");

    nya_string_extend(out, "/* ── vectors ── */\n\n");

    for (u32 count = 2; count <= 4; count++) {
        nya_string_extend_sprintf(out,
                                  "const NYA_TypeReflection _NYA_REFLECT_f32x%u = { .name = \"f32x%u\", .kind = NYA_REFLECT_VECTOR, "
                                  ".size = sizeof(f32x%u), .alignment = alignof(f32x%u), .element = &_NYA_REFLECT_f32, "
                                  ".element_count = %u };\n",
                                  count, count, count, count, count);
    }

    nya_string_extend(out, "\n");
}

/**
 * Reads one member declaration, from `start` up to its terminating semicolon.
 *
 * Handles the four shapes that actually appear: `T name;`, `T* name;`, `T name[N];` and the
 * multi-declarator `f32 r, g, b, a;` that NYA_Color is written with. The base type is every token
 * before the first identifier that is followed by `,`, `;` or `[` — which is what separates
 * `const char* text` from `NYA_Color color` without knowing what either name means.
 *
 * Answers the index just past the semicolon.
 * */
u32 _nya_reflect_parse_members(_NYA_ReflectTypeDecl* decl, const NYA_Lexer* lexer, u32 start, NYA_ConstCString path) {
    u32 index = start;

    // Find where the declarators begin.
    u32 first_declarator = index;

    while (first_declarator < lexer->tokens->length) {
        NYA_Token token = lexer->tokens->items[first_declarator];

        if (token.type == NYA_TOKEN_EOF) return first_declarator;

        if (token.type == NYA_TOKEN_IDENT) {
            NYA_Token next = lexer->tokens->items[first_declarator + 1];

            if (next.type == NYA_TOKEN_SYMBOL && (next.symbol == ';' || next.symbol == ',' || next.symbol == '[')) break;
        }

        first_declarator++;
    }

    if (first_declarator >= lexer->tokens->length) return first_declarator;

    // The base type is everything before it, with any pointer stars counted rather than kept.
    char base[NYA_REFLECT_MAX_NAME] = { 0 };
    u32  pointer_depth              = 0;

    for (u32 i = index; i < first_declarator; i++) {
        NYA_Token token = lexer->tokens->items[i];

        if (token.type == NYA_TOKEN_SYMBOL && token.symbol == '*') {
            pointer_depth++;
            continue;
        }

        if (token.type != NYA_TOKEN_IDENT) continue;

        char piece[NYA_REFLECT_MAX_NAME] = { 0 };
        _nya_reflect_token_copy(lexer, i, piece, sizeof(piece));

        // `const`, `struct`, `enum` and `unsigned` are noise for our purposes: what is wanted is the
        // spelling a reflection symbol is named after.
        if (nya_string_equals(piece, "const") || nya_string_equals(piece, "struct") || nya_string_equals(piece, "enum") ||
            nya_string_equals(piece, "union") || nya_string_equals(piece, "volatile")) {
            continue;
        }

        (void)snprintf(base, sizeof(base), "%s", piece);
    }

    index = first_declarator;

    // Then each declarator in turn, sharing that base type.
    while (index < lexer->tokens->length) {
        NYA_Token token = lexer->tokens->items[index];

        if (token.type != NYA_TOKEN_IDENT) break;

        _NYA_ReflectFieldDecl field = { .pointer_depth = pointer_depth, .hint = "NYA_HINT_NONE" };

        _nya_reflect_token_copy(lexer, index, field.name, sizeof(field.name));
        (void)snprintf(field.type_spelling, sizeof(field.type_spelling), "%s", base);

        index++;

        // A star bound to this declarator rather than to the base type, as in `T *a, b;`.
        // Not supported deliberately: it means a and b have different types, and a struct written
        // that way is a struct that wants rewriting more than it wants reflecting.

        if (index < lexer->tokens->length && lexer->tokens->items[index].type == NYA_TOKEN_SYMBOL &&
            lexer->tokens->items[index].symbol == '[') {
            index++;

            u32 extent_start = index;

            while (index < lexer->tokens->length &&
                   !(lexer->tokens->items[index].type == NYA_TOKEN_SYMBOL && lexer->tokens->items[index].symbol == ']')) {
                index++;
            }

            /*
             * The extent is copied as *source text*, spaces and all.
             *
             * `char name[NYA_NET_MAX_NAME]` must not require this generator to know what that macro
             * expands to. Emitting the text lets the compiler answer, which is the same trick the
             * offsets use.
             */
            if (index > extent_start) {
                NYA_Token first = lexer->tokens->items[extent_start];
                NYA_Token last  = lexer->tokens->items[index - 1];

                u64 length = (last.source_location + last.length) - first.source_location;

                if (length < sizeof(field.array_extent)) {
                    nya_memcpy(field.array_extent, lexer->source + first.source_location, length);
                    field.array_extent[length] = '\0';
                }
            }

            index++;
        }

        // A trailing comment on the same line carries this field's annotations.
        b8 skipped = false;

        for (u32 look = index; look < lexer->tokens->length && look < index + 3; look++) {
            if (lexer->tokens->items[look].type != NYA_TOKEN_COMMENT) continue;
            if (lexer->tokens->items[look].line_number != token.line_number) continue;

            if (_nya_reflect_comment_has(lexer, look, "@skip")) skipped = true;

            field.hint = _nya_reflect_hint_from_comment(lexer, look);

            if (!_nya_reflect_annotation_argument(lexer, look, "@flags", field.enum_override, sizeof(field.enum_override))) {
                (void)_nya_reflect_annotation_argument(lexer, look, "@enum", field.enum_override, sizeof(field.enum_override));
            } else {
                field.hint = "NYA_HINT_BITFLAGS";
            }

            break;
        }

        if (!skipped && base[0] != '\0') {
            nya_assert(decl->field_count < NYA_REFLECT_MAX_FIELDS, "%s: '%s' has more than %d fields", path, decl->name,
                       NYA_REFLECT_MAX_FIELDS);

            decl->fields[decl->field_count] = field;
            decl->field_count++;
        }

        if (index < lexer->tokens->length && lexer->tokens->items[index].type == NYA_TOKEN_SYMBOL &&
            lexer->tokens->items[index].symbol == ',') {
            index++;
            continue;
        }

        break;
    }

    // Step over the semicolon this declaration ends with.
    while (index < lexer->tokens->length) {
        NYA_Token token = lexer->tokens->items[index];

        if (token.type == NYA_TOKEN_EOF) break;
        if (token.type == NYA_TOKEN_SYMBOL && token.symbol == ';') {
            index++;
            break;
        }

        index++;
    }

    return index;
}

/** Reads the variants of an enum body, from just after the opening brace. */
u32 _nya_reflect_parse_variants(_NYA_ReflectTypeDecl* decl, const NYA_Lexer* lexer, u32 start, NYA_ConstCString path) {
    u32 index = start;

    while (index < lexer->tokens->length) {
        NYA_Token token = lexer->tokens->items[index];

        if (token.type == NYA_TOKEN_EOF) break;
        if (token.type == NYA_TOKEN_COMMENT) {
            index++;
            continue;
        }
        if (token.type == NYA_TOKEN_SYMBOL && token.symbol == '}') break;
        if (token.type == NYA_TOKEN_SYMBOL && token.symbol == ',') {
            index++;
            continue;
        }

        if (token.type != NYA_TOKEN_IDENT) {
            index++;
            continue;
        }

        _NYA_ReflectVariantDecl variant = { 0 };
        _nya_reflect_token_copy(lexer, index, variant.name, sizeof(variant.name));

        index++;

        if (index < lexer->tokens->length && lexer->tokens->items[index].type == NYA_TOKEN_SYMBOL &&
            lexer->tokens->items[index].symbol == '=') {
            index++;

            u32 expression_start = index;

            while (index < lexer->tokens->length) {
                NYA_Token inner = lexer->tokens->items[index];

                if (inner.type == NYA_TOKEN_EOF) break;
                if (inner.type == NYA_TOKEN_SYMBOL && (inner.symbol == ',' || inner.symbol == '}')) break;

                // A shifted initializer is what a set of flags looks like, and the only signal in the
                // source that says so. An explicit @bitflags on the type overrides nothing, it just
                // agrees.
                if (inner.type == NYA_TOKEN_SYMBOL && inner.symbol == '<') decl->is_bitflags = true;

                index++;
            }

            if (index > expression_start) {
                NYA_Token first = lexer->tokens->items[expression_start];
                NYA_Token last  = lexer->tokens->items[index - 1];

                u64 length = (last.source_location + last.length) - first.source_location;

                if (length < sizeof(variant.value_expression)) {
                    nya_memcpy(variant.value_expression, lexer->source + first.source_location, length);
                    variant.value_expression[length] = '\0';
                    variant.has_value                = true;
                }
            }
        }

        nya_assert(decl->variant_count < NYA_REFLECT_MAX_VARIANTS, "%s: '%s' has more than %d variants", path, decl->name,
                   NYA_REFLECT_MAX_VARIANTS);

        decl->variants[decl->variant_count] = variant;
        decl->variant_count++;
    }

    return index;
}

void _nya_reflect_scan_file(_NYA_ReflectSet* set, NYA_ConstCString path) {
    NYA_String* contents = nya_string_create(set->arena);

    // Loud rather than skipped: a header that cannot be read is a header whose annotations silently
    // vanish, and the generated tables would simply be missing types with nothing to say why.
    NYA_EXPECT(nya_file_read(path, contents), "while reading a header to scan for annotations");

    NYA_ConstCString source = nya_string_to_cstring(set->arena, contents);

    // UTF-8 identifiers, because this codebase's derived container types mangle their names with
    // non-ASCII brackets and a name that comes apart mid-scan would confuse the declarator search.
    NYA_Lexer lexer = nya_lexer_create(source, NYA_LEXER_UTF8_IDENTS);
    nya_lexer_run(&lexer);

    defer nya_lexer_destroy(&lexer);

    for (u32 index = 0; index < lexer.tokens->length; index++) {
        if (!_nya_reflect_comment_has(&lexer, index, NYA_REFLECT_MARKER)) continue;

        _NYA_ReflectTypeDecl decl = { 0 };
        (void)snprintf(decl.source_file, sizeof(decl.source_file), "%s", path);

        (void)_nya_reflect_annotation_argument(&lexer, index, "@tag", decl.tag_field, sizeof(decl.tag_field));
        (void)_nya_reflect_annotation_argument(&lexer, index, "@on_apply", decl.on_apply, sizeof(decl.on_apply));

        if (_nya_reflect_comment_has(&lexer, index, "@bitflags")) decl.is_bitflags = true;

        u32 cursor = index + 1;

        // Any further comment lines between the annotation and the declaration are skipped, so the
        // marker may sit at the top of a long doc block rather than immediately above the type.
        while (cursor < lexer.tokens->length && lexer.tokens->items[cursor].type == NYA_TOKEN_COMMENT) cursor++;

        b8 is_typedef = _nya_reflect_token_is(&lexer, cursor, "typedef");
        if (is_typedef) cursor++;

        if (_nya_reflect_token_is(&lexer, cursor, "struct")) {
            decl.kind = _NYA_REFLECT_DECL_STRUCT;
        } else if (_nya_reflect_token_is(&lexer, cursor, "union")) {
            decl.kind = _NYA_REFLECT_DECL_UNION;
        } else if (_nya_reflect_token_is(&lexer, cursor, "enum")) {
            decl.kind = _NYA_REFLECT_DECL_ENUM;
        } else {
            nya_log_warn("%s:%u: @reflect is not above a struct, union or enum; ignored.", path,
                     lexer.tokens->items[index].line_number);
            continue;
        }

        cursor++;

        // `struct Name {` names the type here; `typedef struct {` names it after the closing brace.
        char tag_name[NYA_REFLECT_MAX_NAME] = { 0 };

        if (cursor < lexer.tokens->length && lexer.tokens->items[cursor].type == NYA_TOKEN_IDENT) {
            _nya_reflect_token_copy(&lexer, cursor, tag_name, sizeof(tag_name));
            cursor++;
        }

        /*
         * An explicit underlying type, as in `enum GNY_EntityFlags : u64 {`.
         *
         * C23 spelling, which this codebase uses for any flag set that needs more than an int. It is
         * stepped over rather than recorded: the width still comes from sizeof in the emitted table,
         * which stays correct even for an enum that does not say.
         */
        if (cursor < lexer.tokens->length && lexer.tokens->items[cursor].type == NYA_TOKEN_SYMBOL &&
            lexer.tokens->items[cursor].symbol == ':') {
            while (cursor < lexer.tokens->length &&
                   !(lexer.tokens->items[cursor].type == NYA_TOKEN_SYMBOL && lexer.tokens->items[cursor].symbol == '{')) {
                cursor++;
            }
        }

        if (!(cursor < lexer.tokens->length && lexer.tokens->items[cursor].type == NYA_TOKEN_SYMBOL &&
              lexer.tokens->items[cursor].symbol == '{')) {
            nya_log_warn("%s:%u: @reflect on a declaration with no body; ignored.", path, lexer.tokens->items[index].line_number);
            continue;
        }

        cursor++;

        if (decl.kind == _NYA_REFLECT_DECL_ENUM) {
            cursor = _nya_reflect_parse_variants(&decl, &lexer, cursor, path);
        } else {
            // Members until the closing brace. Comments between them are the per-field annotations,
            // which the member parser looks back at rather than consuming here.
            while (cursor < lexer.tokens->length) {
                NYA_Token token = lexer.tokens->items[cursor];

                if (token.type == NYA_TOKEN_EOF) break;
                if (token.type == NYA_TOKEN_SYMBOL && token.symbol == '}') break;
                if (token.type == NYA_TOKEN_COMMENT) {
                    cursor++;
                    continue;
                }

                u32 next = _nya_reflect_parse_members(&decl, &lexer, cursor, path);

                // No progress means something unparseable; step over it rather than spinning.
                if (next <= cursor) {
                    cursor++;
                    continue;
                }

                cursor = next;
            }
        }

        // Past the closing brace, then the typedef name if there is one.
        while (cursor < lexer.tokens->length &&
               !(lexer.tokens->items[cursor].type == NYA_TOKEN_SYMBOL && lexer.tokens->items[cursor].symbol == '}')) {
            cursor++;
        }
        cursor++;

        if (is_typedef && cursor < lexer.tokens->length && lexer.tokens->items[cursor].type == NYA_TOKEN_IDENT) {
            _nya_reflect_token_copy(&lexer, cursor, decl.name, sizeof(decl.name));
        } else {
            (void)snprintf(decl.name, sizeof(decl.name), "%s", tag_name);
        }

        if (decl.name[0] == '\0') {
            nya_log_warn("%s:%u: @reflect on an anonymous type, which nothing could reference; ignored.", path,
                     lexer.tokens->items[index].line_number);
            continue;
        }

        if (_nya_reflect_is_known(set, decl.name)) {
            nya_log_warn("%s: '%s' is annotated more than once; the later one is ignored.", path, decl.name);
            continue;
        }

        nya_assert(set->type_count < NYA_REFLECT_MAX_TYPES, "more than %d annotated types", NYA_REFLECT_MAX_TYPES);

        set->types[set->type_count] = decl;
        set->type_count++;
    }
}

/** The symbol a field's type resolves to, or null when nothing describes it. */
NYA_ConstCString _nya_reflect_field_symbol(const _NYA_ReflectSet* set, const _NYA_ReflectFieldDecl* field, OUT char* buffer,
                                           u64 capacity) {
    // An explicit @enum or @flags wins: the field is an integer and only the annotation knows which
    // enum's names belong to it.
    if (field->enum_override[0] != '\0') {
        (void)snprintf(buffer, capacity, "_NYA_REFLECT_%s", field->enum_override);
        return buffer;
    }

    // A pointer to characters is a string, which is a primitive here. Any other pointer is not
    // followed; see nya_reflect_to_object.
    if (field->pointer_depth > 0) {
        if (nya_string_equals(field->type_spelling, "char")) return "_NYA_REFLECT_string";

        return nullptr;
    }

    NYA_ConstCString builtin = _nya_reflect_builtin_symbol(field->type_spelling);
    if (builtin != nullptr) return builtin;

    if (_nya_reflect_is_known(set, field->type_spelling)) {
        (void)snprintf(buffer, capacity, "_NYA_REFLECT_%s", field->type_spelling);
        return buffer;
    }

    return nullptr;
}

void _nya_reflect_emit_type(const _NYA_ReflectSet* set, NYA_String* out, const _NYA_ReflectTypeDecl* decl) {
    nya_string_extend_sprintf(out, "/* %s — %s */\n\n", decl->name, decl->source_file);

    if (decl->kind == _NYA_REFLECT_DECL_ENUM) {
        nya_string_extend_sprintf(out, "static const NYA_ReflectVariant _NYA_REFLECT_%s_VARIANTS[] = {\n", decl->name);

        for (u32 i = 0; i < decl->variant_count; i++) {
            // The variant's own name is the expression: whatever the compiler decided it equals,
            // including a shift the generator never evaluated.
            nya_string_extend_sprintf(out, "    { .name = \"%s\", .value = (s64)(%s) },\n", decl->variants[i].name,
                                      decl->variants[i].name);
        }

        nya_string_extend(out, "};\n\n");

        nya_string_extend_sprintf(out,
                                  "const NYA_TypeReflection _NYA_REFLECT_%s = {\n"
                                  "    .name = \"%s\",\n"
                                  "    .kind = NYA_REFLECT_ENUM,\n"
                                  "    .size = sizeof(%s),\n"
                                  "    .alignment = alignof(%s),\n"
                                  /*
                                   * The underlying integer is chosen by size, as a constant expression.
                                   *
                                   * C lets the implementation pick the width of an enum, and a set of
                                   * flags written with 1ULL is a different width from a plain one. A
                                   * ternary over sizeof is constant folded, so the compiler answers a
                                   * question the generator cannot.
                                   */
                                  "    .primitive = (sizeof(%s) == 8 ? NYA_TYPE_S64\n"
                                  "                : sizeof(%s) == 2 ? NYA_TYPE_S16\n"
                                  "                : sizeof(%s) == 1 ? NYA_TYPE_S8\n"
                                  "                                  : NYA_TYPE_S32),\n"
                                  "    .variants = _NYA_REFLECT_%s_VARIANTS,\n"
                                  "    .variant_count = %u,\n"
                                  "    .is_bitflags = %s,\n"
                                  "};\n\n",
                                  decl->name, decl->name, decl->name, decl->name, decl->name, decl->name, decl->name,
                                  decl->name, decl->variant_count, decl->is_bitflags ? "true" : "false");

        return;
    }

    /*
     * Array wrappers are synthesised per field rather than deduplicated.
     *
     * Two fields of type `char[32]` produce two identical descriptions, which costs a few dozen bytes
     * of read only data and saves the generator a uniquing pass over spellings that would have to
     * agree textually to be merged anyway.
     */
    for (u32 i = 0; i < decl->field_count; i++) {
        const _NYA_ReflectFieldDecl* field = &decl->fields[i];

        if (field->array_extent[0] == '\0') continue;

        char buffer[NYA_REFLECT_MAX_NAME * 2] = { 0 };

        NYA_ConstCString element = _nya_reflect_field_symbol(set, field, buffer, sizeof(buffer));
        if (element == nullptr) continue;

        nya_string_extend_sprintf(out,
                                  "static const NYA_TypeReflection _NYA_REFLECT_%s_%s_ARRAY = {\n"
                                  "    .name = \"%s[]\", .kind = NYA_REFLECT_ARRAY,\n"
                                  "    .size = sizeof(((%s*)nullptr)->%s),\n"
                                  "    .alignment = alignof(%s),\n"
                                  "    .element = &%s, .element_count = (%s),\n"
                                  "};\n\n",
                                  decl->name, field->name, field->type_spelling, decl->name, field->name,
                                  field->type_spelling, element, field->array_extent);
    }

    nya_string_extend_sprintf(out, "static const NYA_ReflectField _NYA_REFLECT_%s_FIELDS[] = {\n", decl->name);

    u32 emitted = 0;

    for (u32 i = 0; i < decl->field_count; i++) {
        const _NYA_ReflectFieldDecl* field = &decl->fields[i];

        char             buffer[NYA_REFLECT_MAX_NAME * 2] = { 0 };
        NYA_ConstCString symbol                           = nullptr;

        if (field->array_extent[0] != '\0') {
            NYA_ConstCString element = _nya_reflect_field_symbol(set, field, buffer, sizeof(buffer));

            if (element != nullptr) {
                char array_symbol[NYA_REFLECT_MAX_NAME * 2] = { 0 };
                (void)snprintf(array_symbol, sizeof(array_symbol), "_NYA_REFLECT_%s_%s_ARRAY", decl->name, field->name);

                nya_string_extend_sprintf(out,
                                          "    { .name = \"%s\", .type = &%s, .offset = nya_offsetof(%s, %s), .hint = %s },\n",
                                          field->name, array_symbol, decl->name, field->name, field->hint);
                emitted++;
            } else {
                nya_log_warn("%s: '%s.%s' has undescribed element type '%s'; skipped. Add @reflect to it, or @skip to the field.",
                         decl->source_file, decl->name, field->name, field->type_spelling);
            }

            continue;
        }

        symbol = _nya_reflect_field_symbol(set, field, buffer, sizeof(buffer));

        if (symbol == nullptr) {
            // Named rather than silently dropped: a forgotten @reflect on a nested struct is the
            // usual cause, and it would otherwise show up as a field mysteriously missing from an
            // editor days later.
            if (field->pointer_depth == 0) {
                nya_log_warn("%s: '%s.%s' has undescribed type '%s'; skipped. Add @reflect to it, or @skip to the field.",
                         decl->source_file, decl->name, field->name, field->type_spelling);
            }

            continue;
        }

        nya_string_extend_sprintf(out, "    { .name = \"%s\", .type = &%s, .offset = nya_offsetof(%s, %s), .hint = %s },\n",
                                  field->name, symbol, decl->name, field->name, field->hint);
        emitted++;
    }

    // A struct every one of whose fields was skipped still needs a valid array: a zero length one is
    // not legal C, so a placeholder keeps the table well formed and the count honest at zero.
    if (emitted == 0) nya_string_extend(out, "    { .name = nullptr, .type = nullptr, .offset = 0 },\n");

    nya_string_extend(out, "};\n\n");

    nya_string_extend_sprintf(out,
                              "const NYA_TypeReflection _NYA_REFLECT_%s = {\n"
                              "    .name = \"%s\",\n"
                              "    .kind = %s,\n"
                              "    .size = sizeof(%s),\n"
                              "    .alignment = alignof(%s),\n"
                              "    .fields = _NYA_REFLECT_%s_FIELDS,\n"
                              "    .field_count = %u,\n",
                              decl->name, decl->name, decl->kind == _NYA_REFLECT_DECL_UNION ? "NYA_REFLECT_UNION" : "NYA_REFLECT_STRUCT",
                              decl->name, decl->name, decl->name, emitted);

    if (decl->on_apply[0] != '\0') nya_string_extend_sprintf(out, "    .on_apply = %s,\n", decl->on_apply);

    nya_string_extend(out, "};\n\n");
}
