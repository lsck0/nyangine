#include "nyangine/nyangine.h"

#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One C type the boundary can carry, and how it crosses in each direction. */
typedef struct {
    /** As the declaration spells it, after whitespace has been collapsed. */
    NYA_ConstCString spelling;

    /** What a definitions file calls it: "number", "string", "boolean", "table". */
    NYA_ConstCString lua_type;

    /** A C expression reading argument `%u` of the call. Null for `void`, which is a return type only. */
    NYA_ConstCString read;

    /** A C expression turning `%s` into an NYA_Value. Null for `void`. */
    NYA_ConstCString write;
} _NYA_LuaBindType;

typedef struct {
    char type[NYA_LUABIND_MAX_NAME];
    char name[NYA_LUABIND_MAX_NAME];
} _NYA_LuaBindParameter;

/** One binding, generated or hand written. */
typedef struct {
    /** Where it lives in the `nya` table: "nya.input.action_pressed". */
    char path[NYA_LUABIND_MAX_NAME];

    /** Spelled out: "NYA_PLUGIN_PERMISSION_INPUT". */
    char permission[NYA_LUABIND_MAX_NAME];

    /** The C function being wrapped, or the hand written binding's own name. */
    char function[NYA_LUABIND_MAX_NAME];

    /** First sentence of the declaration's doc comment, or empty. */
    char summary[NYA_LUABIND_MAX_SUMMARY + 1];

    /**
     * Hand written: documented here, registered by whoever wrote it, no marshalling generated. See
     * `@lua_manual` in luabind.h.
     * */
    b8 manual;

    /** Generated only. `parameters` is empty for a `void` parameter list. */
    char                  return_type[NYA_LUABIND_MAX_NAME];
    _NYA_LuaBindParameter parameters[NYA_LUABIND_MAX_PARAMETERS];
    u32                   parameter_count;

    /** Hand written only: the Lua signature the annotation spelled out, already rendered. */
    char signature[NYA_LUABIND_MAX_NAME];
    char returns[NYA_LUABIND_MAX_NAME];
} _NYA_LuaBind;

typedef struct {
    _NYA_LuaBind bindings[NYA_LUABIND_MAX_BINDINGS];
    u32          count;

    /** How many annotated declarations were refused, so the build says so once at the end. */
    u32 skipped;
} _NYA_LuaBindSet;

/** A cursor over a file's bytes, handing out one trimmed line at a time. */
typedef struct {
    const char* text;
    u64         length;
    u64         offset;
} _NYA_LuaBindReader;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Everything that crosses, and nothing else.
 *
 * Integers all become Lua numbers, which are doubles: anything past 2^53 loses low bits, which is why
 * NYA_EntityHandle crosses as a table of two u32s rather than one packed number. A type that is not in
 * this table is not bound, and the build says which declaration wanted it.
 * */
NYA_INTERNAL const _NYA_LuaBindType _NYA_LUABIND_TYPES[] = {
    { "void",             "nil",     nullptr,                                                              nullptr                                  },
    { "b8",               "boolean", "_nya_lua_argument_boolean(call, %u, false)",                          "nya_lua_boolean(%s)"                    },
    { "f32",              "number",  "(f32)_nya_lua_argument_number(call, %u, 0.0)",                        "nya_lua_number((f64)%s)"                },
    { "f64",              "number",  "_nya_lua_argument_number(call, %u, 0.0)",                             "nya_lua_number(%s)"                     },
    { "s8",               "integer", "(s8)_nya_lua_argument_number(call, %u, 0.0)",                         "nya_lua_number((f64)%s)"                },
    { "s16",              "integer", "(s16)_nya_lua_argument_number(call, %u, 0.0)",                        "nya_lua_number((f64)%s)"                },
    { "s32",              "integer", "(s32)_nya_lua_argument_number(call, %u, 0.0)",                        "nya_lua_number((f64)%s)"                },
    { "s64",              "integer", "(s64)_nya_lua_argument_number(call, %u, 0.0)",                        "nya_lua_number((f64)%s)"                },
    { "u8",               "integer", "(u8)_nya_lua_argument_number(call, %u, 0.0)",                         "nya_lua_number((f64)%s)"                },
    { "u16",              "integer", "(u16)_nya_lua_argument_number(call, %u, 0.0)",                        "nya_lua_number((f64)%s)"                },
    { "u32",              "integer", "(u32)_nya_lua_argument_number(call, %u, 0.0)",                        "nya_lua_number((f64)%s)"                },
    { "u64",              "integer", "(u64)_nya_lua_argument_number(call, %u, 0.0)",                        "nya_lua_number((f64)%s)"                },
    { "NYA_ConstCString", "string",  "_nya_lua_argument_string(call, %u)",                                  "nya_lua_string(%s)"                     },
    { "NYA_EntityHandle", "table",   "_nya_lua_argument_handle(call, %u)",                                  "_nya_lua_handle_value(call->arena, %s)" },
    { "NYA_InputAction",  "integer", "(NYA_InputAction)(u32)_nya_lua_argument_number(call, %u, 0.0)",        "nya_lua_number((f64)%s)"                },
};

/**
 * The permission tokens an annotation may use, short because they are written dozens of times, and
 * checked against this list because a typo would otherwise generate a binding nobody can reach.
 * */
NYA_INTERNAL const NYA_ConstCString _NYA_LUABIND_PERMISSIONS[] = {
    "NONE", "UI", "INPUT", "KEYBINDING", "ENTITIES", "AUDIO", "ASSETS", "FILESYSTEM", "NETWORK",
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_INTERNAL b8 _nya_luabind_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);
NYA_INTERNAL s32 _nya_luabind_compare(const NYA_String* a, const NYA_String* b);

/** Next line, trimmed of surrounding whitespace. False at the end of the text. */
NYA_INTERNAL b8 _nya_luabind_next_line(_NYA_LuaBindReader* reader, OUT char* out, u64 capacity);

/** Collapses runs of whitespace to one space and trims the ends, in place. */
NYA_INTERNAL void _nya_luabind_collapse(char* text);

/** Removes every `__attr_...` and its parenthesised argument, plus a trailing semicolon. */
NYA_INTERNAL void _nya_luabind_strip_attributes(char* text);

/** The type table's row for `spelling`, or null. */
NYA_INTERNAL const _NYA_LuaBindType* _nya_luabind_type(NYA_ConstCString spelling) __attr_no_discard;

/** Whether `token` is one of the permission names, so a typo is caught here rather than at runtime. */
NYA_INTERNAL b8 _nya_luabind_permission_is_known(NYA_ConstCString token) __attr_no_discard;

/** "nya_input_action_pressed" becomes "nya.input.action_pressed". */
NYA_INTERNAL void _nya_luabind_derive_path(NYA_ConstCString function, OUT char* out, u64 capacity);

/** The text inside `marker...)` on a line of the pending comment, or false. */
NYA_INTERNAL b8 _nya_luabind_annotation(NYA_ConstCString comment, NYA_ConstCString marker, OUT char* out, u64 capacity);

/** Splits `text` on commas at depth zero, trimming each piece. Returns how many pieces there were. */
NYA_INTERNAL u32 _nya_luabind_split(NYA_ConstCString text, OUT char pieces[][NYA_LUABIND_MAX_NAME], u32 capacity);

/** Reads `<type> <name>` into a parameter. False for anything that is not exactly that. */
NYA_INTERNAL b8 _nya_luabind_parse_parameter(NYA_ConstCString text, OUT _NYA_LuaBindParameter* out);

/** Takes a joined `NYA_API` declaration apart into return type, function name and parameters. */
NYA_INTERNAL b8 _nya_luabind_parse_declaration(NYA_ConstCString entry, OUT _NYA_LuaBind* binding, NYA_ConstCString path);

/** Everything the scanner does with one file. */
NYA_INTERNAL void _nya_luabind_scan_file(_NYA_LuaBindSet* set, NYA_Arena* arena, NYA_ConstCString path);

/** Records the first sentence of `prose`, or nothing when it is too long to reproduce whole. */
NYA_INTERNAL void _nya_luabind_summary(NYA_ConstCString prose, OUT char* out, u64 capacity);

/** Writes the marshalling functions and the registration table. */
NYA_INTERNAL void _nya_luabind_emit_source(const _NYA_LuaBindSet* set, NYA_String* out);

/** Writes the `---@meta` definitions an editor reads. */
NYA_INTERNAL void _nya_luabind_emit_definitions(const _NYA_LuaBindSet* set, NYA_String* out);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_luabind_generate(void) {
    NYA_ConstCString inputs[]  = { NYA_LUABIND_DIRECTORY, "./src/build/pp/luabind.c", nullptr };
    NYA_ConstCString outputs[] = { NYA_LUABIND_OUTPUT_SOURCE, NYA_LUABIND_OUTPUT_DEFINITIONS, nullptr };
    if (nya_pp_is_current("generate_lua_bindings", inputs, outputs)) return;

    NYA_Arena* arena = nya_arena_create(.name = "luabind_generate");
    defer      nya_arena_destroy(arena);

    NYA_ArrayᐸNYA_Stringᐳ* files = nya_array_create(arena, NYA_String);
    NYA_EXPECT(nya_filesystem_walk(arena, NYA_LUABIND_DIRECTORY, _nya_luabind_collect, files), "while listing the engine sources");

    // Sorted, so the generated file is a function of the tree rather than of the order the filesystem
    // happened to hand it over. A diff of a generated file has to mean something changed.
    nya_array_sort(files, _nya_luabind_compare);

    _NYA_LuaBindSet* set = nya_arena_alloc(arena, sizeof(_NYA_LuaBindSet));
    *set                 = (_NYA_LuaBindSet){ 0 };

    nya_array_foreach (files, file) _nya_luabind_scan_file(set, arena, nya_string_to_cstring(arena, file));

    // A tree with no annotations at all means the marker changed or the scan broke, and emitting an
    // empty table would compile and leave every script with no `nya` to call.
    NYA_EXPECT(set->count > 0 ? NYA_OK : nya_error(NYA_ERROR_NOT_FOUND, "no @lua annotations found under %s", NYA_LUABIND_DIRECTORY),
               "while generating the Lua bindings");

    NYA_String* source = nya_string_create(arena);
    _nya_luabind_emit_source(set, source);
    NYA_EXPECT(nya_file_write(NYA_LUABIND_OUTPUT_SOURCE, source), "while writing the Lua bindings");

    NYA_String* definitions = nya_string_create(arena);
    _nya_luabind_emit_definitions(set, definitions);

    // The directory may not exist in a fresh clone; the file is committed, but the write has to work
    // the first time somebody adds the pass to a tree that never had it.
    NYA_EXPECT(nya_filesystem_create_directory("./docs/lua"), "while creating the Lua definitions directory");
    NYA_EXPECT(nya_file_write(NYA_LUABIND_OUTPUT_DEFINITIONS, definitions), "while writing the Lua definitions");

    nya_log_info("nya_luabind_generate: " FMTu32 " bindings into %s (" FMTu32 " annotated declarations refused).", set->count,
                 NYA_LUABIND_OUTPUT_SOURCE, set->skipped);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_luabind_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    NYA_ArrayᐸNYA_Stringᐳ* files = user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;

    // Both: `@lua` lives above a public declaration in a header, `@lua_manual` above the hand written
    // binding in the source that implements it.
    if (!nya_string_ends_with(entry->name, ".h") && !nya_string_ends_with(entry->name, ".c")) return true;

    NYA_String* full = nya_string_from(files->arena, path);
    nya_array_push_back(files, *full);

    return true;
}

s32 _nya_luabind_compare(const NYA_String* a, const NYA_String* b) {
    u64 shared     = nya_min(a->length, b->length);
    s32 difference = nya_memcmp(a->items, b->items, shared);

    if (difference != 0) return difference < 0 ? -1 : 1;
    if (a->length == b->length) return 0;

    return a->length < b->length ? -1 : 1;
}

b8 _nya_luabind_next_line(_NYA_LuaBindReader* reader, OUT char* out, u64 capacity) {
    nya_assert(reader != nullptr);
    nya_assert(out != nullptr);
    nya_assert(capacity > 1);

    if (reader->offset >= reader->length) return false;

    u64 start = reader->offset;
    u64 end   = start;
    while (end < reader->length && reader->text[end] != '\n') end++;

    reader->offset = end < reader->length ? end + 1 : reader->length;

    while (start < end && isspace((unsigned char)reader->text[start])) start++;
    while (end > start && isspace((unsigned char)reader->text[end - 1])) end--;

    u64 length = end - start;
    if (length >= capacity) length = capacity - 1;

    nya_memcpy(out, reader->text + start, length);
    out[length] = '\0';

    return true;
}

void _nya_luabind_collapse(char* text) {
    nya_assert(text != nullptr);

    u64 write = 0;
    b8  space = false;

    for (u64 read = 0; text[read] != '\0'; read++) {
        if (isspace((unsigned char)text[read])) {
            space = true;
            continue;
        }

        if (space && write > 0) text[write++] = ' ';

        space         = false;
        text[write++] = text[read];
    }

    text[write] = '\0';
}

void _nya_luabind_strip_attributes(char* text) {
    nya_assert(text != nullptr);

    for (;;) {
        char* found = strstr(text, "__attr");
        if (found == nullptr) break;

        char* after = found;
        while (*after != '\0' && (isalnum((unsigned char)*after) || *after == '_')) after++;

        if (*after == '(') {
            u32 depth = 0;
            while (*after != '\0') {
                if (*after == '(') depth++;
                if (*after == ')') {
                    depth--;
                    after++;
                    if (depth == 0) break;
                    continue;
                }
                after++;
            }
        }

        while (found > text && found[-1] == ' ') found--;

        memmove(found, after, strlen(after) + 1);
    }

    u64 length = strlen(text);
    while (length > 0 && (text[length - 1] == ';' || text[length - 1] == ' ')) text[--length] = '\0';
}

const _NYA_LuaBindType* _nya_luabind_type(NYA_ConstCString spelling) {
    nya_assert(spelling != nullptr);

    for (u32 i = 0; i < nya_carray_length(_NYA_LUABIND_TYPES); i++) {
        if (!nya_string_equals(_NYA_LUABIND_TYPES[i].spelling, spelling)) continue;

        return &_NYA_LUABIND_TYPES[i];
    }

    return nullptr;
}

b8 _nya_luabind_permission_is_known(NYA_ConstCString token) {
    nya_assert(token != nullptr);

    for (u32 i = 0; i < nya_carray_length(_NYA_LUABIND_PERMISSIONS); i++) {
        if (nya_string_equals(_NYA_LUABIND_PERMISSIONS[i], token)) return true;
    }

    return false;
}

void _nya_luabind_derive_path(NYA_ConstCString function, OUT char* out, u64 capacity) {
    nya_assert(function != nullptr && out != nullptr && capacity > 1);

    NYA_ConstCString rest = function;
    if (strncmp(rest, "nya_", 4) == 0) rest += 4;

    // The first word is the sub-table and the rest is the function, so `nya_entity_move_to` reads as
    // `nya.entity.move_to` rather than as one long name under `nya`.
    const char* underscore = strchr(rest, '_');

    if (underscore == nullptr || underscore == rest) {
        (void)snprintf(out, capacity, "nya.%s", rest);
        return;
    }

    (void)snprintf(out, capacity, "nya.%.*s.%s", (s32)(underscore - rest), rest, underscore + 1);
}

b8 _nya_luabind_annotation(NYA_ConstCString comment, NYA_ConstCString marker, OUT char* out, u64 capacity) {
    nya_assert(comment != nullptr && marker != nullptr && out != nullptr);

    out[0] = '\0';

    // Both markers carry their opening parenthesis, so "@lua(" cannot match inside "@lua_manual(" and
    // the two searches do not have to be ordered against each other.
    const char* found = strstr(comment, marker);
    if (found == nullptr) return false;

    const char* open = found + strlen(marker);
    const char* end  = strchr(open, ')');
    if (end == nullptr) return false;

    u64 length = (u64)(end - open);
    if (length >= capacity) length = capacity - 1;

    nya_memcpy(out, open, length);
    out[length] = '\0';

    return true;
}

u32 _nya_luabind_split(NYA_ConstCString text, OUT char pieces[][NYA_LUABIND_MAX_NAME], u32 capacity) {
    nya_assert(text != nullptr && pieces != nullptr);

    u32 count = 0;
    u32 depth = 0;
    u64 start = 0;
    u64 i     = 0;

    for (;; i++) {
        if (text[i] == '(' || text[i] == '[') depth++;
        if (text[i] == ')' || text[i] == ']') depth--;

        b8 at_end = text[i] == '\0';
        if (!at_end && (text[i] != ',' || depth > 0)) continue;

        if (count >= capacity) return count;

        u64 from = start;
        u64 to   = i;

        while (from < to && isspace((unsigned char)text[from])) from++;
        while (to > from && isspace((unsigned char)text[to - 1])) to--;

        u64 length = to - from;
        if (length >= NYA_LUABIND_MAX_NAME) length = NYA_LUABIND_MAX_NAME - 1;

        nya_memcpy(pieces[count], text + from, length);
        pieces[count][length] = '\0';

        if (length > 0) count++;

        if (at_end) break;

        start = i + 1;
    }

    return count;
}

b8 _nya_luabind_parse_parameter(NYA_ConstCString text, OUT _NYA_LuaBindParameter* out) {
    nya_assert(text != nullptr && out != nullptr);

    *out = (_NYA_LuaBindParameter){ 0 };

    // `void` as the whole parameter list means no parameters, which the caller handles; anything with
    // a star, a bracket or a `const` in it is a pointer, an array or a view and does not cross.
    if (strchr(text, '*') != nullptr || strchr(text, '[') != nullptr) return false;

    const char* space = strrchr(text, ' ');
    if (space == nullptr) return false;

    u64 type_length = (u64)(space - text);
    if (type_length == 0 || type_length >= sizeof(out->type)) return false;

    nya_memcpy(out->type, text, type_length);
    out->type[type_length] = '\0';

    (void)snprintf(out->name, sizeof(out->name), "%s", space + 1);

    return out->name[0] != '\0';
}

b8 _nya_luabind_parse_declaration(NYA_ConstCString entry, OUT _NYA_LuaBind* binding, NYA_ConstCString path) {
    nya_assert(entry != nullptr && binding != nullptr && path != nullptr);

    const char* open = strchr(entry, '(');
    if (open == nullptr) {
        nya_log_warn("%s: '%s' carries @lua but is not a function; skipped.", path, entry);
        return false;
    }

    const char* close = strrchr(entry, ')');
    if (close == nullptr || close < open) return false;

    // ── the name, which is the last word before the parenthesis ─────────────────────────────────
    const char* name_end   = open;
    const char* name_start = name_end;
    while (name_start > entry && (isalnum((unsigned char)name_start[-1]) || name_start[-1] == '_')) name_start--;

    u64 name_length = (u64)(name_end - name_start);
    if (name_length == 0 || name_length >= sizeof(binding->function)) return false;

    nya_memcpy(binding->function, name_start, name_length);
    binding->function[name_length] = '\0';

    // ── the return type, which is everything before it ──────────────────────────────────────────
    u64 return_length = (u64)(name_start - entry);
    while (return_length > 0 && entry[return_length - 1] == ' ') return_length--;

    if (return_length == 0 || return_length >= sizeof(binding->return_type)) return false;

    nya_memcpy(binding->return_type, entry, return_length);
    binding->return_type[return_length] = '\0';

    if (_nya_luabind_type(binding->return_type) == nullptr) {
        nya_log_warn("%s: '%s' returns '%s', which does not cross into Lua; skipped.", path, binding->function, binding->return_type);
        return false;
    }

    // ── the parameters ──────────────────────────────────────────────────────────────────────────
    char parameters[NYA_LUABIND_MAX_ENTRY];
    u64  parameters_length = (u64)(close - open - 1);

    if (parameters_length >= sizeof(parameters)) return false;

    nya_memcpy(parameters, open + 1, parameters_length);
    parameters[parameters_length] = '\0';
    _nya_luabind_collapse(parameters);

    if (parameters[0] == '\0' || nya_string_equals(parameters, "void")) return true;

    if (strstr(parameters, "...") != nullptr) {
        nya_log_warn("%s: '%s' is variadic, which has no signature to generate against; skipped.", path, binding->function);
        return false;
    }

    char pieces[NYA_LUABIND_MAX_PARAMETERS][NYA_LUABIND_MAX_NAME] = { 0 };
    u32  piece_count                                              = _nya_luabind_split(parameters, pieces, NYA_LUABIND_MAX_PARAMETERS);

    for (u32 i = 0; i < piece_count; i++) {
        if (!_nya_luabind_parse_parameter(pieces[i], &binding->parameters[i])) {
            nya_log_warn("%s: '%s' takes '%s', which does not cross into Lua; skipped.", path, binding->function, pieces[i]);
            return false;
        }

        if (_nya_luabind_type(binding->parameters[i].type) != nullptr) continue;

        nya_log_warn("%s: '%s' takes a '%s', which does not cross into Lua; skipped.", path, binding->function, binding->parameters[i].type);
        return false;
    }

    binding->parameter_count = piece_count;

    return true;
}

void _nya_luabind_summary(NYA_ConstCString prose, OUT char* out, u64 capacity) {
    nya_assert(prose != nullptr && out != nullptr);

    out[0] = '\0';

    u64 length = 0;
    while (prose[length] != '\0') {
        if (prose[length] == '.' && (prose[length + 1] == '\0' || prose[length + 1] == ' ')) {
            length++;
            break;
        }
        length++;
    }

    if (length == 0 || length >= capacity) return;
    if (prose[0] == '@' || prose[0] == '`') return;

    nya_memcpy(out, prose, length);
    out[length] = '\0';
}

void _nya_luabind_scan_file(_NYA_LuaBindSet* set, NYA_Arena* arena, NYA_ConstCString path) {
    nya_assert(set != nullptr && arena != nullptr && path != nullptr);

    NYA_String* text = nya_string_create(arena);
    NYA_EXPECT(nya_file_read(path, text), "while reading %s", path);

    // Read whole and searched once: almost every file in the tree carries no annotation at all, and a
    // line by line scan of three hundred files to find forty markers is most of this pass's time.
    NYA_CString contents = nya_string_to_cstring(arena, text);
    if (strstr(contents, NYA_LUABIND_MARKER) == nullptr && strstr(contents, NYA_LUABIND_MARKER_MANUAL) == nullptr) return;

    _NYA_LuaBindReader reader = { .text = contents, .length = text->length };

    char line[NYA_LUABIND_MAX_LINE];
    char comment[NYA_LUABIND_MAX_ENTRY];
    char prose[NYA_LUABIND_MAX_ENTRY];

    comment[0] = '\0';
    prose[0]   = '\0';

    b8 in_comment = false;

    while (_nya_luabind_next_line(&reader, line, sizeof(line))) {
        if (strncmp(line, "/*", 2) == 0) {
            in_comment = true;
            comment[0] = '\0';
            prose[0]   = '\0';
        }

        if (in_comment) {
            // The leading `*` of a continuation line is decoration; the text after it is either prose
            // or an annotation, and both are wanted.
            const char* content = line;
            while (*content == '*' || *content == '/') content++;
            while (*content == ' ') content++;

            char stripped[NYA_LUABIND_MAX_LINE];
            (void)snprintf(stripped, sizeof(stripped), "%s", content);

            // The closing `*/` is not part of what the line says, and leaving it on would put it in the
            // summary of every one-line doc comment.
            char* terminator = strstr(stripped, "*/");
            if (terminator != nullptr) *terminator = '\0';

            while (strlen(stripped) > 0 && stripped[strlen(stripped) - 1] == ' ') stripped[strlen(stripped) - 1] = '\0';

            u64 used = strlen(comment);
            (void)snprintf(&comment[used], sizeof(comment) - used, "%s\n", stripped);

            if (prose[0] == '\0' && stripped[0] != '\0' && stripped[0] != '@') {
                (void)snprintf(prose, sizeof(prose), "%s", stripped);
            }

            if (strstr(line, "*/") != nullptr) in_comment = false;
            continue;
        }

        if (line[0] == '\0') continue;

        b8 has_manual = strstr(comment, NYA_LUABIND_MARKER_MANUAL) != nullptr;
        b8 has_lua    = strstr(comment, NYA_LUABIND_MARKER) != nullptr;

        if (!has_manual && !has_lua) {
            // A line that is not a declaration and carries no annotation clears the pending comment, so
            // a doc block never drifts onto something further down the file.
            comment[0] = '\0';
            prose[0]   = '\0';
            continue;
        }

        if (set->count >= NYA_LUABIND_MAX_BINDINGS) {
            nya_log_warn("%s: more than " FMTu32 " bindings; the rest are skipped.", path, (u32)NYA_LUABIND_MAX_BINDINGS);
            return;
        }

        _NYA_LuaBind binding = { 0 };
        _nya_luabind_summary(prose, binding.summary, sizeof(binding.summary));

        char argument[NYA_LUABIND_MAX_ENTRY];

        if (has_manual) {
            (void)_nya_luabind_annotation(comment, NYA_LUABIND_MARKER_MANUAL, argument, sizeof(argument));

            /*
             * `@lua_manual(path, PERMISSION, name: type, ..., -> type)`. Documented here and registered
             * by whoever wrote it, so there is nothing to parse out of the declaration below.
             */
            char pieces[NYA_LUABIND_MAX_PARAMETERS + 3][NYA_LUABIND_MAX_NAME] = { 0 };
            u32  piece_count = _nya_luabind_split(argument, pieces, NYA_LUABIND_MAX_PARAMETERS + 3);

            if (piece_count < 2 || !_nya_luabind_permission_is_known(pieces[1])) {
                nya_log_warn("%s: @lua_manual(%s) needs a path and a known permission; skipped.", path, argument);
                set->skipped++;
                comment[0] = '\0';
                continue;
            }

            binding.manual = true;
            (void)snprintf(binding.path, sizeof(binding.path), "%s", pieces[0]);
            (void)snprintf(binding.permission, sizeof(binding.permission), "NYA_PLUGIN_PERMISSION_%s", pieces[1]);

            for (u32 i = 2; i < piece_count; i++) {
                if (strncmp(pieces[i], "->", 2) == 0) {
                    NYA_ConstCString returns = pieces[i] + 2;
                    while (*returns == ' ') returns++;

                    (void)snprintf(binding.returns, sizeof(binding.returns), "%s", returns);
                    continue;
                }

                u64 used = strlen(binding.signature);
                (void)snprintf(&binding.signature[used], sizeof(binding.signature) - used, "%s%s", used > 0 ? ", " : "", pieces[i]);
            }

            set->bindings[set->count] = binding;
            set->count++;

            comment[0] = '\0';
            prose[0]   = '\0';
            continue;
        }

        /* `@lua(PERMISSION)` or `@lua(PERMISSION, path)`, above a NYA_API declaration. */
        if (strncmp(line, "NYA_API ", 8) != 0) {
            nya_log_warn("%s: @lua is not above a NYA_API declaration; skipped.", path);
            set->skipped++;
            comment[0] = '\0';
            continue;
        }

        (void)_nya_luabind_annotation(comment, NYA_LUABIND_MARKER, argument, sizeof(argument));

        char pieces[2][NYA_LUABIND_MAX_NAME] = { 0 };
        u32  piece_count                     = _nya_luabind_split(argument, pieces, 2);

        if (piece_count == 0 || !_nya_luabind_permission_is_known(pieces[0])) {
            nya_log_warn("%s: @lua(%s) does not name a permission; skipped.", path, argument);
            set->skipped++;
            comment[0] = '\0';
            continue;
        }

        (void)snprintf(binding.permission, sizeof(binding.permission), "NYA_PLUGIN_PERMISSION_%s", pieces[0]);

        // ── the declaration, joined until its semicolon ──────────────────────────────────────────
        char entry[NYA_LUABIND_MAX_ENTRY];
        (void)snprintf(entry, sizeof(entry), "%s", line + 8);

        while (strchr(entry, ';') == nullptr && _nya_luabind_next_line(&reader, line, sizeof(line))) {
            u64 used = strlen(entry);
            (void)snprintf(&entry[used], sizeof(entry) - used, " %s", line);
        }

        _nya_luabind_collapse(entry);

        b8 overloaded = strstr(entry, "__attr_overloaded") != nullptr;

        _nya_luabind_strip_attributes(entry);

        if (overloaded) {
            nya_log_warn("%s: '%s' is overloaded, and two C functions of one name are not one Lua function; skipped.", path, entry);
            set->skipped++;
            comment[0] = '\0';
            continue;
        }

        if (!_nya_luabind_parse_declaration(entry, &binding, path)) {
            set->skipped++;
            comment[0] = '\0';
            prose[0]   = '\0';
            continue;
        }

        if (piece_count > 1) (void)snprintf(binding.path, sizeof(binding.path), "%s", pieces[1]);
        else _nya_luabind_derive_path(binding.function, binding.path, sizeof(binding.path));

        set->bindings[set->count] = binding;
        set->count++;

        comment[0] = '\0';
        prose[0]   = '\0';
    }
}

void _nya_luabind_emit_source(const _NYA_LuaBindSet* set, NYA_String* out) {
    nya_assert(set != nullptr && out != nullptr);

    nya_string_extend(out,
                      "/*\n"
                      " * GENERATED by src/build/pp/luabind.c from the @lua annotations in the engine headers. DO NYAT EDIT.\n"
                      " *\n"
                      " * Included by src/nyangine/plugins/lua/lua_engine.c, after the marshalling helpers it calls and the\n"
                      " * _NYA_LuaBindingEntry it fills in. One function per binding, and one table for them all.\n"
                      " */\n\n");

    for (u32 i = 0; i < set->count; i++) {
        const _NYA_LuaBind* binding = &set->bindings[i];
        if (binding->manual) continue;

        const _NYA_LuaBindType* returns = _nya_luabind_type(binding->return_type);

        nya_string_extend_sprintf(out, "NYA_INTERNAL void _nya_lua_bind_%s(NYA_LuaCall* call) {\n", binding->function);

        if (binding->parameter_count == 0 && returns->write == nullptr) nya_string_extend(out, "    nya_unused(call);\n\n");

        for (u32 p = 0; p < binding->parameter_count; p++) {
            const _NYA_LuaBindType* type = _nya_luabind_type(binding->parameters[p].type);

            char read[NYA_LUABIND_MAX_NAME];
            (void)snprintf(read, sizeof(read), type->read, p);

            nya_string_extend_sprintf(out, "    %s %s = %s;\n", binding->parameters[p].type, binding->parameters[p].name, read);
        }

        if (binding->parameter_count > 0) nya_string_extend(out, "\n");

        // The declared return type rather than `auto`: the generated file is read by people, and a
        // reader should not have to find the header to know what came back.
        if (returns->write != nullptr) nya_string_extend_sprintf(out, "    %s result = ", binding->return_type);
        else nya_string_extend(out, "    ");

        nya_string_extend_sprintf(out, "%s(", binding->function);

        for (u32 p = 0; p < binding->parameter_count; p++) {
            nya_string_extend_sprintf(out, "%s%s", p > 0 ? ", " : "", binding->parameters[p].name);
        }

        nya_string_extend(out, ");\n");

        if (returns->write != nullptr) {
            char write[NYA_LUABIND_MAX_NAME];
            (void)snprintf(write, sizeof(write), returns->write, "result");

            nya_string_extend_sprintf(out, "\n    call->results[0]   = %s;\n    call->result_count = 1;\n", write);
        }

        nya_string_extend(out, "}\n\n");
    }

    nya_string_extend(out,
                      "/* Every generated binding, in one table. The hand written ones are in lua_engine.c beside it. */\n"
                      "NYA_INTERNAL const _NYA_LuaBindingEntry _NYA_LUA_GENERATED_BINDINGS[] = {\n");

    for (u32 i = 0; i < set->count; i++) {
        const _NYA_LuaBind* binding = &set->bindings[i];
        if (binding->manual) continue;

        nya_string_extend_sprintf(out, "    { \"%s\", _nya_lua_bind_%s, %s },\n", binding->path, binding->function, binding->permission);
    }

    nya_string_extend(out, "};\n");
}

void _nya_luabind_emit_definitions(const _NYA_LuaBindSet* set, NYA_String* out) {
    nya_assert(set != nullptr && out != nullptr);

    nya_string_extend(out,
                      "---@meta\n"
                      "--\n"
                      "-- GENERATED by src/build/pp/luabind.c from the @lua annotations in the engine headers. DO NYAT EDIT.\n"
                      "--\n"
                      "-- What a plugin may actually call is narrower than this: a binding is only registered when the\n"
                      "-- manifest asked for its permission and the game's build granted it. The permission is on every\n"
                      "-- entry below, so a call that is missing at runtime can be traced to the manifest that did not ask\n"
                      "-- for it. See src/nyangine/core/core_plugin.h.\n"
                      "\n"
                      "---@class nya\n"
                      "nya = {}\n\n");

    /*
     * The sub-tables first and each of them once: `nya.input = {}` has to exist before anything is
     * declared on it, and two functions in one module must not declare it twice.
     */
    char seen[NYA_LUABIND_MAX_BINDINGS][NYA_LUABIND_MAX_NAME] = { 0 };
    u32  seen_count                                           = 0;

    for (u32 i = 0; i < set->count; i++) {
        char module[NYA_LUABIND_MAX_NAME];
        (void)snprintf(module, sizeof(module), "%s", set->bindings[i].path);

        char* last = strrchr(module, '.');
        if (last == nullptr) continue;

        *last = '\0';

        // Only one level deep, which is every path the derivation produces; a deeper one would need its
        // parents declared too and nothing writes one.
        if (strchr(module, '.') == nullptr) continue;

        b8 already = false;
        for (u32 s = 0; s < seen_count; s++) already = already || nya_string_equals(seen[s], module);

        if (already || seen_count >= NYA_LUABIND_MAX_BINDINGS) continue;

        (void)snprintf(seen[seen_count], sizeof(seen[0]), "%s", module);
        seen_count++;

        nya_string_extend_sprintf(out, "%s = {}\n", module);
    }

    nya_string_extend(out, "\n");

    for (u32 i = 0; i < set->count; i++) {
        const _NYA_LuaBind* binding = &set->bindings[i];

        if (binding->summary[0] != '\0') nya_string_extend_sprintf(out, "---%s\n", binding->summary);

        nya_string_extend_sprintf(out, "---Needs %s.\n", binding->permission);

        if (binding->manual) {
            char parameters[NYA_LUABIND_MAX_NAME * 2] = { 0 };

            char pieces[NYA_LUABIND_MAX_PARAMETERS][NYA_LUABIND_MAX_NAME] = { 0 };
            u32  piece_count = _nya_luabind_split(binding->signature, pieces, NYA_LUABIND_MAX_PARAMETERS);

            for (u32 p = 0; p < piece_count; p++) {
                char name[NYA_LUABIND_MAX_NAME];
                (void)snprintf(name, sizeof(name), "%s", pieces[p]);

                char* colon = strchr(name, ':');
                if (colon != nullptr) *colon = '\0';

                NYA_ConstCString type = colon != nullptr ? colon + 1 : "any";
                while (*type == ' ') type++;

                nya_string_extend_sprintf(out, "---@param %s %s\n", name, type);

                u64 used = strlen(parameters);
                (void)snprintf(&parameters[used], sizeof(parameters) - used, "%s%s", used > 0 ? ", " : "", name);
            }

            if (binding->returns[0] != '\0') nya_string_extend_sprintf(out, "---@return %s\n", binding->returns);

            nya_string_extend_sprintf(out, "function %s(%s) end\n\n", binding->path, parameters);
            continue;
        }

        char parameters[NYA_LUABIND_MAX_NAME * 2] = { 0 };

        for (u32 p = 0; p < binding->parameter_count; p++) {
            const _NYA_LuaBindType* type = _nya_luabind_type(binding->parameters[p].type);

            nya_string_extend_sprintf(out, "---@param %s %s\n", binding->parameters[p].name, type->lua_type);

            u64 used = strlen(parameters);
            (void)snprintf(&parameters[used], sizeof(parameters) - used, "%s%s", used > 0 ? ", " : "", binding->parameters[p].name);
        }

        const _NYA_LuaBindType* returns = _nya_luabind_type(binding->return_type);
        if (returns->write != nullptr) nya_string_extend_sprintf(out, "---@return %s\n", returns->lua_type);

        nya_string_extend_sprintf(out, "function %s(%s) end\n\n", binding->path, parameters);
    }
}
