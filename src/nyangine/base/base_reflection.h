/**
 * @file base_reflection.h
 *
 * Runtime type description, generated from `@reflect` annotations by src/build/reflection.c.
 *
 * ## What this is for
 *
 * One mechanism behind everything that has to be generic over a struct it was not written for: an
 * editor's property panel, a scene file, an undo snapshot, a debug dump. Each of those otherwise
 * needs code per field, which is the thing that rots.
 *
 * ## The shape
 *
 * A NYA_TypeReflection describes exactly one type, and a **struct member whose type is itself a
 * struct points at that type's own reflection** rather than flattening it. So walking a type is a
 * recursion that ends at NYA_REFLECT_PRIMITIVE, where NYA_Type says which primitive it is and
 * NYA_Value can hold it. That is the whole design: everything resolves to the primitives base_types.h
 * already names.
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
 *
 * ## Why the generator never computes a layout
 *
 * `offset` and `size` are emitted as `nya_offsetof(NYA_Entity, position)` and `sizeof(f32x3)` —
 * *source text*, evaluated by the compiler that is already compiling the struct. The generator
 * therefore has no model of padding, alignment or ABI, and cannot disagree with the real layout on a
 * platform it was never run on. It only has to know the field's **name** and its **type's spelling**.
 *
 * This is the single decision that makes a hand written parser sufficient. A generator that computed
 * offsets would be reimplementing a C ABI, which is not a build step, it is a compiler.
 *
 * ## What it deliberately does not do
 *
 * - **Untagged unions cannot be read.** The members are described, but nothing says which one is
 *   live. See NYA_TypeReflection.tag_field: a union is only safely readable when a `@tag` names the
 *   discriminant, which is what NYA_Value does with its own `type` member.
 * - **Bitfields are not described.** `nya_offsetof` does not apply to them, so there is no honest
 *   offset to emit. Annotate the struct's bitfields with `@skip`.
 * - **Nothing is allocated and nothing is registered at startup.** Every NYA_TypeReflection is a
 *   `const` object in generated data, so it costs image size and no runtime work.
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
 *
 * Separate from NYA_Type rather than added to it: NYA_Type names the things NYA_Value can hold, and a
 * struct is not one of them. Widening that enum would put a case into every switch over a serialised
 * value for the sake of a case that can never appear there.
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
     *
     * Like an array to read — element `i` sits at `i * element->size` — but **not** in its footprint:
     * `ext_vector_type(3)` of f32 has a size of sixteen, not twelve, because it is padded to a power
     * of two. So `size` is the real size and `element_count * element->size` is not, and anything
     * stepping over one of these must use the former to find the next.
     * */
    NYA_REFLECT_VECTOR,

    /** `element` points at the pointee. Null `element` means `void*`. */
    NYA_REFLECT_POINTER,

    NYA_REFLECT_COUNT,
};

/**
 * What a field *means*, where its type does not say.
 *
 * Three floats are three floats: nothing in the type distinguishes a position from a colour from a
 * pair of euler angles, and an editor drawing all of them as three spin boxes is the difference
 * between a property panel and a hex editor. Set from `@hint(...)` on the field.
 *
 * Presentation only. Nothing here changes how a value is read or written.
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
     *
     * See the header note: this is `nya_offsetof` evaluated at compile time, never a number the
     * generator worked out.
     * */
    u64 offset;

    NYA_ReflectHint hint;

    /**
     * For a member of a tagged union: the value of the tag that selects this member.
     *
     * Only meaningful when the containing type is a NYA_REFLECT_UNION with a `tag_field`, and only
     * when `has_tag_value` is set — zero is a legitimate tag value and cannot itself mean "unset".
     * */
    b8  has_tag_value;
    s64 tag_value;
};

/** One variant of an enum. */
struct NYA_ReflectVariant {
    NYA_ConstCString name;

    /**
     * Signed, so an enum with negative variants is describable.
     *
     * A bitflag enum's value is the flag itself, `1 << n`, not the index of the bit — which is what
     * lets a set of flags be decomposed by testing rather than by shifting.
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
     *
     * Points at a field of the **containing struct**, not of the union itself, because that is where
     * a tag lives in every arrangement worth supporting — NYA_Value's `type` sits beside its union,
     * not inside it. A union whose tag is null is describable but not readable; see the header.
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
     *
     * For the part of loading that is behaviour rather than data. An entity's `b2BodyId` cannot be
     * restored by writing bytes into it: the body has to be created in the Box2D world from the shape
     * and size that *were* restored. No annotation can express that, so `@on_apply(fn)` names a
     * function instead.
     *
     * A plain function pointer rather than an NYA_CallbackHandle, for two reasons that agree.
     *
     * Layering: base is compiled before core in the unity build, so nya_callback_get is not reachable
     * from here at all. And correctness: the handle exists to survive a hot reload, which this does
     * not need — the reflection table is `const` data generated into the same binary as the function
     * it names, so a reload replaces both together and a stale pointer cannot outlive its table.
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
 *
 * Resolved by the linker rather than by a lookup, so a misspelling is a link error and not a null at
 * runtime. The generator emits the `extern` for every annotated type into one header.
 * */
#define nya_reflect_of(type) (&_NYA_REFLECT_##type)

/** The field called `name`, or null. Does not search into nested structs; see nya_reflect_path. */
NYA_API const NYA_ReflectField* nya_reflect_field(const NYA_TypeReflection* type, NYA_ConstCString name) __attr_no_discard;

/**
 * The field at a dotted path — `"visual.color.r"` — resolving through nested structs.
 *
 * `out_instance` is advanced to the address of that field within `instance`, so the two answers a
 * caller needs come back together. Passing null for `instance` looks the path up without touching
 * memory, which is what a schema walk wants.
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
 *
 * Only for NYA_REFLECT_PRIMITIVE and NYA_REFLECT_ENUM fields — everything else is a composite and
 * belongs to nya_reflect_to_object. Returns a null-typed value for anything it cannot represent.
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
 *
 * The payoff, and what makes a hand written `nya_settings_to_object` unnecessary for anything
 * carrying `@reflect`: one implementation walks the fields, recurses into nested structs and arrays,
 * and writes each primitive as the NYA_Value it already maps to.
 *
 * Enums are written as their **variant name** rather than their number, so a saved file survives an
 * enum gaining a member in the middle — which is the same reasoning nya_settings_to_object gives for
 * writing input action names instead of their indices.
 * */
NYA_API NYA_Object* nya_reflect_to_object(NYA_Arena* arena, const NYA_TypeReflection* type, const void* instance) __attr_no_discard;

/**
 * The inverse, in place.
 *
 * **Partial by design.** A key the type does not have is ignored, and a field the object does not
 * mention is left alone rather than zeroed — so an older save loads into a newer struct and the new
 * fields keep whatever the caller had already put there. That is what makes this usable for an undo
 * step as well as for a file.
 *
 * Runs `on_apply` last when the type has one. See NYA_TypeReflection.on_apply for why bytes are not
 * always enough.
 * */
NYA_API NYA_Error nya_reflect_from_object(const NYA_TypeReflection* type, void* instance, const NYA_Object* object);
