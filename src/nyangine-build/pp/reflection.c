#include "nyangine-core/nyangine.h"

#include "nyangine-build/build.h"

/* PRIVATE API DECLARATION */

typedef enum {
    _NYA_REFLECT_DECL_STRUCT,
    _NYA_REFLECT_DECL_UNION,
    _NYA_REFLECT_DECL_ENUM,
} _NYA_ReflectDeclKind;

/** One parsed `@name` or `@name(args)`, before it becomes an NYA_ReflectAttribute in the generated table. */
typedef struct {
    char name[NYA_REFLECT_MAX_ATTRIBUTE_NAME];
    char args[NYA_REFLECT_MAX_ATTRIBUTE_ARGS];
    b8   has_args;
} _NYA_ReflectAttributeDecl;

typedef struct {
    char name[NYA_REFLECT_MAX_NAME];

    /** The type as written, minus pointers and array brackets: "f32", "NYA_Color". */
    char type_spelling[NYA_REFLECT_MAX_NAME];

    /** The text between the brackets, verbatim, or empty. Emitted as-is for the compiler to evaluate. */
    char array_extent[NYA_REFLECT_MAX_NAME];

    /** From `@enum(X)` or `@flags(X)`: describe this field as that enum rather than as its integer. */
    char enum_override[NYA_REFLECT_MAX_NAME];

    u32 pointer_depth;

    /** From `@key`: the field is the type's primary key. Read by the sqlite ORM; see orm.h. */
    b8 is_key;

    /** From `@redact`: the field is a secret. Read by nya_reflect_to_object_redacted, which every
     *  logging path goes through; see base_reflection.h. */
    b8 is_redacted;

    /** From `@secret`: the field is written encrypted and read back decrypted. Acted on by the
     *  reflected save path; see serde_reflect.h. Masked in logs too, like `@redact`. */
    b8 is_secret;

    NYA_ConstCString hint;

    /** Every `@name`/`@name(args)` on the field, the typed ones above included. See NYA_ReflectAttribute. */
    _NYA_ReflectAttributeDecl attributes[NYA_REFLECT_MAX_ATTRIBUTES];
    u32                       attribute_count;
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

    /** Every `@name`/`@name(args)` on the type's marker comment, minus `@reflect` itself. */
    _NYA_ReflectAttributeDecl attributes[NYA_REFLECT_MAX_ATTRIBUTES];
    u32                       attribute_count;

    char source_file[512];
} _NYA_ReflectTypeDecl;

typedef struct {
    _NYA_ReflectTypeDecl* types;
    u32                   type_count;

    /**
     * Where the engine's types end and the game's begin. The engine tree is scanned first, so
     * `types[0 .. engine_type_count)` is what goes into the engine's generated pair and the rest into
     * the game's. A field of an engine type may only resolve against the first part; see the `limit`
     * argument threaded through the emitters.
     * */
    u32 engine_type_count;

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

/**
 * Parses every `@name` / `@name(args)` in the comment at `index` into `attributes`, appending to the
 * `count` already there. `exclude` is a bare name skipped when met (the `@reflect` marker on a type), or
 * null. `owner` names the type or field for a diagnostic. Bounded: extra attributes past the cap and a
 * malformed `@name(` with no closing paren are dropped with a warning rather than crashing the build.
 * */
NYA_INTERNAL void _nya_reflect_collect_attributes(const NYA_Lexer* lexer, u32 index, OUT _NYA_ReflectAttributeDecl* attributes,
                                                  u32* count, NYA_ConstCString exclude, NYA_ConstCString path, u32 line,
                                                  NYA_ConstCString owner);

/** Whether the field's type resolves to a description, i.e. whether _nya_reflect_emit_type will emit it. */
NYA_INTERNAL b8 _nya_reflect_field_emits(const _NYA_ReflectSet* set, const _NYA_ReflectFieldDecl* field, u32 limit);

/** Emits one `static const NYA_ReflectAttribute NAME[] = { ... };` table, or nothing when `count` is zero. */
NYA_INTERNAL void _nya_reflect_emit_attributes(NYA_String* out, NYA_ConstCString symbol, const _NYA_ReflectAttributeDecl* attributes, u32 count);

/** Whether `name` is annotated within the first `limit` types. See _NYA_ReflectSet.engine_type_count. */
NYA_INTERNAL b8   _nya_reflect_is_known(const _NYA_ReflectSet* set, NYA_ConstCString name, u32 limit);
NYA_INTERNAL s32  _nya_reflect_compare_paths(const NYA_String* a, const NYA_String* b);
NYA_INTERNAL void _nya_reflect_emit_builtin_declarations(NYA_String* out);
NYA_INTERNAL void _nya_reflect_emit_builtins(NYA_String* out);
NYA_INTERNAL void _nya_reflect_emit_type(const _NYA_ReflectSet* set, NYA_String* out, const _NYA_ReflectTypeDecl* decl, u32 limit);
NYA_INTERNAL u32  _nya_reflect_parse_members(_NYA_ReflectTypeDecl* decl, const NYA_Lexer* lexer, u32 start, NYA_ConstCString path);
NYA_INTERNAL u32  _nya_reflect_parse_variants(_NYA_ReflectTypeDecl* decl, const NYA_Lexer* lexer, u32 start, NYA_ConstCString path);
NYA_INTERNAL NYA_ConstCString
_nya_reflect_field_symbol(const _NYA_ReflectSet* set, const _NYA_ReflectFieldDecl* field, u32 limit, OUT char* buffer, u64 capacity);

/** Walks one tree, sorted, and scans every header in it into `set`. */
NYA_INTERNAL void _nya_reflect_scan_tree(_NYA_ReflectSet* set, NYA_ConstCString directory);

/**
 * Whether a type's source file sits in a module that names SDL, the renderer or core, and so whose
 * reflection cannot compile without the SDL graph. Everything else under src/nyangine is the
 * server-safe floor. See docs/layering-core-split.md: this is what routes a type's definition to
 * reflection_engine.c (SDL-bound) or reflection_engine_server.c (compiled by a headless build too).
 * */
NYA_INTERNAL b8 _nya_reflect_is_sdl_bound(NYA_ConstCString source_file);

/**
 * Whether a server-safe type's struct is only in scope when the db module is on — its header sits
 * behind NYA_MODULE_DB in nyangine.h — so its reflection has to sit behind the same flag.
 * */
NYA_INTERNAL b8 _nya_reflect_is_db_module(NYA_ConstCString source_file);

/* PUBLIC API IMPLEMENTATION */

void nya_reflection_generate(void) {
    NYA_ConstCString inputs[] = {
        NYA_REFLECT_ENGINE_STD,
        NYA_REFLECT_ENGINE_CORE,
        NYA_REFLECT_ENGINE_UI,
        NYA_REFLECT_ENGINE_PLUGINS,
        NYA_REFLECT_GAME_DIRECTORY,
        "./src/nyangine-build/pp/reflection.c",
        nullptr,
    };
    NYA_ConstCString outputs[] = {
        NYA_REFLECT_OUTPUT_ENGINE_HEADER, NYA_REFLECT_OUTPUT_ENGINE_SOURCE, NYA_REFLECT_OUTPUT_ENGINE_SERVER_SOURCE,
        NYA_REFLECT_OUTPUT_HEADER,        NYA_REFLECT_OUTPUT_SOURCE,        nullptr,
    };
    if (nya_pp_is_current("generate_reflection", inputs, outputs)) return;

    NYA_Arena* arena = nya_arena_create(.name = "reflection_generate");
    defer      nya_arena_destroy(arena);

    _NYA_ReflectSet set = {
        .arena = arena,
        .types = nya_arena_alloc(arena, sizeof(_NYA_ReflectTypeDecl) * NYA_REFLECT_MAX_TYPES),
    };

    nya_memset(set.types, 0, sizeof(_NYA_ReflectTypeDecl) * NYA_REFLECT_MAX_TYPES);

    // The engine tree first, so its types land in the first part of the set and a game type can never end up as a field of an engine type's description. See _NYA_ReflectSet.engine_type_count.
    _nya_reflect_scan_tree(&set, NYA_REFLECT_ENGINE_STD);
    _nya_reflect_scan_tree(&set, NYA_REFLECT_ENGINE_CORE);
    _nya_reflect_scan_tree(&set, NYA_REFLECT_ENGINE_UI);
    _nya_reflect_scan_tree(&set, NYA_REFLECT_ENGINE_PLUGINS);
    set.engine_type_count = set.type_count;

    _nya_reflect_scan_tree(&set, NYA_REFLECT_GAME_DIRECTORY);

    nya_log_info("nya_reflection_generate: %u annotated types, %u of them the engine's.", set.type_count, set.engine_type_count);

    // the engine header
    NYA_String* engine_header = nya_string_create(arena);

    nya_string_extend(engine_header, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n#pragma once\n\n");
    nya_string_extend(engine_header, "#include \"nyangine-std/base/base_reflection.h\"\n\n");
    nya_string_extend(engine_header,
                      "/*\n"
                      " * Generated by src/nyangine-build/pp/reflection.c from the @reflect annotations under src/nyangine.\n"
                      " *\n"
                      " * Reach one of these through nya_reflect_of(TypeName) rather than by naming the symbol: the\n"
                      " * macro is what makes a misspelling a link error instead of a null at runtime.\n"
                      " */\n\n");

    _nya_reflect_emit_builtin_declarations(engine_header);

    for (u32 i = 0; i < set.engine_type_count; i++) {
        nya_string_extend_sprintf(engine_header, "extern const NYA_TypeReflection _NYA_REFLECT_%s;\n", set.types[i].name);
    }

    nya_string_extend(engine_header, "\n/** Every annotated engine type. The game's are in genyarated/reflection.h. */\n");
    nya_string_extend_sprintf(engine_header, "#define NYA_REFLECT_ENGINE_TYPE_COUNT %u\n\n", set.engine_type_count);
    nya_string_extend(engine_header, "extern const NYA_TypeReflection* const NYA_REFLECT_ENGINE_TYPES[NYA_REFLECT_ENGINE_TYPE_COUNT];\n");

    NYA_EXPECT(nya_file_write(NYA_REFLECT_OUTPUT_ENGINE_HEADER, engine_header), "while writing the generated engine reflection header");

    // the engine server source — The server-safe half: the builtins and every engine type in a module a headless build compiles. Included from nyangine.c inside the NYA_SERVER seam, so a server binary has these descriptions without ever compiling the SDL-bound half below. The builtins live here rather than in the SDL source because both a full build and a headless one need them and a definition in each would be a duplicate symbol; a full build compiles this file too (the seam is true whenever SDL is present), so it is the one place they are defined. See docs/layering-core-split.md.
    NYA_String* server_source = nya_string_create(arena);

    nya_string_extend(server_source, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(server_source, "#include \"nyangine-core/nyangine.h\"\n\n");
    nya_string_extend(server_source, "#include \"genyarated/reflection_engine.h\"\n\n");
    nya_string_extend(server_source,
                      "/*\n"
                      " * The server-safe engine reflections: the builtins and the annotated types in modules a\n"
                      " * headless (NYA_NO_SDL + NYA_SERVER) build compiles. The SDL-bound ones are in\n"
                      " * reflection_engine.c. Every size and offset is an expression, so the compiler already\n"
                      " * compiling these structs computes the layout. See src/nyangine-build/pp/reflection.h.\n"
                      " */\n\n");

    _nya_reflect_emit_builtins(server_source);

    // The unguarded server-safe types first, then the db-module ones behind NYA_MODULE_DB — the same flag their headers sit behind in nyangine.h, so a headless build without db still compiles this file.
    for (u32 i = 0; i < set.engine_type_count; i++) {
        if (_nya_reflect_is_sdl_bound(set.types[i].source_file) || _nya_reflect_is_db_module(set.types[i].source_file)) continue;
        _nya_reflect_emit_type(&set, server_source, &set.types[i], set.engine_type_count);
    }

    nya_string_extend(server_source, "#ifdef NYA_MODULE_DB\n\n");
    for (u32 i = 0; i < set.engine_type_count; i++) {
        if (_nya_reflect_is_sdl_bound(set.types[i].source_file) || !_nya_reflect_is_db_module(set.types[i].source_file)) continue;
        _nya_reflect_emit_type(&set, server_source, &set.types[i], set.engine_type_count);
    }
    nya_string_extend(server_source, "#endif // NYA_MODULE_DB\n");

    NYA_EXPECT(nya_file_write(NYA_REFLECT_OUTPUT_ENGINE_SERVER_SOURCE, server_source), "while writing the generated server reflection source");

    // the engine source — The SDL-bound half: the type descriptions that need the renderer, core, ui or physics graph to compile, plus the NYA_REFLECT_ENGINE_TYPES table over *every* engine type. The table names the server-safe symbols too, which is why this file needs them linked — a full build always compiles reflection_engine_server.c beside it (with the db module on), so they resolve.
    NYA_String* engine_source = nya_string_create(arena);

    nya_string_extend(engine_source, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(engine_source, "#include \"nyangine-core/nyangine.h\"\n\n");
    // Its own header too, so every symbol below is declared before it is defined whatever order the unity build happens to reach this file in.
    nya_string_extend(engine_source, "#include \"genyarated/reflection_engine.h\"\n\n");
    nya_string_extend(engine_source,
                      "/*\n"
                      " * Every size and offset below is an expression rather than a number, so the compiler that is\n"
                      " * already compiling these structs is what computes the layout. See src/nyangine-build/pp/reflection.h.\n"
                      " * The builtins and the server-safe types are in reflection_engine_server.c; see it.\n"
                      " */\n\n");

    for (u32 i = 0; i < set.engine_type_count; i++) {
        if (!_nya_reflect_is_sdl_bound(set.types[i].source_file)) continue;
        _nya_reflect_emit_type(&set, engine_source, &set.types[i], set.engine_type_count);
    }

    nya_string_extend(engine_source, "const NYA_TypeReflection* const NYA_REFLECT_ENGINE_TYPES[NYA_REFLECT_ENGINE_TYPE_COUNT] = {\n");
    for (u32 i = 0; i < set.engine_type_count; i++) {
        nya_string_extend_sprintf(engine_source, "    &_NYA_REFLECT_%s,\n", set.types[i].name);
    }
    nya_string_extend(engine_source, "};\n");

    NYA_EXPECT(nya_file_write(NYA_REFLECT_OUTPUT_ENGINE_SOURCE, engine_source), "while writing the generated engine reflection source");

    // the game header
    NYA_String* header = nya_string_create(arena);

    nya_string_extend(header, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n#pragma once\n\n");
    nya_string_extend(header, "#include \"genyarated/reflection_engine.h\"\n\n");
    nya_string_extend(header,
                      "/*\n"
                      " * Generated by src/nyangine-build/pp/reflection.c from the @reflect annotations under src/gnyame.\n"
                      " *\n"
                      " * Reach one of these through nya_reflect_of(TypeName) rather than by naming the symbol: the\n"
                      " * macro is what makes a misspelling a link error instead of a null at runtime.\n"
                      " */\n\n");

    for (u32 i = set.engine_type_count; i < set.type_count; i++) {
        nya_string_extend_sprintf(header, "extern const NYA_TypeReflection _NYA_REFLECT_%s;\n", set.types[i].name);
    }

    nya_string_extend(header, "\n/** Every annotated type, the engine's and the game's, for an editor that needs to enumerate them. */\n");
    nya_string_extend_sprintf(header, "#define NYA_REFLECT_GAME_TYPE_COUNT %u\n", set.type_count - set.engine_type_count);
    nya_string_extend(header, "#define NYA_REFLECT_TYPE_COUNT      (NYA_REFLECT_ENGINE_TYPE_COUNT + NYA_REFLECT_GAME_TYPE_COUNT)\n\n");
    nya_string_extend(header, "extern const NYA_TypeReflection* const NYA_REFLECT_TYPES[NYA_REFLECT_TYPE_COUNT];\n\n");
    nya_string_extend(header, "/** The type called `name`, or null. Linear: this is an editor path, not a hot one. */\n");
    nya_string_extend(header, "NYA_API const NYA_TypeReflection* nya_reflect_find(NYA_ConstCString name) __attr_no_discard;\n");

    NYA_EXPECT(nya_file_write(NYA_REFLECT_OUTPUT_HEADER, header), "while writing the generated reflection header");

    // the game source
    NYA_String* out = nya_string_create(arena);

    nya_string_extend(out, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n");
    nya_string_extend(out, "#include \"nyangine-core/nyangine.h\"\n\n");
    nya_string_extend(out, "#include \"genyarated/reflection.h\"\n\n");
    nya_string_extend(out,
                      "/*\n"
                      " * Every size and offset below is an expression rather than a number, so the compiler that is\n"
                      " * already compiling these structs is what computes the layout. See src/nyangine-build/pp/reflection.h.\n"
                      " */\n\n");

    for (u32 i = set.engine_type_count; i < set.type_count; i++) {
        _nya_reflect_emit_type(&set, out, &set.types[i], set.type_count);
    }

    // the table and the lookup
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

/* PRIVATE API IMPLEMENTATION */

b8 _nya_reflect_is_sdl_bound(NYA_ConstCString source_file) {
    // A whitelist of the modules that name SDL, the renderer or core, checked against the type's source path. Everything else under src/nyangine is the server-safe floor. The leading and trailing slashes keep this from matching a substring of some longer name.
    return strstr(source_file, "/core/") != nullptr || strstr(source_file, "/renderer/") != nullptr || strstr(source_file, "/ui/") != nullptr ||
           strstr(source_file, "/physics/") != nullptr || strstr(source_file, "/debug/") != nullptr || strstr(source_file, "/replicate/") != nullptr;
}

b8 _nya_reflect_is_db_module(NYA_ConstCString source_file) {
    return strstr(source_file, "/db/") != nullptr || strstr(source_file, "/accounts/") != nullptr;
}

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

    // Headers only. A declaration lives in a header by this codebase's convention, and scanning the .c files as well would double the work to find nothing.
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

    /* Searched, but only at the start of a line within the comment. */
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

/** A character an attribute name is made of: the same set an identifier is. */
NYA_INTERNAL b8 _nya_reflect_attribute_ident_char(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

void _nya_reflect_collect_attributes(const NYA_Lexer* lexer, u32 index, OUT _NYA_ReflectAttributeDecl* attributes, u32* count,
                                     NYA_ConstCString exclude, NYA_ConstCString path, u32 line, NYA_ConstCString owner) {
    if (index >= lexer->tokens->length) return;

    NYA_Token token = lexer->tokens->items[index];
    if (token.type != NYA_TOKEN_COMMENT) return;

    NYA_ConstCString body   = lexer->source + token.source_location;
    u64              length = token.length;

    for (u64 i = 0; i + 1 < length; i++) {
        if (body[i] != '@') continue;

        // The name is the identifier run right after the '@'. A bare '@' is not an attribute.
        u64 name_start = i + 1;
        u64 name_end   = name_start;

        while (name_end < length && _nya_reflect_attribute_ident_char((u8)body[name_end])) name_end++;

        if (name_end == name_start) continue;

        _NYA_ReflectAttributeDecl attribute = { 0 };

        u64 name_length = name_end - name_start;
        if (name_length >= sizeof(attribute.name)) name_length = sizeof(attribute.name) - 1;

        nya_memcpy(attribute.name, body + name_start, name_length);
        attribute.name[name_length] = '\0';

        // An optional `(args)`, the arguments copied verbatim up to the closing paren.
        u64 cursor = name_end;
        while (cursor < length && (body[cursor] == ' ' || body[cursor] == '\t')) cursor++;

        if (cursor < length && body[cursor] == '(') {
            cursor++;

            u64 args_start = cursor;
            while (cursor < length && body[cursor] != ')') cursor++;

            // Malformed rather than a crash: named on the build's output and dropped, the field kept.
            if (cursor >= length) {
                nya_log_warn("%s:%u: '%s' has an attribute @%s( with no closing ')'; the attribute is dropped.", path, line, owner,
                             attribute.name);
                i = name_end - 1;
                continue;
            }

            u64 args_length = cursor - args_start;
            if (args_length >= sizeof(attribute.args)) args_length = sizeof(attribute.args) - 1;

            nya_memcpy(attribute.args, body + args_start, args_length);
            attribute.args[args_length] = '\0';
            attribute.has_args          = true;

            i = cursor;   // resume after the ')', which the loop's i++ steps past
        } else {
            i = name_end - 1;   // resume after the name
        }

        // The marker word is the trigger, not an attribute of what it marks.
        if (exclude != nullptr && nya_string_equals(attribute.name, exclude)) continue;

        // The same annotation written twice is one attribute, so a lookup by name is unambiguous.
        b8 seen = false;
        for (u32 k = 0; k < *count; k++) {
            if (nya_string_equals(attributes[k].name, attribute.name)) {
                seen = true;
                break;
            }
        }
        if (seen) continue;

        if (*count >= NYA_REFLECT_MAX_ATTRIBUTES) {
            nya_log_warn("%s:%u: '%s' has more than %d attributes; @%s and any after it are dropped.", path, line, owner,
                         NYA_REFLECT_MAX_ATTRIBUTES, attribute.name);
            return;
        }

        attributes[*count] = attribute;
        (*count)++;
    }
}

b8 _nya_reflect_is_known(const _NYA_ReflectSet* set, NYA_ConstCString name, u32 limit) {
    for (u32 i = 0; i < limit; i++) {
        if (nya_string_equals(set->types[i].name, name)) return true;
    }

    return false;
}

void _nya_reflect_scan_tree(_NYA_ReflectSet* set, NYA_ConstCString directory) {
    /* Sources are collected and sorted before any of them is read, so the generated file's order is the tree's rather than the filesystem's. */
    NYA_ArrayᐸNYA_Stringᐳ* sources = nya_array_create(set->arena, NYA_String);

    NYA_EXPECT(nya_filesystem_walk(set->arena, directory, _nya_reflect_collect_sources, sources));

    nya_array_sort(sources, _nya_reflect_compare_paths);

    nya_array_foreach (sources, source) {
        _nya_reflect_scan_file(set, nya_string_to_cstring(set->arena, source));
    }
}

/**
 * The primitives that get a reflection of their own. One table, read by both the declaration emitter
 * and the definition emitter, so the two cannot drift apart.
 * */
NYA_INTERNAL const struct {
    NYA_ConstCString name;
    NYA_ConstCString primitive;
} PRIMITIVES[] = {
    { "b8", "NYA_TYPE_B8" },     { "b16", "NYA_TYPE_B16" }, { "b32", "NYA_TYPE_B32" }, { "b64", "NYA_TYPE_B64" },
    { "u8", "NYA_TYPE_U8" },     { "u16", "NYA_TYPE_U16" }, { "u32", "NYA_TYPE_U32" }, { "u64", "NYA_TYPE_U64" },
    { "s8", "NYA_TYPE_S8" },     { "s16", "NYA_TYPE_S16" }, { "s32", "NYA_TYPE_S32" }, { "s64", "NYA_TYPE_S64" },
    { "f32", "NYA_TYPE_F32" },   { "f64", "NYA_TYPE_F64" },  { "char", "NYA_TYPE_CHAR" },
};

/**
 * The externs for the builtins, so the game's generated file can name the same ones the engine's file
 * defines rather than defining a second copy of each.
 * */
void _nya_reflect_emit_builtin_declarations(NYA_String* out) {
    nya_string_extend(out, "/* ── primitives, defined in genyarated/reflection_engine.c ── */\n\n");

    for (u64 i = 0; i < sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0]); i++) {
        nya_string_extend_sprintf(out, "extern const NYA_TypeReflection _NYA_REFLECT_%s;\n", PRIMITIVES[i].name);
    }

    nya_string_extend(out, "extern const NYA_TypeReflection _NYA_REFLECT_string;\n\n");

    for (u32 count = 2; count <= 4; count++) {
        nya_string_extend_sprintf(out, "extern const NYA_TypeReflection _NYA_REFLECT_f32x%u;\n", count);
    }

    nya_string_extend(out, "\n/* ── annotated types ── */\n\n");
}

/** The fixed prelude: one reflection per primitive the tree actually uses, plus the vectors. */
void _nya_reflect_emit_builtins(NYA_String* out) {
    nya_string_extend(out, "/* ── primitives ── */\n\n");

    for (u64 i = 0; i < sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0]); i++) {
        nya_string_extend_sprintf(out,
                                  "const NYA_TypeReflection _NYA_REFLECT_%s = { .name = \"%s\", .kind = NYA_REFLECT_PRIMITIVE, "
                                  ".size = sizeof(%s), .alignment = alignof(%s), .primitive = %s };\n",
                                  PRIMITIVES[i].name, PRIMITIVES[i].name, PRIMITIVES[i].name, PRIMITIVES[i].name,
                                  PRIMITIVES[i].primitive);
    }

    // A string field is a pointer to characters the struct does not own, which NYA_Value already has a case for, so it is a primitive here rather than a pointer to one.
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
 * */
u32 _nya_reflect_parse_members(_NYA_ReflectTypeDecl* decl, const NYA_Lexer* lexer, u32 start, NYA_ConstCString path) {
    u32 index = start;

    // A bitfield has no address, so there is no offsetof to emit and nothing that could describe it. Named rather than dropped in silence; see the limits block in base_reflection.h.
    for (u32 look = index; look < lexer->tokens->length; look++) {
        NYA_Token token = lexer->tokens->items[look];

        if (token.type == NYA_TOKEN_EOF) break;
        if (token.type == NYA_TOKEN_SYMBOL && token.symbol == ';') break;
        if (!(token.type == NYA_TOKEN_SYMBOL && token.symbol == ':')) continue;

        nya_log_warn("%s:%u: '%s' has a bitfield member, which has no address to describe; skipped.", path, token.line_number, decl->name);

        while (look < lexer->tokens->length) {
            NYA_Token end = lexer->tokens->items[look];

            if (end.type == NYA_TOKEN_EOF) return look;
            if (end.type == NYA_TOKEN_SYMBOL && end.symbol == ';') return look + 1;

            look++;
        }

        return look;
    }

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

        // `const`, `struct`, `enum` and `unsigned` are noise for our purposes: what is wanted is the spelling a reflection symbol is named after.
        if (nya_string_equals(piece, "const") || nya_string_equals(piece, "struct") || nya_string_equals(piece, "enum") ||
            nya_string_equals(piece, "union") || nya_string_equals(piece, "volatile")) {
            continue;
        }

        (void)snprintf(base, sizeof(base), "%s", piece);
    }

    index = first_declarator;

    // Then each declarator in turn, sharing that base type.
    u32 declarator_count = 0;

    while (index < lexer->tokens->length) {
        NYA_Token token = lexer->tokens->items[index];

        if (token.type != NYA_TOKEN_IDENT) break;

        declarator_count++;

        // `T *a, b;` gives a and b different types, and the star is bound to the declarator rather than to the base, which these tokens cannot tell apart from `T* a, b;`. Refused with a name rather than described wrongly; a struct written that way wants rewriting.
        if (pointer_depth > 0 && declarator_count > 1) {
            nya_log_warn("%s:%u: '%s' declares several names off one pointer type; only the first is described.", path,
                         token.line_number, decl->name);
            break;
        }

        _NYA_ReflectFieldDecl field = { .pointer_depth = pointer_depth, .hint = "NYA_HINT_NONE" };

        _nya_reflect_token_copy(lexer, index, field.name, sizeof(field.name));
        (void)snprintf(field.type_spelling, sizeof(field.type_spelling), "%s", base);

        index++;

        // A star bound to this declarator rather than to the base type, as in `T *a, b;`. Not supported deliberately: it means a and b have different types, and a struct written that way is a struct that wants rewriting more than it wants reflecting.

        if (index < lexer->tokens->length && lexer->tokens->items[index].type == NYA_TOKEN_SYMBOL &&
            lexer->tokens->items[index].symbol == '[') {
            index++;

            u32 extent_start = index;

            while (index < lexer->tokens->length &&
                   !(lexer->tokens->items[index].type == NYA_TOKEN_SYMBOL && lexer->tokens->items[index].symbol == ']')) {
                index++;
            }

            /* The extent is copied as *source text*, spaces and all. */
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
            if (_nya_reflect_comment_has(lexer, look, "@key")) field.is_key = true;
            if (_nya_reflect_comment_has(lexer, look, "@redact")) field.is_redacted = true;
            if (_nya_reflect_comment_has(lexer, look, "@secret")) field.is_secret = true;

            field.hint = _nya_reflect_hint_from_comment(lexer, look);

            if (!_nya_reflect_annotation_argument(lexer, look, "@flags", field.enum_override, sizeof(field.enum_override))) {
                (void)_nya_reflect_annotation_argument(lexer, look, "@enum", field.enum_override, sizeof(field.enum_override));
            } else {
                field.hint = "NYA_HINT_BITFLAGS";
            }

            // The typed flags above are shortcuts read on hot paths; the same annotations, and any the
            // core has no flag for, land in the generic table so a component can reach them by name.
            _nya_reflect_collect_attributes(lexer, look, field.attributes, &field.attribute_count, nullptr, path, token.line_number,
                                            field.name);

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

    // Loud rather than skipped: a header that cannot be read is a header whose annotations silently vanish, and the generated tables would simply be missing types with nothing to say why.
    NYA_EXPECT(nya_file_read(path, contents), "while reading a header to scan for annotations");

    NYA_ConstCString source = nya_string_to_cstring(set->arena, contents);

    // UTF-8 identifiers, because this codebase's derived container types mangle their names with non-ASCII brackets and a name that comes apart mid-scan would confuse the declarator search.
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

        // Any further comment lines between the annotation and the declaration are skipped, so the marker may sit at the top of a long doc block rather than immediately above the type.
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

        /* An explicit underlying type, as in `enum GNY_EntityFlags : u64 {`. */
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
            // Members until the closing brace. Comments between them are the per-field annotations, which the member parser looks back at rather than consuming here.
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

        // Every annotation on the marker comment becomes a type attribute, `@reflect` itself excepted.
        _nya_reflect_collect_attributes(&lexer, index, decl.attributes, &decl.attribute_count, "reflect", path,
                                        lexer.tokens->items[index].line_number, decl.name);

        if (_nya_reflect_is_known(set, decl.name, set->type_count)) {
            nya_log_warn("%s: '%s' is annotated more than once; the later one is ignored.", path, decl.name);
            continue;
        }

        // A key identifies a row, so a second one identifies nothing. Caught here rather than by the consumer, so the person who wrote the annotation is told while looking at the build output instead of at a database that refuses to open. See orm.h.
        for (u32 first = 0, seen = 0; first < decl.field_count; first++) {
            if (!decl.fields[first].is_key) continue;

            seen++;
            if (seen == 1) continue;

            nya_log_warn("%s: '%s.%s' is a second @key; only the first one is kept.", path, decl.name, decl.fields[first].name);
            decl.fields[first].is_key = false;
        }

        nya_assert(set->type_count < NYA_REFLECT_MAX_TYPES, "more than %d annotated types", NYA_REFLECT_MAX_TYPES);

        set->types[set->type_count] = decl;
        set->type_count++;
    }
}

/** The symbol a field's type resolves to, or null when nothing describes it. */
NYA_ConstCString _nya_reflect_field_symbol(const _NYA_ReflectSet* set, const _NYA_ReflectFieldDecl* field, u32 limit, OUT char* buffer,
                                           u64 capacity) {
    // An explicit @enum or @flags wins: the field is an integer and only the annotation knows which
    // enum's names belong to it. Checked against `limit` like any other named type, so an engine
    // struct cannot be annotated with a game enum it could never link against.
    if (field->enum_override[0] != '\0' && _nya_reflect_is_known(set, field->enum_override, limit)) {
        (void)snprintf(buffer, capacity, "_NYA_REFLECT_%s", field->enum_override);
        return buffer;
    }

    // A pointer to characters is a string, which is a primitive here. Any other pointer is not followed; see nya_reflect_to_object.
    if (field->pointer_depth > 0) {
        if (nya_string_equals(field->type_spelling, "char")) return "_NYA_REFLECT_string";

        return nullptr;
    }

    NYA_ConstCString builtin = _nya_reflect_builtin_symbol(field->type_spelling);
    if (builtin != nullptr) return builtin;

    if (_nya_reflect_is_known(set, field->type_spelling, limit)) {
        (void)snprintf(buffer, capacity, "_NYA_REFLECT_%s", field->type_spelling);
        return buffer;
    }

    return nullptr;
}

b8 _nya_reflect_field_emits(const _NYA_ReflectSet* set, const _NYA_ReflectFieldDecl* field, u32 limit) {
    char buffer[NYA_REFLECT_MAX_NAME * 2] = { 0 };

    // Both a plain field and an array field emit exactly when their (element) type resolves to a symbol.
    return _nya_reflect_field_symbol(set, field, limit, buffer, sizeof(buffer)) != nullptr;
}

void _nya_reflect_emit_attributes(NYA_String* out, NYA_ConstCString symbol, const _NYA_ReflectAttributeDecl* attributes, u32 count) {
    if (count == 0) return;

    nya_string_extend_sprintf(out, "static const NYA_ReflectAttribute %s[] = {\n", symbol);

    for (u32 i = 0; i < count; i++) {
        nya_string_extend_sprintf(out, "    { .name = \"%s\"", attributes[i].name);

        if (attributes[i].has_args) {
            // The argument text is a string literal, so a quote or a backslash in it is escaped.
            nya_string_extend(out, ", .args = \"");

            for (const char* c = attributes[i].args; *c != '\0'; c++) {
                if (*c == '"' || *c == '\\') nya_string_extend(out, "\\");

                char one[2] = { *c, '\0' };
                nya_string_extend(out, one);
            }

            nya_string_extend(out, "\"");
        }

        nya_string_extend(out, " },\n");
    }

    nya_string_extend(out, "};\n\n");
}

void _nya_reflect_emit_type(const _NYA_ReflectSet* set, NYA_String* out, const _NYA_ReflectTypeDecl* decl, u32 limit) {
    nya_string_extend_sprintf(out, "/* %s, %s */\n\n", decl->name, decl->source_file);

    if (decl->kind == _NYA_REFLECT_DECL_ENUM) {
        nya_string_extend_sprintf(out, "static const NYA_ReflectVariant _NYA_REFLECT_%s_VARIANTS[] = {\n", decl->name);

        for (u32 i = 0; i < decl->variant_count; i++) {
            // The variant's own name is the expression: whatever the compiler decided it equals, including a shift the generator never evaluated.
            nya_string_extend_sprintf(out, "    { .name = \"%s\", .value = (s64)(%s) },\n", decl->variants[i].name,
                                      decl->variants[i].name);
        }

        nya_string_extend(out, "};\n\n");

        char enum_attr_symbol[NYA_REFLECT_MAX_NAME * 2] = { 0 };
        (void)snprintf(enum_attr_symbol, sizeof(enum_attr_symbol), "_NYA_REFLECT_%s_ATTRIBUTES", decl->name);
        _nya_reflect_emit_attributes(out, enum_attr_symbol, decl->attributes, decl->attribute_count);

        nya_string_extend_sprintf(out,
                                  "const NYA_TypeReflection _NYA_REFLECT_%s = {\n"
                                  "    .name = \"%s\",\n"
                                  "    .kind = NYA_REFLECT_ENUM,\n"
                                  "    .size = sizeof(%s),\n"
                                  "    .alignment = alignof(%s),\n"
                                  /* The underlying integer is chosen by size, as a constant expression. */
                                  "    .primitive = (sizeof(%s) == 8 ? NYA_TYPE_S64\n"
                                  "                : sizeof(%s) == 2 ? NYA_TYPE_S16\n"
                                  "                : sizeof(%s) == 1 ? NYA_TYPE_S8\n"
                                  "                                  : NYA_TYPE_S32),\n"
                                  "    .variants = _NYA_REFLECT_%s_VARIANTS,\n"
                                  "    .variant_count = %u,\n"
                                  "    .is_bitflags = %s,\n",
                                  decl->name, decl->name, decl->name, decl->name, decl->name, decl->name, decl->name,
                                  decl->name, decl->variant_count, decl->is_bitflags ? "true" : "false");

        if (decl->attribute_count > 0) {
            nya_string_extend_sprintf(out, "    .attributes = %s, .attribute_count = %u,\n", enum_attr_symbol, decl->attribute_count);
        }

        nya_string_extend(out, "};\n\n");

        return;
    }

    /* Array wrappers are synthesised per field rather than deduplicated. */
    for (u32 i = 0; i < decl->field_count; i++) {
        const _NYA_ReflectFieldDecl* field = &decl->fields[i];

        if (field->array_extent[0] == '\0') continue;

        char buffer[NYA_REFLECT_MAX_NAME * 2] = { 0 };

        NYA_ConstCString element = _nya_reflect_field_symbol(set, field, limit, buffer, sizeof(buffer));
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

    // The type's own attributes, then each emitted field's, ahead of the field table that names them. A
    // field that will not be emitted gets no array, so nothing declared here is left unreferenced.
    char type_attr_symbol[NYA_REFLECT_MAX_NAME * 2] = { 0 };
    (void)snprintf(type_attr_symbol, sizeof(type_attr_symbol), "_NYA_REFLECT_%s_ATTRIBUTES", decl->name);
    _nya_reflect_emit_attributes(out, type_attr_symbol, decl->attributes, decl->attribute_count);

    for (u32 i = 0; i < decl->field_count; i++) {
        const _NYA_ReflectFieldDecl* field = &decl->fields[i];

        if (field->attribute_count == 0) continue;
        if (!_nya_reflect_field_emits(set, field, limit)) continue;

        char field_attr_symbol[NYA_REFLECT_MAX_NAME * 2] = { 0 };
        (void)snprintf(field_attr_symbol, sizeof(field_attr_symbol), "_NYA_REFLECT_%s_%s_ATTRIBUTES", decl->name, field->name);
        _nya_reflect_emit_attributes(out, field_attr_symbol, field->attributes, field->attribute_count);
    }

    nya_string_extend_sprintf(out, "static const NYA_ReflectField _NYA_REFLECT_%s_FIELDS[] = {\n", decl->name);

    u32 emitted = 0;

    for (u32 i = 0; i < decl->field_count; i++) {
        const _NYA_ReflectFieldDecl* field = &decl->fields[i];

        char             buffer[NYA_REFLECT_MAX_NAME * 2] = { 0 };
        NYA_ConstCString symbol                           = nullptr;

        // The reference to this field's attribute table, or empty when it carries none. Only ever set for
        // a field that emits, so it names an array _nya_reflect_emit_attributes actually wrote above.
        char attribute_suffix[NYA_REFLECT_MAX_NAME * 3] = { 0 };
        if (field->attribute_count > 0) {
            (void)snprintf(attribute_suffix, sizeof(attribute_suffix),
                           ", .attributes = _NYA_REFLECT_%s_%s_ATTRIBUTES, .attribute_count = %u", decl->name, field->name,
                           field->attribute_count);
        }

        if (field->array_extent[0] != '\0') {
            NYA_ConstCString element = _nya_reflect_field_symbol(set, field, limit, buffer, sizeof(buffer));

            if (element != nullptr) {
                char array_symbol[NYA_REFLECT_MAX_NAME * 2] = { 0 };
                (void)snprintf(array_symbol, sizeof(array_symbol), "_NYA_REFLECT_%s_%s_ARRAY", decl->name, field->name);

                nya_string_extend_sprintf(out,
                                          "    { .name = \"%s\", .type = &%s, .offset = nya_offsetof(%s, %s), .hint = %s%s%s%s%s },\n",
                                          field->name, array_symbol, decl->name, field->name, field->hint,
                                          field->is_key ? ", .is_key = true" : "",
                                          field->is_redacted ? ", .is_redacted = true" : "",
                                          field->is_secret ? ", .is_secret = true" : "", attribute_suffix);
                emitted++;
            } else {
                nya_log_warn("%s: '%s.%s' has undescribed element type '%s'; skipped. Add @reflect to it, or @skip to the field.",
                         decl->source_file, decl->name, field->name, field->type_spelling);
            }

            continue;
        }

        symbol = _nya_reflect_field_symbol(set, field, limit, buffer, sizeof(buffer));

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

        nya_string_extend_sprintf(out, "    { .name = \"%s\", .type = &%s, .offset = nya_offsetof(%s, %s), .hint = %s%s%s%s%s },\n",
                                  field->name, symbol, decl->name, field->name, field->hint,
                                  field->is_key ? ", .is_key = true" : "",
                                  field->is_redacted ? ", .is_redacted = true" : "",
                                  field->is_secret ? ", .is_secret = true" : "", attribute_suffix);
        emitted++;
    }

    // A struct every one of whose fields was skipped still needs a valid array: a zero length one is not legal C, so a placeholder keeps the table well formed and the count honest at zero.
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

    if (decl->attribute_count > 0) {
        nya_string_extend_sprintf(out, "    .attributes = %s, .attribute_count = %u,\n", type_attr_symbol, decl->attribute_count);
    }

    nya_string_extend(out, "};\n\n");
}
