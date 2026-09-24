#include "build/build.h"

/* PRIVATE API DECLARATION */

/** One key of the base locale, with the argument types its format specifiers imply. */
typedef struct {
    NYA_CString key;

    /** The C type each argument is, as written into the generated signature. Null past `argument_count`. */
    NYA_ConstCString argument_types[NYA_I18N_MAX_ARGUMENTS];
    u32              argument_count;

    /**
     * The specifiers in the order they appear, one character each: 's', 'd', 'u', 'f'.
     * */
    char specifiers[NYA_I18N_MAX_ARGUMENTS + 1];

    /** A plural message: an object of CLDR category variants rather than one string. */
    b8 is_plural;
} NYA_I18nKey;

/** Reads the specifiers out of a format string. False when one is unsupported, naming it. */
NYA_INTERNAL b8 _nya_i18n_parse_specifiers(NYA_ConstCString format, NYA_ConstCString where, NYA_ConstCString key, OUT NYA_I18nKey* out_key);

/**
 * Validates a plural message — an object of category variants — and fills `out_key` from its `other`
 * variant. Panics if `other` is missing, if a variant uses a different argument set than `other`, or if
 * the first argument is not an integer, since that first argument is the count the runtime selects on.
 * The CLDR category names are the only object keys allowed.
 * */
NYA_INTERNAL void _nya_i18n_plural_key(NYA_Object* object, NYA_ConstCString where, NYA_ConstCString key, OUT NYA_I18nKey* out_key);

/** Uppercases and sanitises a key into an enum suffix: `hud_score` becomes `HUD_SCORE`. */
NYA_INTERNAL void _nya_i18n_enum_name(NYA_ConstCString key, OUT char* out, u64 capacity);

/** Every `.json` directly under NYA_I18N_DIRECTORY, sorted, without their extensions. */
NYA_INTERNAL b8 _nya_i18n_collect(NYA_ConstCString path, const NYA_DirectoryEntry* entry, void* user_data);

NYA_INTERNAL s32 _nya_i18n_compare(const NYA_String* a, const NYA_String* b);

/** Sorts a specifier string in place. Three elements at most in practice; insertion is plenty. */
NYA_INTERNAL void _nya_i18n_sort_specifiers(char* specifiers);

/* PUBLIC API IMPLEMENTATION */

void nya_i18n_generate(void) {
    NYA_ConstCString inputs[]  = { NYA_I18N_DIRECTORY, "./src/build/pp/i18n.c", nullptr };
    NYA_ConstCString outputs[] = { NYA_I18N_OUTPUT, nullptr };
    if (nya_pp_is_current("generate_strings", inputs, outputs)) return;

    NYA_Arena* arena = nya_arena_create(.name = "i18n_generate");
    defer nya_arena_destroy(arena);

    // the base locale, which is the schema
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

        // keys starting with an underscore are metadata, such as the locale name or translator notes, and generate nothing.
        if (key[0] == '_') continue;

        NYA_Value* value = nya_object_get(base, key);
        if (value == nullptr || (value->type != NYA_TYPE_STRING && value->type != NYA_TYPE_OBJECT)) {
            nya_log_panic("i18n: base locale key '%s' is neither a string nor a plural object", key);
        }

        nya_assert(key_count < NYA_I18N_MAX_KEYS, "i18n: more than %d keys; raise NYA_I18N_MAX_KEYS", NYA_I18N_MAX_KEYS);

        NYA_I18nKey* entry = &keys[key_count++];
        *entry             = (NYA_I18nKey){ .key = key };

        // A plural key's arguments come off its `other` variant; a plain key's off its one string.
        if (value->type == NYA_TYPE_OBJECT) {
            entry->is_plural = true;
            _nya_i18n_plural_key(&value->as_object, NYA_I18N_BASE_LOCALE, key, entry);
        } else if (!_nya_i18n_parse_specifiers(value->as_string, NYA_I18N_BASE_LOCALE, key, entry)) {
            nya_log_panic("i18n: base locale key '%s' uses an unsupported format specifier", key);
        }
    }

    /* Sorted, because the enum's values are its output. */
    for (u32 i = 1; i < key_count; i++) {
        NYA_I18nKey current = keys[i];
        u32         j       = i;

        while (j > 0 && strcmp(keys[j - 1].key, current.key) > 0) {
            keys[j] = keys[j - 1];
            j--;
        }

        keys[j] = current;
    }

    // every locale, checked against it
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

        // Every key the base has, and with the same arguments. A missing one would silently fall back to English, so a half-finished translation would ship looking finished.
        for (u32 i = 0; i < key_count; i++) {
            NYA_Value* value = nya_object_get(translated, keys[i].key);

            if (value == nullptr) {
                nya_log_panic("i18n: locale '%s' is missing key '%s'", name, keys[i].key);
            }

            // A plural key stays plural in every locale: a translation that flattened it to one string would silently lose the count agreement the base spells out.
            NYA_Type wanted = keys[i].is_plural ? NYA_TYPE_OBJECT : NYA_TYPE_STRING;
            if (value->type != wanted) {
                nya_log_panic(
                    "i18n: locale '%s' key '%s' must be %s, like the base", name, keys[i].key, keys[i].is_plural ? "a plural object" : "a string"
                );
            }

            NYA_I18nKey translated_key = { .key = keys[i].key };
            if (keys[i].is_plural) {
                _nya_i18n_plural_key(&value->as_object, name, keys[i].key, &translated_key);
            } else if (!_nya_i18n_parse_specifiers(value->as_string, name, keys[i].key, &translated_key)) {
                nya_log_panic("i18n: locale '%s' key '%s' uses an unsupported format specifier", name, keys[i].key);
            }

            /* Sorted before comparison, so a translation may reorder its arguments positionally. */
            /* Zeroed, not just assigned into. _nya_i18n_sort_specifiers walks to the first '\0' and nya_string_equals reads the buffers again, so leftover stack bytes made keys intermittently report specifiers they did not have. */
            char expected[NYA_I18N_MAX_ARGUMENTS + 1] = { 0 };
            char actual[NYA_I18N_MAX_ARGUMENTS + 1]   = { 0 };

            (void)snprintf(expected, sizeof(expected), "%s", keys[i].specifiers);
            (void)snprintf(actual, sizeof(actual), "%s", translated_key.specifiers);

            _nya_i18n_sort_specifiers(expected);
            _nya_i18n_sort_specifiers(actual);

            if (!nya_string_equals(expected, actual)) {
                nya_log_panic(
                    "i18n: locale '%s' key '%s' takes {%s} but the base takes {%s}; a translation must use the same arguments", name, keys[i].key,
                    actual, expected
                );
            }
        }

        // The other direction: a key here that the base does not have is a key that was renamed in the base and not here, so its translation is already dead and nobody would notice.
        nya_dict_foreach_key (translated, key_slot) {
            NYA_CString key = *key_slot;
            if (key[0] == '_') continue;

            if (nya_object_get(base, key) == nullptr) {
                nya_log_panic("i18n: locale '%s' has key '%s', which the base locale does not; was it renamed?", name, key);
            }
        }
    }

    // the header
    NYA_String* out = nya_string_create(arena);

    nya_string_extend(out, "/* THIS FILE IS GENERATED. DO NYAT TOUCH. */\n\n#pragma once\n\n");
    nya_string_extend(out, "#include \"nyangine/core/core_i18n.h\"\n\n");

    nya_string_extend_sprintf(
        out,
        "/*\n"
        " * Generated from %s by src/build/i18n.c. One entry and one accessor per key of the base\n"
        " * locale, with the accessor's parameters read off that string's format specifiers, so a call\n"
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

    // The key names, so the runtime can look a locale's JSON up by them without the header and the loader having to agree on an order by hand.
    nya_string_extend(out, "/** The JSON key each id came from, in id order. Read by nya_i18n_load. */\n");
    // __attr_allow_unused for the same reason the accessors below carry it: this header is included by every translation unit that draws text, and one that never calls nya_i18n_load still gets the table. Without it that is -Wunused-const-variable in each of them.
    nya_string_extend(out, "static const NYA_ConstCString NYA_STRING_KEYS[NYA_STRING_COUNT] __attr_allow_unused = {\n");

    for (u32 i = 0; i < key_count; i++) nya_string_extend_sprintf(out, "    \"%s\",\n", keys[i].key);

    nya_string_extend(out, "};\n\n");

    for (u32 i = 0; i < key_count; i++) {
        char name[256];
        _nya_i18n_enum_name(keys[i].key, name, sizeof(name));

        nya_string_extend_sprintf(out, "/** `%s` */\n", keys[i].key);
        /* __attr_allow_unused, on every accessor. */
        nya_string_extend_sprintf(out, "static inline __attr_allow_unused NYA_ConstCString nya_string_%s(", keys[i].key);

        if (keys[i].argument_count == 0) {
            nya_string_extend(out, "void");
        } else {
            for (u32 argument = 0; argument < keys[i].argument_count; argument++) {
                if (argument > 0) nya_string_extend(out, ", ");
                nya_string_extend_sprintf(out, "%s a%u", keys[i].argument_types[argument], argument);
            }
        }

        // A plural accessor selects the variant on its first argument, the count, and passes that same argument on to be formatted; a plain one formats its string directly.
        if (keys[i].is_plural) {
            nya_string_extend_sprintf(out, ") {\n    return _nya_i18n_format_plural(NYA_STRING_%s, (s64)a0", name);
        } else {
            nya_string_extend_sprintf(out, ") {\n    return _nya_i18n_format(NYA_STRING_%s", name);
        }

        for (u32 argument = 0; argument < keys[i].argument_count; argument++) nya_string_extend_sprintf(out, ", a%u", argument);

        nya_string_extend(out, ");\n}\n\n");
    }

    NYA_EXPECT(nya_file_write(NYA_I18N_OUTPUT, out), "while writing the generated strings header");

    // Formatted like the asset index is, and for the same reason: it is a header a human reads when they want to know what keys exist.
    NYA_Command format_command = {
        .program   = "clang-format",
        .arguments = { "-i", NYA_I18N_OUTPUT },
    };
    NYA_EXPECT(nya_command_run(&format_command));

    nya_log_info("Generated %s: %u keys across %llu locales.", NYA_I18N_OUTPUT, key_count, (unsigned long long)locales->length);
}

/* PRIVATE API IMPLEMENTATION */

b8 _nya_i18n_parse_specifiers(NYA_ConstCString format, NYA_ConstCString where, NYA_ConstCString key, OUT NYA_I18nKey* out_key) {
    nya_unused(where, key);

    u32 count = 0;

    for (const char* cursor = format; *cursor != '\0'; cursor++) {
        if (*cursor != '%') continue;

        cursor++;

        // `%%` is a literal percent and takes no argument.
        if (*cursor == '%') continue;
        if (*cursor == '\0') return false;

        /* A positional prefix such as `2$` is skipped. */
        const char* digits = cursor;
        while (*cursor >= '0' && *cursor <= '9') cursor++;
        if (*cursor == '$') cursor++;
        else cursor = digits;

        // Width, precision and flags, none of which change the argument's type.
        while (*cursor == '-' || *cursor == '+' || *cursor == ' ' || *cursor == '#' || *cursor == '0') cursor++;
        while ((*cursor >= '0' && *cursor <= '9') || *cursor == '.') cursor++;

        if (count >= NYA_I18N_MAX_ARGUMENTS) return false;

        /* Only four kinds, deliberately. */
        switch (*cursor) {
            case 's': out_key->argument_types[count] = "NYA_ConstCString"; break;
            case 'd':
            case 'i': out_key->argument_types[count] = "s32"; break;
            case 'u': out_key->argument_types[count] = "u32"; break;
            case 'f':
            case 'g': out_key->argument_types[count] = "f64"; break;
            default:  return false;
        }

        // normalised, so `%i` matches `%d` and `%g` matches `%f`. A translator swapping them is not an error.
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

void _nya_i18n_plural_key(NYA_Object* object, NYA_ConstCString where, NYA_ConstCString key, OUT NYA_I18nKey* out_key) {
    static const NYA_ConstCString category_names[] = { "zero", "one", "two", "few", "many", "other" };

    // `other` is the variant every plural language has, and the one the accessor's arguments come from.
    NYA_Value* other = nya_object_get(object, "other");
    if (other == nullptr || other->type != NYA_TYPE_STRING) {
        nya_log_panic("i18n: %s key '%s' is a plural message but has no 'other' variant", where, key);
    }

    if (!_nya_i18n_parse_specifiers(other->as_string, where, key, out_key)) {
        nya_log_panic("i18n: %s key '%s' variant 'other' uses an unsupported format specifier", where, key);
    }

    // The count the runtime selects on is the first argument, so it has to be an integer.
    if (out_key->argument_count == 0 || (out_key->specifiers[0] != 'd' && out_key->specifiers[0] != 'u')) {
        nya_log_panic("i18n: %s key '%s' is plural but its first argument is not an integer count (put a %%d or %%u first)", where, key);
    }

    char expected[NYA_I18N_MAX_ARGUMENTS + 1] = { 0 };
    (void)snprintf(expected, sizeof(expected), "%s", out_key->specifiers);
    _nya_i18n_sort_specifiers(expected);

    // Every variant present takes the same arguments as `other`, and no key may be one CLDR does not name: a typo like `"ohter"` would otherwise be dropped in silence and the language fall back.
    nya_dict_foreach_key (object, slot) {
        NYA_CString variant_name = *slot;
        if (variant_name[0] == '_') continue;

        b8 known = false;
        for (u64 c = 0; c < sizeof(category_names) / sizeof(category_names[0]); c++) {
            if (nya_string_equals(variant_name, category_names[c])) {
                known = true;
                break;
            }
        }

        if (!known) {
            nya_log_panic("i18n: %s key '%s' has variant '%s', which is not a CLDR plural category", where, key, variant_name);
        }

        NYA_Value* variant = nya_object_get(object, variant_name);
        if (variant == nullptr || variant->type != NYA_TYPE_STRING) {
            nya_log_panic("i18n: %s key '%s' variant '%s' is not a string", where, key, variant_name);
        }

        NYA_I18nKey variant_key = { .key = (NYA_CString)key };
        if (!_nya_i18n_parse_specifiers(variant->as_string, where, key, &variant_key)) {
            nya_log_panic("i18n: %s key '%s' variant '%s' uses an unsupported format specifier", where, key, variant_name);
        }

        char actual[NYA_I18N_MAX_ARGUMENTS + 1] = { 0 };
        (void)snprintf(actual, sizeof(actual), "%s", variant_key.specifiers);
        _nya_i18n_sort_specifiers(actual);

        if (!nya_string_equals(expected, actual)) {
            nya_log_panic("i18n: %s key '%s' variant '%s' takes {%s} but 'other' takes {%s}", where, key, variant_name, actual, expected);
        }
    }
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
