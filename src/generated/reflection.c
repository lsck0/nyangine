/* THIS FILE IS GENERATED. DO NYAT TOUCH. */

#include "nyangine/nyangine.h"

/*
 * Every size and offset below is an expression rather than a number, so the compiler that is
 * already compiling these structs is what computes the layout. See src/build/reflection.h.
 */

/* ── primitives ── */

const NYA_TypeReflection _NYA_REFLECT_b8 = { .name = "b8", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(b8), .alignment = alignof(b8), .primitive = NYA_TYPE_B8 };
const NYA_TypeReflection _NYA_REFLECT_b16 = { .name = "b16", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(b16), .alignment = alignof(b16), .primitive = NYA_TYPE_B16 };
const NYA_TypeReflection _NYA_REFLECT_b32 = { .name = "b32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(b32), .alignment = alignof(b32), .primitive = NYA_TYPE_B32 };
const NYA_TypeReflection _NYA_REFLECT_b64 = { .name = "b64", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(b64), .alignment = alignof(b64), .primitive = NYA_TYPE_B64 };
const NYA_TypeReflection _NYA_REFLECT_u8 = { .name = "u8", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(u8), .alignment = alignof(u8), .primitive = NYA_TYPE_U8 };
const NYA_TypeReflection _NYA_REFLECT_u16 = { .name = "u16", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(u16), .alignment = alignof(u16), .primitive = NYA_TYPE_U16 };
const NYA_TypeReflection _NYA_REFLECT_u32 = { .name = "u32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(u32), .alignment = alignof(u32), .primitive = NYA_TYPE_U32 };
const NYA_TypeReflection _NYA_REFLECT_u64 = { .name = "u64", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(u64), .alignment = alignof(u64), .primitive = NYA_TYPE_U64 };
const NYA_TypeReflection _NYA_REFLECT_s8 = { .name = "s8", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(s8), .alignment = alignof(s8), .primitive = NYA_TYPE_S8 };
const NYA_TypeReflection _NYA_REFLECT_s16 = { .name = "s16", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(s16), .alignment = alignof(s16), .primitive = NYA_TYPE_S16 };
const NYA_TypeReflection _NYA_REFLECT_s32 = { .name = "s32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(s32), .alignment = alignof(s32), .primitive = NYA_TYPE_S32 };
const NYA_TypeReflection _NYA_REFLECT_s64 = { .name = "s64", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(s64), .alignment = alignof(s64), .primitive = NYA_TYPE_S64 };
const NYA_TypeReflection _NYA_REFLECT_f32 = { .name = "f32", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(f32), .alignment = alignof(f32), .primitive = NYA_TYPE_F32 };
const NYA_TypeReflection _NYA_REFLECT_f64 = { .name = "f64", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(f64), .alignment = alignof(f64), .primitive = NYA_TYPE_F64 };
const NYA_TypeReflection _NYA_REFLECT_char = { .name = "char", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(char), .alignment = alignof(char), .primitive = NYA_TYPE_CHAR };

const NYA_TypeReflection _NYA_REFLECT_string = { .name = "string", .kind = NYA_REFLECT_PRIMITIVE, .size = sizeof(char*), .alignment = alignof(char*), .primitive = NYA_TYPE_STRING };

/* ── vectors ── */

const NYA_TypeReflection _NYA_REFLECT_f32x2 = { .name = "f32x2", .kind = NYA_REFLECT_VECTOR, .size = sizeof(f32x2), .alignment = alignof(f32x2), .element = &_NYA_REFLECT_f32, .element_count = 2 };
const NYA_TypeReflection _NYA_REFLECT_f32x3 = { .name = "f32x3", .kind = NYA_REFLECT_VECTOR, .size = sizeof(f32x3), .alignment = alignof(f32x3), .element = &_NYA_REFLECT_f32, .element_count = 3 };
const NYA_TypeReflection _NYA_REFLECT_f32x4 = { .name = "f32x4", .kind = NYA_REFLECT_VECTOR, .size = sizeof(f32x4), .alignment = alignof(f32x4), .element = &_NYA_REFLECT_f32, .element_count = 4 };

/* GNY_EntityKind — src/gnyame/entities/entities.h */

static const NYA_ReflectVariant _NYA_REFLECT_GNY_EntityKind_VARIANTS[] = {
    { .name = "GNY_ENTITY_NONE", .value = (s64)(GNY_ENTITY_NONE) },
    { .name = "GNY_ENTITY_TERRAIN", .value = (s64)(GNY_ENTITY_TERRAIN) },
    { .name = "GNY_ENTITY_BOX", .value = (s64)(GNY_ENTITY_BOX) },
    { .name = "GNY_ENTITY_CAMERA", .value = (s64)(GNY_ENTITY_CAMERA) },
    { .name = "GNY_ENTITY_CUBE3D", .value = (s64)(GNY_ENTITY_CUBE3D) },
    { .name = "GNY_ENTITY_TILEMAP", .value = (s64)(GNY_ENTITY_TILEMAP) },
    { .name = "GNY_ENTITY_PLAYER", .value = (s64)(GNY_ENTITY_PLAYER) },
    { .name = "GNY_ENTITY_KIND_COUNT", .value = (s64)(GNY_ENTITY_KIND_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_GNY_EntityKind = {
    .name = "GNY_EntityKind",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(GNY_EntityKind),
    .alignment = alignof(GNY_EntityKind),
    .primitive = (sizeof(GNY_EntityKind) == 8 ? NYA_TYPE_S64
                : sizeof(GNY_EntityKind) == 2 ? NYA_TYPE_S16
                : sizeof(GNY_EntityKind) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_GNY_EntityKind_VARIANTS,
    .variant_count = 8,
    .is_bitflags = false,
};

/* GNY_EntityFlags — src/gnyame/entities/entities.h */

static const NYA_ReflectVariant _NYA_REFLECT_GNY_EntityFlags_VARIANTS[] = {
    { .name = "GNY_ENTITY_FLAG_NONE", .value = (s64)(GNY_ENTITY_FLAG_NONE) },
    { .name = "GNY_ENTITY_FLAG_CULL_WHEN_LOST", .value = (s64)(GNY_ENTITY_FLAG_CULL_WHEN_LOST) },
    { .name = "GNY_ENTITY_FLAG_AUDIBLE", .value = (s64)(GNY_ENTITY_FLAG_AUDIBLE) },
    { .name = "GNY_ENTITY_FLAG_PLAYER_CONTROLLED", .value = (s64)(GNY_ENTITY_FLAG_PLAYER_CONTROLLED) },
    { .name = "GNY_ENTITY_FLAG_CAMERA_TARGET", .value = (s64)(GNY_ENTITY_FLAG_CAMERA_TARGET) },
    { .name = "GNY_ENTITY_FLAG_CAMERA_PRIMARY", .value = (s64)(GNY_ENTITY_FLAG_CAMERA_PRIMARY) },
};

const NYA_TypeReflection _NYA_REFLECT_GNY_EntityFlags = {
    .name = "GNY_EntityFlags",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(GNY_EntityFlags),
    .alignment = alignof(GNY_EntityFlags),
    .primitive = (sizeof(GNY_EntityFlags) == 8 ? NYA_TYPE_S64
                : sizeof(GNY_EntityFlags) == 2 ? NYA_TYPE_S16
                : sizeof(GNY_EntityFlags) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_GNY_EntityFlags_VARIANTS,
    .variant_count = 6,
    .is_bitflags = true,
};

/* NYA_NetChatMessage — src/nyangine/net/net_chat.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_NetChatMessage_name_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_NetChatMessage*)nullptr)->name),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_NET_MAX_NAME),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_NetChatMessage_text_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_NetChatMessage*)nullptr)->text),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_NET_CHAT_TEXT_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_NetChatMessage_FIELDS[] = {
    { .name = "sender", .type = &_NYA_REFLECT_NYA_NetPeerId, .offset = nya_offsetof(NYA_NetChatMessage, sender), .hint = NYA_HINT_NONE },
    { .name = "name", .type = &_NYA_REFLECT_NYA_NetChatMessage_name_ARRAY, .offset = nya_offsetof(NYA_NetChatMessage, name), .hint = NYA_HINT_NONE },
    { .name = "text", .type = &_NYA_REFLECT_NYA_NetChatMessage_text_ARRAY, .offset = nya_offsetof(NYA_NetChatMessage, text), .hint = NYA_HINT_NONE },
    { .name = "received_ms", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_NetChatMessage, received_ms), .hint = NYA_HINT_NONE },
    { .name = "is_system", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_NetChatMessage, is_system), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_NetChatMessage = {
    .name = "NYA_NetChatMessage",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_NetChatMessage),
    .alignment = alignof(NYA_NetChatMessage),
    .fields = _NYA_REFLECT_NYA_NetChatMessage_FIELDS,
    .field_count = 5,
};

/* NYA_NetPeerId — src/nyangine/net/net_types.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_NetPeerId_FIELDS[] = {
    { .name = "index", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_NetPeerId, index), .hint = NYA_HINT_NONE },
    { .name = "generation", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_NetPeerId, generation), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_NetPeerId = {
    .name = "NYA_NetPeerId",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_NetPeerId),
    .alignment = alignof(NYA_NetPeerId),
    .fields = _NYA_REFLECT_NYA_NetPeerId_FIELDS,
    .field_count = 2,
};

/* NYA_Color — src/nyangine/renderer/render_color.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_Color_FIELDS[] = {
    { .name = "r", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Color, r), .hint = NYA_HINT_NONE },
    { .name = "g", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Color, g), .hint = NYA_HINT_NONE },
    { .name = "b", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Color, b), .hint = NYA_HINT_NONE },
    { .name = "a", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Color, a), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_Color = {
    .name = "NYA_Color",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_Color),
    .alignment = alignof(NYA_Color),
    .fields = _NYA_REFLECT_NYA_Color_FIELDS,
    .field_count = 4,
};

const NYA_TypeReflection* const NYA_REFLECT_TYPES[NYA_REFLECT_TYPE_COUNT] = {
    &_NYA_REFLECT_GNY_EntityKind,
    &_NYA_REFLECT_GNY_EntityFlags,
    &_NYA_REFLECT_NYA_NetChatMessage,
    &_NYA_REFLECT_NYA_NetPeerId,
    &_NYA_REFLECT_NYA_Color,
};

const NYA_TypeReflection* nya_reflect_find(NYA_ConstCString name) {
    if (name == nullptr) return nullptr;

    for (u32 i = 0; i < NYA_REFLECT_TYPE_COUNT; i++) {
        if (nya_string_equals(NYA_REFLECT_TYPES[i]->name, name)) return NYA_REFLECT_TYPES[i];
    }

    return nullptr;
}
