/**
 * @file base_reflection.h
 *
 * One generated description per annotated type, and everything generic over a struct it was not
 * written for reads it: property panels, scene and save files, config reload, undo snapshots, debug
 * dumps. The alternative is a per-field conversion in each consumer, which is the code that rots.
 *
 * The tables are `const` data emitted by src/build/pp/reflection.c from `@reflect` comments in the
 * headers themselves. Nothing registers anything, nothing runs at startup, and the layout numbers are
 * `sizeof` and `offsetof` expressions the compiler evaluates rather than numbers the generator
 * guessed.
 *
 * ```
 * NYA_SceneEntity                  STRUCT
 *   position   f32x3               VECTOR    -> f32   PRIMITIVE  x3
 *   visual     NYA_SceneVisual     STRUCT    -> ...
 *     color    NYA_Color           STRUCT
 *       r      f32                 PRIMITIVE
 *   state      NYA_EntityState     ENUM      -> s32   PRIMITIVE, bitflags
 *   name       char[64]            ARRAY     -> char  PRIMITIVE, written as text
 * ```
 *
 * ```c
 * NYA_Object* document = nya_reflect_to_object(arena, nya_reflect_of(NYA_SettingsGraphics), &graphics);
 * NYA_TRY(nya_reflect_from_object(nya_reflect_of(NYA_SettingsGraphics), &graphics, document));
 *
 * // Or straight to and from a file, which is the same pair with the serde layer folded in.
 * NYA_TRY(nya_reflect_save_file(nya_reflect_of(NYA_SettingsGraphics), &graphics, path, NYA_SERDE_PRETTY));
 * ```
 *
 * ─────────────────────────────────────────────────────────
 * WHAT REFLECTION CANNOT DESCRIBE
 * ─────────────────────────────────────────────────────────
 *
 * Stated here rather than discovered as a field that silently never appears in a file. The generator
 * warns on the build's output for each of these when it meets one, naming the type and the field.
 *
 * - **Untagged unions.** A union without `@tag(field)` has no way to say which member is live, so
 *   nya_reflect_to_object writes nothing for it rather than writing an arbitrary member's bytes. Add
 *   `@tag`, or `@skip` the field and persist it by hand.
 * - **Bitfields.** `u32 flags : 3` has no address and no `offsetof`, so no field of one is described.
 *   Use a whole integer with `@flags(TheEnum)`.
 * - **Pointers.** Followed for nothing but `char*`, which is a string. A pointer is an address in one
 *   run of one process; anything a file has to name needs a name, a handle or an index instead.
 *   `void*`, callback handles and live physics bodies are therefore `@skip` territory, and the
 *   engine's own answer to that is a persistable projection type; see core_scene.h.
 * - **Anything of unknown length.** `NYA_Array`, a dictionary, a `T*` plus a count: a description
 *   carries one `element_count`, fixed at compile time, so only a real C array round trips. A list
 *   whose length is data is the caller's to write as an array of objects; see nya_scene_to_object.
 * - **Arrays indexed by an enum.** `f32 volumes[NYA_VOLUME_CHANNEL_COUNT]` is written as positions,
 *   not as names, so inserting a channel silently reinterprets an old file. Where the index *is* the
 *   meaning, use a struct with one named field per entry.
 * - **Types the generator never saw.** A field whose type carries no `@reflect` is skipped, with a
 *   warning naming it. Only the engine's own types describe engine types and only the game's describe
 *   the game's; see src/build/pp/reflection.h.
 * - **Anonymous structs and unions**, and `T *a, b;` declaring two different types in one statement.
 *   Both are rejected with a warning rather than half described.
 *
 * Two further behaviours that are choices rather than limits, and are relied on:
 *
 * - A field the document does not mention is left alone, not zeroed. That is what lets an old save
 *   load into a struct that has grown a field.
 * - A string longer than the `char[N]` it loads into is truncated on a character boundary rather
 *   than refused, since the array is the struct's own storage and running past it is not an option.
 *
 * ─────────────────────────────────────────────────────────
 * THE NAMES ARE PUBLIC, INCLUDING IN A SHIPPING BUILD
 * ─────────────────────────────────────────────────────────
 *
 * Every type, field and enum variant name below sits in `.rodata` verbatim in every build, so
 * `strings` on a shipping binary lists the layout of every described type. That is deliberate, and
 * it is worth saying plainly rather than leaving someone to discover it.
 *
 * The obvious fix does not work, so here is why, at the point somebody would try it. Lookups take a
 * string (nya_reflect_field, nya_reflect_path), so hashing the name and comparing hashes would hide
 * the *lookup* key. But the same `name` is also what gets written: nya_reflect_to_object passes it
 * straight to nya_object_add as the document key, and an enum's value is written as its variant's
 * name. A field's name *is* the key it appears under in settings, in saves and in
 * `assets/config/engine.nya`. So a shipping build with hashed names writes hashed keys, and that
 * costs three things worth more than the obfuscation is worth: settings stop being editable by the
 * person they belong to, a file written by a shipping build stops being readable by a development
 * build and by the config the game ships with, and the format stops being one that can be documented
 * and exported. Keeping a hash *beside* the name buys nothing, since the name has to stay.
 *
 * The size is not an argument either way: measured on this tree the names are 6220 bytes, 1339 of
 * them type names, which is the only group nothing writes out and so the only group a hash could
 * replace. That is under a kilobyte of a binary that is measured in megabytes.
 *
 * So the bar this raises is the one the save format raises, not one reflection raises. Save data is
 * written with NYA_SERDE_OBFUSCATE, which keeps a text editor from showing the keys of a save; see
 * core_save.h. Anything that must not be edited by the player it belongs to does not belong in a
 * file on their disk in the first place, and stays authoritative on a server, exactly as
 * base_integrity.h says about the anti-tamper checks.
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * What nya_reflect_to_object_redacted writes in place of a `@redact` field.
 *
 * One spelling, in one place, so a log is searchable for it and a test can assert on it. The angle
 * brackets are not something a name or a number could hold, so a caller who sends this exact text has
 * only managed to make their own request look redacted.
 * */
#define NYA_REFLECT_REDACTED "<redacted>"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

typedef enum NYA_ReflectKind        NYA_ReflectKind;
typedef enum NYA_ReflectHint        NYA_ReflectHint;
typedef struct NYA_ReflectField     NYA_ReflectField;
typedef struct NYA_ReflectVariant   NYA_ReflectVariant;
typedef struct NYA_TypeReflection   NYA_TypeReflection;

/** What `@on_apply` names. Runs once the plain fields of `instance` have been written. */
typedef NYA_Error (*NYA_ReflectApplyFn)(void* instance);

/**
 * What a described type *is*, which selects which members of NYA_TypeReflection mean anything.
 * */
enum NYA_ReflectKind {
    /** A type NYA_Value can hold directly. `primitive` says which. The base case of every walk. */
    NYA_REFLECT_PRIMITIVE,

    /** `fields` and `field_count`. */
    NYA_REFLECT_STRUCT,

    /** `fields` and `field_count`, plus `tag_field` when it is safe to read. See the note above. */
    NYA_REFLECT_UNION,

    /** `variants`, `variant_count`, `primitive` for the underlying integer, and `is_bitflags`. */
    NYA_REFLECT_ENUM,

    /** A C array, `char name[32]`. `element` and `element_count`. */
    NYA_REFLECT_ARRAY,

    /**
     * A clang extended vector, which is what f32x3 and friends are.
     * */
    NYA_REFLECT_VECTOR,

    /** `element` points at the pointee. Null `element` means `void*`. */
    NYA_REFLECT_POINTER,

    NYA_REFLECT_COUNT,
};

/**
 * What a field *means*, where its type does not say.
 * */
enum NYA_ReflectHint {
    NYA_HINT_NONE,

    NYA_HINT_POSITION,
    NYA_HINT_SCALE,
    NYA_HINT_EULER,
    NYA_HINT_COLOR,

    /** A string naming an asset, so the editor offers a picker rather than a text box. */
    NYA_HINT_ASSET,

    /** An integer whose bits are the flags of the enum `element` names. */
    NYA_HINT_BITFLAGS,

    NYA_HINT_COUNT,
};

/** One member of a struct or union. */
struct NYA_ReflectField {
    NYA_ConstCString name;

    const NYA_TypeReflection* type;

    /**
     * Bytes from the start of the containing type, as the compiler computed it.
     * */
    u64 offset;

    NYA_ReflectHint hint;

    /**
     * `@key` on the field: this is the type's primary key, at most one per type.
     *
     * Nothing in this module reads it. An annotation is a fact about the source and this table is
     * where facts about the source land, so the flag lives here and the consumer that acts on it is
     * elsewhere; the ORM in db/db_orm.h is the one that exists.
     * */
    b8 is_key;

    /**
     * `@redact` on the field: its content is a secret, and nothing that writes this type for a person
     * to read may write it.
     *
     * A fact about the source, like `is_key`, and acted on by nya_reflect_to_object_redacted, which is
     * what every logging and dumping path goes through. It does not change what
     * nya_reflect_to_object writes, because that is the conversion a response body and a save file are
     * made of and redacting there would be redacting the answer itself.
     * */
    b8 is_redacted;

    /**
     * `@secret` on the field: its value is written encrypted and read back decrypted, so a save file
     * holds ciphertext where the field's plaintext would be.
     *
     * Where `@redact` masks the value in a *log* and still writes its plaintext to a file, `@secret`
     * round-trips: the reflected save path (nya_reflect_save_file_secret) turns the value into a sealed
     * base64 string on the way out and back into the value on the way in, and refuses to write the
     * field at all when no key is given rather than leaking it in the clear. It is the stronger of the
     * two, so a `@secret` field is also masked by nya_reflect_to_object_redacted: a secret has no
     * business in a log whether or not it is encrypted at rest. See serde_reflect.h for the codec that
     * threads the key through, and crypto_seal.h for the box it is sealed in.
     * */
    b8 is_secret;

    /**
     * For a member of a tagged union: the value of the tag that selects this member.
     * */
    b8  has_tag_value;
    s64 tag_value;
};

/** One variant of an enum. */
struct NYA_ReflectVariant {
    NYA_ConstCString name;

    /**
     * Signed, so an enum with negative variants is describable.
     * */
    s64 value;
};

/** Everything known about one type. Which members apply is decided by `kind`. */
struct NYA_TypeReflection {
    /** As written in the source: "NYA_Entity", "f32x3", "GNY_EntityType". */
    NYA_ConstCString name;

    NYA_ReflectKind kind;

    /** `sizeof` and `alignof`, evaluated by the compiler. See the header note. */
    u64 size;
    u64 alignment;

    /** NYA_REFLECT_PRIMITIVE: which one. NYA_REFLECT_ENUM: the underlying integer type. */
    NYA_Type primitive;

    /* ── struct and union ── */

    const NYA_ReflectField* fields;
    u32                     field_count;

    /**
     * Which field discriminates a union, or null.
     * */
    const NYA_ReflectField* tag_field;

    /* ── enum ── */

    const NYA_ReflectVariant* variants;
    u32                       variant_count;

    /** Whether the variants are `1 << n` flags to be tested rather than values to be matched. */
    b8 is_bitflags;

    /* ── array, vector and pointer ── */

    const NYA_TypeReflection* element;

    /** Elements in an array or vector. Zero for a pointer. */
    u32 element_count;

    /* ── escape hatch ── */

    /**
     * Called after nya_reflect_from_object has written every field, or null.
     * */
    NYA_ReflectApplyFn on_apply;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The reflection for `type`, by its bare name: `nya_reflect_of(NYA_Entity)`.
 * */
#define nya_reflect_of(type) (&_NYA_REFLECT_##type)

/** The field called `name`, or null. Does not search into nested structs; see nya_reflect_path. */
NYA_API const NYA_ReflectField* nya_reflect_field(const NYA_TypeReflection* type, NYA_ConstCString name) __attr_no_discard;

/** The field at a dotted path such as `"visual.color.r"`, resolving through nested structs. */
NYA_API const NYA_ReflectField* nya_reflect_path(const NYA_TypeReflection* type, NYA_ConstCString path, void* instance,
                                                OUT void** out_instance) __attr_no_discard;

/** The address of `field` within `instance`. Offset arithmetic, kept in one place. */
NYA_API void* nya_reflect_field_pointer(void* instance, const NYA_ReflectField* field) __attr_no_discard;

/** The name of the variant with `value`, or null. For an enum. */
NYA_API NYA_ConstCString nya_reflect_variant_name(const NYA_TypeReflection* type, s64 value) __attr_no_discard;

/** The value of the variant called `name`. Fails rather than guessing, since zero is a real value. */
NYA_API b8 nya_reflect_variant_value(const NYA_TypeReflection* type, NYA_ConstCString name, OUT s64* out_value);

/**
 * Whether a described type is a `char[N]`, which is text rather than a list of numbers and is written
 * and read as such everywhere. The one place that decides it, since a consumer that decided otherwise
 * would write a field out in a shape nothing reads back.
 * */
NYA_API b8 nya_reflect_is_char_array(const NYA_TypeReflection* type) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * THE LAYOUT HASH
 * ─────────────────────────────────────────────────────────
 *
 * One number that changes whenever a document written from this type could be read wrongly by a build
 * whose type differs. The binary `.nya` header carries it, so a peer built from other headers is
 * refused by name instead of misread; see serde_nya_binary.h.
 *
 * What it covers, walked depth first from `type` in declaration order:
 *
 * - every type met: its kind, its `size` and its `alignment`
 * - a primitive, and an enum's underlying integer: the type's name ("u32"), not its NYA_Type number,
 *   so inserting a member into NYA_Type does not change every hash
 * - an enum: whether it is bitflags, and every variant's name and value, since a document carries
 *   the name and a renamed or renumbered variant reads as a different one
 * - a struct or union: every field's name, `offset` and tag value, then the field's type; and a
 *   union's tag field by name
 * - an array or vector: its element count, then the element type
 * - a pointer: only the pointee's kind and primitive, since reflection follows nothing but `char*`
 *   and a self referencing type would otherwise never end
 *
 * What it leaves out, because none of it changes what a document means: type names (renaming a
 * struct is not a layout change), hints, `is_key`, `is_redacted`, `is_secret` and `on_apply`. Tagging
 * a field `@redact` changes what a *log* holds, never what nya_reflect_to_object writes, so two builds
 * that disagree about it still read each other's documents. `@secret` is applied a layer up, in the
 * reflected save path, over the object this describes rather than in the wire encode the hash guards,
 * so it does not belong here either.
 *
 * Offsets and sizes are the compiler's, so a 32 bit peer disagrees with a 64 bit one over any type
 * holding a pointer, `char*` included. That is the intended strictness; a DTO meant for both carries
 * `char[N]` rather than a string pointer. FNV-1a 64 over an explicit little endian stream, so the
 * answer does not depend on the host's byte order, and the same on every run of every build.
 * */

/**
 * Deepest nesting of described types the hash walks. The described types in the tree nest a handful of
 * levels; the tables are generated and const, so going past this is a broken table and asserted.
 * */
#define NYA_REFLECT_LAYOUT_DEPTH_MAX 32

/** The layout hash of `type`. Pure, allocates nothing, walks the whole description on every call. */
NYA_API u64 nya_reflect_layout_hash(const NYA_TypeReflection* type) __attr_no_discard;

/**
 * The numeric content of a value, however it was spelled: every integer width and every boolean
 * widens, a char reads as its byte, and a whole number written with a decimal point truncates, so
 * `3.0` reads as `3`. False when the value holds nothing numeric at all.
 *
 * Public because it is what decides whether a value fits a field, and a second implementation of that
 * rule anywhere else would be a second answer. nya_reflect_write and the sqlite ORM both read it.
 * */
NYA_API b8 nya_reflect_value_to_s64(NYA_Value value, OUT s64* out_value);

/** The same for a real. Integers widen into one without complaint, since a hand written 1 must load. */
NYA_API b8 nya_reflect_value_to_f64(NYA_Value value, OUT f64* out_value);

/**
 * Reads one primitive field out of `instance` as an NYA_Value.
 * */
NYA_API NYA_Value nya_reflect_read(const NYA_TypeReflection* type, const void* instance) __attr_no_discard;

/** The inverse. Converts where safe: integers widen, floats never become integers. */
NYA_API b8 nya_reflect_write(const NYA_TypeReflection* type, void* instance, NYA_Value value);

/*
 * ─────────────────────────────────────────────────────────
 * THE GENERIC CONVERSION
 * ─────────────────────────────────────────────────────────
 */

/**
 * Any annotated type, as a self describing document.
 * */
NYA_API NYA_Object* nya_reflect_to_object(NYA_Arena* arena, const NYA_TypeReflection* type, const void* instance) __attr_no_discard;

/**
 * The inverse, in place.
 * */
NYA_API NYA_Error nya_reflect_from_object(const NYA_TypeReflection* type, void* instance, const NYA_Object* object) __attr_no_discard;

/**
 * The same document, with every `@redact` field written as NYA_REFLECT_REDACTED instead of its
 * content, at any depth and whatever the field's kind.
 *
 * This is what anything that turns a struct into text a person will read calls: a request log, a debug
 * dump, an IPC or net message written out. The secret is never copied — the walk substitutes rather
 * than overwriting afterwards — so there is no moment at which the arena holds it.
 *
 * It is a separate entry point rather than a flag on nya_reflect_to_object because the two have
 * opposite jobs: that one builds the answer a caller receives and the save a program reloads, and both
 * have to carry the field. Whoever writes for a reader chooses this one, once, and every sink
 * downstream of that choice is covered.
 * */
NYA_API NYA_Object* nya_reflect_to_object_redacted(NYA_Arena* arena, const NYA_TypeReflection* type, const void* instance) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────
 * CHECKING A DOCUMENT BEFORE IT IS APPLIED
 * ─────────────────────────────────────────────────────────
 *
 * nya_reflect_from_object skips what it cannot write, which is what keeps one bad line in a hand
 * edited file from costing the user the rest of it. Skipping in silence is the other half of the
 * problem, so nya_reflect_check walks the same document first and says exactly what it found.
 *
 * ```c
 * static void report(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data) {
 *     nya_log_warn("%s: '%s' is %s, expected %s; ignoring it.", (NYA_ConstCString)user_data, path, found, expected);
 * }
 *
 * (void)nya_reflect_check(nya_reflect_of(NYA_SettingsGraphics), document, report, NYA_SETTINGS_FILE);
 * NYA_TRY(nya_reflect_from_object(nya_reflect_of(NYA_SettingsGraphics), &graphics, document));
 * ```
 */

/** Longest dotted path a report carries, terminator included. Deep enough for any described tree; a document nested past it is reported at the depth that fits. */
#define NYA_REFLECT_PATH_MAX 256

/**
 * One problem found in a document. `path` is dotted from the root ("graphics.fov"), `found` describes
 * what the document holds there and `expected` what the type wanted. All three are only valid for the
 * duration of the call.
 * */
typedef void (*NYA_ReflectReportFn)(NYA_ConstCString path, NYA_ConstCString found, NYA_ConstCString expected, void* user_data);

/**
 * Walks `object` against `type` and reports every key that names no field and every value that
 * nya_reflect_from_object would refuse to write. Returns how many problems it found, so a caller that
 * only wants to know whether the document is clean need not install a reporter.
 *
 * Reads nothing and writes nothing: this is a check over the document alone, so it can run before any
 * instance is touched. Allocates nothing; the path is built in a NYA_REFLECT_PATH_MAX buffer.
 * */
NYA_API u32 nya_reflect_check(const NYA_TypeReflection* type, const NYA_Object* object, NYA_ReflectReportFn report, void* user_data);
