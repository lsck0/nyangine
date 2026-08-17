#include "build/build.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One key of the base locale, with the argument types its format specifiers imply. */
typedef struct {
    NYA_CString key;

    /** The C type each argument is, as written into the generated signature. Null past `argument_count`. */
    NYA_ConstCString argument_types[NYA_I18N_MAX_ARGUMENTS];
    u32              argument_count;

    /**
     * The specifiers in the order they appear, one character each: 's', 'd', 'u', 'f'.
     *
     * Sorted before comparison, so a translation that reorders its arguments positionally still
     * matches. See the note on reordering in i18n.h.
     * */
    char specifiers[NYA_I18N_MAX_ARGUMENTS + 1];
} NYA_I18nKey;

/** Reads the specifiers out of a format string. False when one is unsupported, naming it. */
NYA_INTERNAL b8 _nya_i18n_parse_specifiers(NYA_ConstCString format, NYA_ConstCString where, NYA_ConstCString key, OUT NYA_I18nKey* out_key);

/** Uppercases and sanitises a key into an enum suffix: `hud_score` becomes `HUD_SCORE`. */
NYA_INTERNAL void _nya_i18n_enum_name(NYA_ConstCString key, OUT char* out, u64 capacity);

/** Every `.json` directly under NYA_I18N_DIRECTORY, sorted, without their extensions. */
NYA_INTERNAL b8 _nya_i18n_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);

NYA_INTERNAL s32 _nya_i18n_compare(const NYA_String* a, const NYA_String* b);

/** Sorts a specifier string in place. Three elements at most in practice; insertion is plenty. */
NYA_INTERNAL void _nya_i18n_sort_specifiers(char* specifiers);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_i18n_generate(void) {
    NYA_Arena* arena = nya_arena_create(.name = "i18n_generate");
    defer nya_arena_destroy(arena);

    // ── the base locale, which is the schema ────────────────────────────────────────────────────
    NYA_String* base_path = nya_string_sprintf(arena, "%s/%s.json", NYA_I18N_DIRECTORY, NYA_I18N_BASE_LOCALE);

    NYA_String* base_text = nya_string_create(arena);
    NYA_EXPECT(nya_file_read(nya_string_to_cstring(arena, base_path), base_text), "while reading the base locale");

    NYA_Object* base = nullptr;
    NYA_EXPECT(
        nya_deserialize(arena, base_text->items, base_text->length, NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &base), "while parsing the base locale"
    );

    NYA_I18nKey keys[NYA_I18N_MAX_KEYS];
    u32         key_count = 0;

    nya_dict_foreach_key (base, key_slot) {
        NYA_CString key = *key_slot;

        // Keys beginning with an underscore are metadata — the locale's own name, a comment for
        // translators. Skipped rather than generated, so a file can document itself.
        if (key[0] == '_') continue;

        NYA_Value* value = nya_object_get(base, key);
        if (value == nullptr || value->type != NYA_TYPE_STRING) {
            nya_panic("i18n: base locale key '%s' is not a string", key);
        }

        nya_assert(key_count < NYA_I18N_MAX_KEYS, "i18n: more than %d keys; raise NYA_I18N_MAX_KEYS", NYA_I18N_MAX_KEYS);

        NYA_I18nKey* entry = &keys[key_count++];
        *entry             = (NYA_I18nKey){ .key = key };

        if (!_nya_i18n_parse_specifiers(value->as_string, NYA_I18N_BASE_LOCALE, key, entry)) {
            nya_panic("i18n: base locale key '%s' uses an unsupported format specifier", key);
        }
    }

    /*
     * Sorted, because the enum's values are its output.
     *
     * The keys come out of a hash map, so their order depends on the hash and would differ between
     * runs — which would renumber NYA_StringId on every build. Anything that stored an id, or any
     * object file not rebuilt in the same pass, would then be reading a different string. Sorting
     * makes the generated header a function of the JSON and nothing else.
     */
    for (u32 i = 1; i < key_count; i++) {
        NYA_I18nKey current = keys[i];
        u32         j       = i;

        while (j > 0 && strcmp(keys[j - 1].key, current.key) > 0) {
            keys[j] = keys[j - 1];
            j--;
        }

        keys[j] = current;
    }

    // ── every locale, checked against it ────────────────────────────────────────────────────────
    NYA_ArrayᐸNYA_Stringᐳ* locales = nya_array_create(arena, NYA_String);
    NYA_EXPECT(nya_filesystem_walk(arena, NYA_I18N_DIRECTORY, _nya_i18n_collect, locales));
    nya_array_sort(locales, _nya_i18n_compare);

    nya_array_foreach (locales, locale) {
        NYA_CString name = nya_string_to_cstring(arena, locale);
        if (nya_string_equals(name, NYA_I18N_BASE_LOCALE)) continue;

        NYA_String* path = nya_string_sprintf(arena, "%s/%s.json", NYA_I18N_DIRECTORY, name);

        NYA_String* text = nya_string_create(arena);
        NYA_EXPECT(nya_file_read(nya_string_to_cstring(arena, path), text), "while reading a locale");

        NYA_Object* translated = nullptr;
        NYA_EXPECT(nya_deserialize(arena, text->items, text->length, NYA_SERDE_FORMAT_JSONC, NYA_SERDE_NONE, &translated), "while parsing a locale");

        // Every key the base has, and with the same arguments. A missing one would silently fall back
        // to English, so a half-finished translation would ship looking finished.
        for (u32 i = 0; i < key_count; i++) {
            NYA_Value* value = nya_object_get(translated, keys[i].key);

            if (value == nullptr || value->type != NYA_TYPE_STRING) {
                nya_panic("i18n: locale '%s' is missing key '%s'", name, keys[i].key);
            }

            NYA_I18nKey translated_key = { .key = keys[i].key };
            if (!_nya_i18n_parse_specifiers(value->as_string, name, keys[i].key, &translated_key)) {
                nya_panic("i18n: locale '%s' key '%s' uses an unsupported format specifier", name, keys[i].key);
            }

            /*
             * Sorted before comparison, so a translation may reorder its arguments positionally.
             *
             * That is the one case where a different order is correct rather than a bug, and it is
             * common enough — German and Japanese both need it constantly — that refusing it would
             * make the whole system unusable. What must not differ is the *set*: `"%s scored %d"`
             * translated with two `%s` reads an integer as a pointer, in exactly one language.
             */
            char expected[NYA_I18N_MAX_ARGUMENTS + 1];
            char actual[NYA_I18N_MAX_ARGUMENTS + 1];

            (void)snprintf(expected, sizeof(expected), "%s", keys[i].specifiers);
            (void)snprintf(actual, sizeof(actual), "%s", translated_key.specifiers);

            _nya_i18n_sort_specifiers(expected);
            _nya_i18n_sort_specifiers(actual);

            if (!nya_string_equals(expected, actual)) {
                nya_panic(
                    "i18n: locale '%s' key '%s' takes {%s} but the base takes {%s}; a translation must use the same arguments", name, keys[i].key,
                    actual, expected
                );
            }
        }

        // The other direction: a key here that the base does not have is a key that was renamed in
        // the base and not here, so its translation is already dead and nobody would notice.
        nya_dict_foreach_key (translated, key_slot) {
            NYA_CString key = *key_slot;
            if (key[0] == '_') continue;

            if (nya_object_get(base, key) == nullptr) {
                nya_panic("i18n: locale '%s' has key '%s', which the base locale does not; was it renamed?", name, key);
            }
        }
    }

    // ── the header ──────────────────────────────────────────────────────────────────────────────
    NYA_String* out = nya_string_create(arena);

    nya_string_extend(out, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n#pragma once\n\n");
    nya_string_extend(out, "#include \"nyangine/core/core_i18n.h\"\n\n");

    nya_string_extend_sprintf(
        out,
        "/*\n"
        " * Generated from %s by src/build/i18n.c. One entry and one accessor per key of the base\n"
        " * locale, with the accessor's parameters read off that string's format specifiers — so a call\n"
        " * with the wrong argument types is a compile error rather than a crash in one language.\n"
        " */\n\n",
        NYA_I18N_DIRECTORY "/" NYA_I18N_BASE_LOCALE ".json"
    );

    nya_string_extend(out, "typedef enum {\n");

    for (u32 i = 0; i < key_count; i++) {
        char name[256];
        _nya_i18n_enum_name(keys[i].key, name, sizeof(name));

        nya_string_extend_sprintf(out, "    NYA_STRING_%s,\n", name);
    }

    nya_string_extend(out, "\n    NYA_STRING_COUNT,\n} NYA_StringId;\n\n");

    // The key names, so the runtime can look a locale's JSON up by them without the header and the
    // loader having to agree on an order by hand.
    nya_string_extend(out, "/** The JSON key each id came from, in id order. Read by nya_i18n_load. */\n");
    // __attr_allow_unused for the same reason the accessors below carry it: this header is included
    // by every translation unit that draws text, and one that never calls nya_i18n_load still gets
    // the table. Without it that is -Wunused-const-variable in each of them.
    nya_string_extend(out, "static const NYA_ConstCString NYA_STRING_KEYS[NYA_STRING_COUNT] __attr_allow_unused = {\n");

    for (u32 i = 0; i < key_count; i++) nya_string_extend_sprintf(out, "    \"%s\",\n", keys[i].key);

    nya_string_extend(out, "};\n\n");

    for (u32 i = 0; i < key_count; i++) {
        char name[256];
        _nya_i18n_enum_name(keys[i].key, name, sizeof(name));

        nya_string_extend_sprintf(out, "/** `%s` */\n", keys[i].key);
        /*
         * __attr_allow_unused, on every accessor.
         *
         * There is one of these per key and this header is included wholesale, so any translation
         * unit that uses a single string still defines the other few hundred. `static inline` is
         * exempt from -Wunused-function in a normal build, but clangd and clang-tidy both analyse a
         * header as its own translation unit and report every accessor the file itself does not
         * call — "Unused function 'nya_string_hud_score'", several hundred times, in the editor.
         *
         * Marking them silences that at the definition rather than asking every consumer to disable
         * a warning, which is what a generated header should do.
         */
        nya_string_extend_sprintf(out, "static inline __attr_allow_unused NYA_ConstCString nya_string_%s(", keys[i].key);

        if (keys[i].argument_count == 0) {
            nya_string_extend(out, "void");
        } else {
            for (u32 argument = 0; argument < keys[i].argument_count; argument++) {
                if (argument > 0) nya_string_extend(out, ", ");
                nya_string_extend_sprintf(out, "%s a%u", keys[i].argument_types[argument], argument);
            }
        }

        nya_string_extend(out, ") {\n    return _nya_i18n_format(NYA_STRING_");
        nya_string_extend(out, name);

        for (u32 argument = 0; argument < keys[i].argument_count; argument++) nya_string_extend_sprintf(out, ", a%u", argument);

        nya_string_extend(out, ");\n}\n\n");
    }

    NYA_EXPECT(nya_file_write(NYA_I18N_OUTPUT, out), "while writing the generated strings header");

    // Formatted like the asset index is, and for the same reason: it is a header a human reads when
    // they want to know what keys exist.
    NYA_Command format_command = {
        .program   = "clang-format",
        .arguments = { "-i", NYA_I18N_OUTPUT },
    };
    NYA_EXPECT(nya_command_run(&format_command));

    nya_info("Generated %s: %u keys across %llu locales.", NYA_I18N_OUTPUT, key_count, (unsigned long long)locales->length);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_i18n_parse_specifiers(NYA_ConstCString format, NYA_ConstCString where, NYA_ConstCString key, OUT NYA_I18nKey* out_key) {
    nya_unused(where, key);

    u32 count = 0;

    for (const char* cursor = format; *cursor != '\0'; cursor++) {
        if (*cursor != '%') continue;

        cursor++;

        // `%%` is a literal percent and takes no argument.
        if (*cursor == '%') continue;
        if (*cursor == '\0') return false;

        /*
         * A positional prefix — `2$` — is skipped rather than acted on.
         *
         * It changes which argument a specifier consumes, not how many there are or what types they
         * have, and the caller's signature comes from the base locale where positions are not used.
         * A translation using them still has to name the same set, which is what the sorted
         * comparison checks.
         */
        const char* digits = cursor;
        while (*cursor >= '0' && *cursor <= '9') cursor++;
        if (*cursor == '$') cursor++;
        else cursor = digits;

        // Width, precision and flags, none of which change the argument's type.
        while (*cursor == '-' || *cursor == '+' || *cursor == ' ' || *cursor == '#' || *cursor == '0') cursor++;
        while ((*cursor >= '0' && *cursor <= '9') || *cursor == '.') cursor++;

        if (count >= NYA_I18N_MAX_ARGUMENTS) return false;

        /*
         * Only four kinds, deliberately.
         *
         * Every extra conversion is a type a translator can get wrong and a signature a caller has to
         * match, and none of the ones left out — `%p`, `%c`, the length modifiers — belongs in text a
         * player reads. A string that wants one of those wants formatting done before it gets here.
         */
        switch (*cursor) {
            case 's': out_key->argument_types[count] = "NYA_ConstCString"; break;
            case 'd':
            case 'i': out_key->argument_types[count] = "s32"; break;
            case 'u': out_key->argument_types[count] = "u32"; break;
            case 'f':
            case 'g': out_key->argument_types[count] = "f64"; break;
            default:  return false;
        }

        // Normalised, so `%i` and `%d` compare equal and `%g` and `%f` do too — a translator writing
        // one where the base wrote the other is not a bug and should not fail the build.
        char specifier = *cursor;
        if (specifier == 'i') specifier = 'd';
        if (specifier == 'g') specifier = 'f';

        out_key->specifiers[count] = specifier;
        count++;
    }

    out_key->specifiers[count] = '\0';
    out_key->argument_count    = count;

    return true;
}

void _nya_i18n_enum_name(NYA_ConstCString key, OUT char* out, u64 capacity) {
    u64 length = 0;

    for (const char* cursor = key; *cursor != '\0' && length + 1 < capacity; cursor++) {
        char character = *cursor;

        if (character >= 'a' && character <= 'z') character = (char)(character - 'a' + 'A');
        else if (!((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'))) character = '_';

        out[length++] = character;
    }

    out[length] = '\0';
}

b8 _nya_i18n_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data) {
    nya_unused(path);

    NYA_ArrayᐸNYA_Stringᐳ* locales = user_data;

    if (entry->type != NYA_FILE_TYPE_FILE) return true;
    if (!nya_string_ends_with(entry->name, ".json")) return true;

    NYA_String* name = nya_string_clone(locales->arena, entry->name);
    nya_string_strip_suffix(name, ".json");

    nya_array_push_back(locales, *name);

    return true;
}

s32 _nya_i18n_compare(const NYA_String* a, const NYA_String* b) {
    // Byte order, so the generated header is identical whatever order the filesystem walks in.
    u64 shortest = a->length < b->length ? a->length : b->length;

    for (u64 i = 0; i < shortest; i++) {
        if (a->items[i] != b->items[i]) return a->items[i] < b->items[i] ? -1 : 1;
    }

    if (a->length == b->length) return 0;

    return a->length < b->length ? -1 : 1;
}

void _nya_i18n_sort_specifiers(char* specifiers) {
    for (u32 i = 1; specifiers[i] != '\0'; i++) {
        char current = specifiers[i];
        u32  j       = i;

        while (j > 0 && specifiers[j - 1] > current) {
            specifiers[j] = specifiers[j - 1];
            j--;
        }

        specifiers[j] = current;
    }
}
