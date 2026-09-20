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
 * */
#pragma once

#include "nyangine/base/base_error.h"
#include "nyangine/base/base_object.h"
#include "nyangine/base/base_types.h"

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
