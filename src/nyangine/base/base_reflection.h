/**
 * @file base_reflection.h
 *
 * ```
 * NYA_Entity                       STRUCT
 *   position   f32x3               VECTOR    -> f32   PRIMITIVE  x3
 *   visual     NYA_EntityVisual    STRUCT    -> ...
 *     color    NYA_Color           STRUCT
 *       r      f32                 PRIMITIVE
 *   type       GNY_EntityType      ENUM      -> u32   PRIMITIVE
 *   name       NYA_ConstCString    PRIMITIVE (NYA_TYPE_STRING)
 * ```
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

/**
 * The field at a dotted path — `"visual.color.r"` — resolving through nested structs.
 * */
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

/** The inverse. Converts where it safely can — an integer widens, a float does not become an integer. */
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
