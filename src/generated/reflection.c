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

/* GNY_ConfigRobots, src/gnyame/config.h */

static const NYA_ReflectField _NYA_REFLECT_GNY_ConfigRobots_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(GNY_ConfigRobots, enabled), .hint = NYA_HINT_NONE },
    { .name = "population", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(GNY_ConfigRobots, population), .hint = NYA_HINT_NONE },
    { .name = "generations_per_second", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(GNY_ConfigRobots, generations_per_second), .hint = NYA_HINT_NONE },
    { .name = "dqn_steps_per_second", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(GNY_ConfigRobots, dqn_steps_per_second), .hint = NYA_HINT_NONE },
    { .name = "show_brain", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(GNY_ConfigRobots, show_brain), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_GNY_ConfigRobots = {
    .name = "GNY_ConfigRobots",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(GNY_ConfigRobots),
    .alignment = alignof(GNY_ConfigRobots),
    .fields = _NYA_REFLECT_GNY_ConfigRobots_FIELDS,
    .field_count = 5,
};

/* GNY_ConfigGame, src/gnyame/config.h */

static const NYA_ReflectField _NYA_REFLECT_GNY_ConfigGame_FIELDS[] = {
    { .name = "player_speed", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(GNY_ConfigGame, player_speed), .hint = NYA_HINT_NONE },
    { .name = "player_spawn_spacing", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(GNY_ConfigGame, player_spawn_spacing), .hint = NYA_HINT_NONE },
    { .name = "animation_speed", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(GNY_ConfigGame, animation_speed), .hint = NYA_HINT_NONE },
    { .name = "robots", .type = &_NYA_REFLECT_GNY_ConfigRobots, .offset = nya_offsetof(GNY_ConfigGame, robots), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_GNY_ConfigGame = {
    .name = "GNY_ConfigGame",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(GNY_ConfigGame),
    .alignment = alignof(GNY_ConfigGame),
    .fields = _NYA_REFLECT_GNY_ConfigGame_FIELDS,
    .field_count = 4,
};

/* GNY_Config, src/gnyame/config.h */

static const NYA_ReflectField _NYA_REFLECT_GNY_Config_FIELDS[] = {
    { .name = "engine", .type = &_NYA_REFLECT_NYA_ConfigEngine, .offset = nya_offsetof(GNY_Config, engine), .hint = NYA_HINT_NONE },
    { .name = "game", .type = &_NYA_REFLECT_GNY_ConfigGame, .offset = nya_offsetof(GNY_Config, game), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_GNY_Config = {
    .name = "GNY_Config",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(GNY_Config),
    .alignment = alignof(GNY_Config),
    .fields = _NYA_REFLECT_GNY_Config_FIELDS,
    .field_count = 2,
};

/* GNY_EntityKind, src/gnyame/entities/entities.h */

static const NYA_ReflectVariant _NYA_REFLECT_GNY_EntityKind_VARIANTS[] = {
    { .name = "GNY_ENTITY_NONE", .value = (s64)(GNY_ENTITY_NONE) },
    { .name = "GNY_ENTITY_TERRAIN", .value = (s64)(GNY_ENTITY_TERRAIN) },
    { .name = "GNY_ENTITY_BOX", .value = (s64)(GNY_ENTITY_BOX) },
    { .name = "GNY_ENTITY_CAMERA", .value = (s64)(GNY_ENTITY_CAMERA) },
    { .name = "GNY_ENTITY_CUBE3D", .value = (s64)(GNY_ENTITY_CUBE3D) },
    { .name = "GNY_ENTITY_TILEMAP", .value = (s64)(GNY_ENTITY_TILEMAP) },
    { .name = "GNY_ENTITY_LEDGE", .value = (s64)(GNY_ENTITY_LEDGE) },
    { .name = "GNY_ENTITY_PLAYER", .value = (s64)(GNY_ENTITY_PLAYER) },
    { .name = "GNY_ENTITY_ROBOT", .value = (s64)(GNY_ENTITY_ROBOT) },
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
    .variant_count = 10,
    .is_bitflags = false,
};

/* GNY_EntityFlags, src/gnyame/entities/entities.h */

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

/* NYA_ConfigEngineRenderer, src/nyangine/core/core_config.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_ConfigEngineRenderer_grade_lut_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_ConfigEngineRenderer*)nullptr)->grade_lut),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_CONFIG_ASSET_PATH_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_ConfigEngineRenderer_FIELDS[] = {
    { .name = "msaa_samples", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_ConfigEngineRenderer, msaa_samples), .hint = NYA_HINT_NONE },
    { .name = "shadow_bias", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_ConfigEngineRenderer, shadow_bias), .hint = NYA_HINT_NONE },
    { .name = "shadow_cascades", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_ConfigEngineRenderer, shadow_cascades), .hint = NYA_HINT_NONE },
    { .name = "shadow_map_size", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_ConfigEngineRenderer, shadow_map_size), .hint = NYA_HINT_NONE },
    { .name = "ink", .type = &_NYA_REFLECT_NYA_PostInk, .offset = nya_offsetof(NYA_ConfigEngineRenderer, ink), .hint = NYA_HINT_NONE },
    { .name = "ambient_occlusion", .type = &_NYA_REFLECT_NYA_PostAmbientOcclusion, .offset = nya_offsetof(NYA_ConfigEngineRenderer, ambient_occlusion), .hint = NYA_HINT_NONE },
    { .name = "antialias", .type = &_NYA_REFLECT_NYA_PostAntialias, .offset = nya_offsetof(NYA_ConfigEngineRenderer, antialias), .hint = NYA_HINT_NONE },
    { .name = "depth_of_field", .type = &_NYA_REFLECT_NYA_PostDepthOfField, .offset = nya_offsetof(NYA_ConfigEngineRenderer, depth_of_field), .hint = NYA_HINT_NONE },
    { .name = "speed_lines", .type = &_NYA_REFLECT_NYA_PostSpeedLines, .offset = nya_offsetof(NYA_ConfigEngineRenderer, speed_lines), .hint = NYA_HINT_NONE },
    { .name = "decals", .type = &_NYA_REFLECT_NYA_Render3DDecals, .offset = nya_offsetof(NYA_ConfigEngineRenderer, decals), .hint = NYA_HINT_NONE },
    { .name = "output", .type = &_NYA_REFLECT_NYA_RenderOutput, .offset = nya_offsetof(NYA_ConfigEngineRenderer, output), .hint = NYA_HINT_NONE },
    { .name = "debug_view", .type = &_NYA_REFLECT_NYA_PostDebugView, .offset = nya_offsetof(NYA_ConfigEngineRenderer, debug_view), .hint = NYA_HINT_NONE },
    { .name = "shadow_color", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_ConfigEngineRenderer, shadow_color), .hint = NYA_HINT_NONE },
    { .name = "grade_lut", .type = &_NYA_REFLECT_NYA_ConfigEngineRenderer_grade_lut_ARRAY, .offset = nya_offsetof(NYA_ConfigEngineRenderer, grade_lut), .hint = NYA_HINT_NONE },
    { .name = "grade_strength", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_ConfigEngineRenderer, grade_strength), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_ConfigEngineRenderer = {
    .name = "NYA_ConfigEngineRenderer",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_ConfigEngineRenderer),
    .alignment = alignof(NYA_ConfigEngineRenderer),
    .fields = _NYA_REFLECT_NYA_ConfigEngineRenderer_FIELDS,
    .field_count = 15,
};

/* NYA_ConfigEnginePhysics, src/nyangine/core/core_config.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_ConfigEnginePhysics_FIELDS[] = {
    { .name = "gravity", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_ConfigEnginePhysics, gravity), .hint = NYA_HINT_NONE },
    { .name = "sub_steps", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_ConfigEnginePhysics, sub_steps), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_ConfigEnginePhysics = {
    .name = "NYA_ConfigEnginePhysics",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_ConfigEnginePhysics),
    .alignment = alignof(NYA_ConfigEnginePhysics),
    .fields = _NYA_REFLECT_NYA_ConfigEnginePhysics_FIELDS,
    .field_count = 2,
};

/* NYA_ConfigEngine, src/nyangine/core/core_config.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_ConfigEngine_FIELDS[] = {
    { .name = "renderer", .type = &_NYA_REFLECT_NYA_ConfigEngineRenderer, .offset = nya_offsetof(NYA_ConfigEngine, renderer), .hint = NYA_HINT_NONE },
    { .name = "physics", .type = &_NYA_REFLECT_NYA_ConfigEnginePhysics, .offset = nya_offsetof(NYA_ConfigEngine, physics), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_ConfigEngine = {
    .name = "NYA_ConfigEngine",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_ConfigEngine),
    .alignment = alignof(NYA_ConfigEngine),
    .fields = _NYA_REFLECT_NYA_ConfigEngine_FIELDS,
    .field_count = 2,
};

/* NYA_NetChatMessage, src/nyangine/net/net_chat.h */

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

/* NYA_NetPeerId, src/nyangine/net/net_types.h */

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

/* NYA_Render3DDecals, src/nyangine/renderer/render3d_decal.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_Render3DDecals_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_Render3DDecals, enabled), .hint = NYA_HINT_NONE },
    { .name = "lift", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Render3DDecals, lift), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_Render3DDecals = {
    .name = "NYA_Render3DDecals",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_Render3DDecals),
    .alignment = alignof(NYA_Render3DDecals),
    .fields = _NYA_REFLECT_NYA_Render3DDecals_FIELDS,
    .field_count = 2,
};

/* NYA_Color, src/nyangine/renderer/render_color.h */

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

/* NYA_RenderOutput, src/nyangine/renderer/render_output.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_RenderOutput_FIELDS[] = {
    { .name = "hdr", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_RenderOutput, hdr), .hint = NYA_HINT_NONE },
    { .name = "peak", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_RenderOutput, peak), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_RenderOutput = {
    .name = "NYA_RenderOutput",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_RenderOutput),
    .alignment = alignof(NYA_RenderOutput),
    .fields = _NYA_REFLECT_NYA_RenderOutput_FIELDS,
    .field_count = 2,
};

/* NYA_PostInk, src/nyangine/renderer/render_post.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_PostInk_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_PostInk, enabled), .hint = NYA_HINT_NONE },
    { .name = "color", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_PostInk, color), .hint = NYA_HINT_NONE },
    { .name = "width", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostInk, width), .hint = NYA_HINT_NONE },
    { .name = "crease", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostInk, crease), .hint = NYA_HINT_NONE },
    { .name = "fade_start", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostInk, fade_start), .hint = NYA_HINT_NONE },
    { .name = "fade_end", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostInk, fade_end), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostInk = {
    .name = "NYA_PostInk",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PostInk),
    .alignment = alignof(NYA_PostInk),
    .fields = _NYA_REFLECT_NYA_PostInk_FIELDS,
    .field_count = 6,
};

/* NYA_PostAmbientOcclusion, src/nyangine/renderer/render_post.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_PostAmbientOcclusion_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_PostAmbientOcclusion, enabled), .hint = NYA_HINT_NONE },
    { .name = "radius", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostAmbientOcclusion, radius), .hint = NYA_HINT_NONE },
    { .name = "strength", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostAmbientOcclusion, strength), .hint = NYA_HINT_NONE },
    { .name = "band", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostAmbientOcclusion, band), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostAmbientOcclusion = {
    .name = "NYA_PostAmbientOcclusion",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PostAmbientOcclusion),
    .alignment = alignof(NYA_PostAmbientOcclusion),
    .fields = _NYA_REFLECT_NYA_PostAmbientOcclusion_FIELDS,
    .field_count = 4,
};

/* NYA_PostAntialias, src/nyangine/renderer/render_post.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_PostAntialias_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_PostAntialias, enabled), .hint = NYA_HINT_NONE },
    { .name = "subpixel", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostAntialias, subpixel), .hint = NYA_HINT_NONE },
    { .name = "threshold", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostAntialias, threshold), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostAntialias = {
    .name = "NYA_PostAntialias",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PostAntialias),
    .alignment = alignof(NYA_PostAntialias),
    .fields = _NYA_REFLECT_NYA_PostAntialias_FIELDS,
    .field_count = 3,
};

/* NYA_PostFocus, src/nyangine/renderer/render_post.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_PostFocus_VARIANTS[] = {
    { .name = "NYA_POST_FOCUS_OFF", .value = (s64)(NYA_POST_FOCUS_OFF) },
    { .name = "NYA_POST_FOCUS_TILT_SHIFT", .value = (s64)(NYA_POST_FOCUS_TILT_SHIFT) },
    { .name = "NYA_POST_FOCUS_DISTANCE", .value = (s64)(NYA_POST_FOCUS_DISTANCE) },
    { .name = "NYA_POST_FOCUS_COUNT", .value = (s64)(NYA_POST_FOCUS_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostFocus = {
    .name = "NYA_PostFocus",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_PostFocus),
    .alignment = alignof(NYA_PostFocus),
    .primitive = (sizeof(NYA_PostFocus) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_PostFocus) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_PostFocus) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_PostFocus_VARIANTS,
    .variant_count = 4,
    .is_bitflags = false,
};

/* NYA_PostDepthOfField, src/nyangine/renderer/render_post.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_PostDepthOfField_FIELDS[] = {
    { .name = "focus", .type = &_NYA_REFLECT_NYA_PostFocus, .offset = nya_offsetof(NYA_PostDepthOfField, focus), .hint = NYA_HINT_NONE },
    { .name = "band_offset", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostDepthOfField, band_offset), .hint = NYA_HINT_NONE },
    { .name = "band", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostDepthOfField, band), .hint = NYA_HINT_NONE },
    { .name = "focus_distance", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostDepthOfField, focus_distance), .hint = NYA_HINT_NONE },
    { .name = "focus_range", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostDepthOfField, focus_range), .hint = NYA_HINT_NONE },
    { .name = "falloff", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostDepthOfField, falloff), .hint = NYA_HINT_NONE },
    { .name = "radius", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostDepthOfField, radius), .hint = NYA_HINT_NONE },
    { .name = "layers", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_PostDepthOfField, layers), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostDepthOfField = {
    .name = "NYA_PostDepthOfField",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PostDepthOfField),
    .alignment = alignof(NYA_PostDepthOfField),
    .fields = _NYA_REFLECT_NYA_PostDepthOfField_FIELDS,
    .field_count = 8,
};

/* NYA_PostSpeedLines, src/nyangine/renderer/render_post.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_PostSpeedLines_FIELDS[] = {
    { .name = "amount", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostSpeedLines, amount), .hint = NYA_HINT_NONE },
    { .name = "center_x", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostSpeedLines, center_x), .hint = NYA_HINT_NONE },
    { .name = "center_y", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostSpeedLines, center_y), .hint = NYA_HINT_NONE },
    { .name = "density", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostSpeedLines, density), .hint = NYA_HINT_NONE },
    { .name = "clear_radius", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostSpeedLines, clear_radius), .hint = NYA_HINT_NONE },
    { .name = "color", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_PostSpeedLines, color), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostSpeedLines = {
    .name = "NYA_PostSpeedLines",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PostSpeedLines),
    .alignment = alignof(NYA_PostSpeedLines),
    .fields = _NYA_REFLECT_NYA_PostSpeedLines_FIELDS,
    .field_count = 6,
};

/* NYA_PostDebugView, src/nyangine/renderer/render_post.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_PostDebugView_VARIANTS[] = {
    { .name = "NYA_POST_DEBUG_VIEW_NONE", .value = (s64)(NYA_POST_DEBUG_VIEW_NONE) },
    { .name = "NYA_POST_DEBUG_VIEW_NORMALS", .value = (s64)(NYA_POST_DEBUG_VIEW_NORMALS) },
    { .name = "NYA_POST_DEBUG_VIEW_DEPTH", .value = (s64)(NYA_POST_DEBUG_VIEW_DEPTH) },
    { .name = "NYA_POST_DEBUG_VIEW_OCCLUSION", .value = (s64)(NYA_POST_DEBUG_VIEW_OCCLUSION) },
    { .name = "NYA_POST_DEBUG_VIEW_INK", .value = (s64)(NYA_POST_DEBUG_VIEW_INK) },
    { .name = "NYA_POST_DEBUG_VIEW_CASCADES", .value = (s64)(NYA_POST_DEBUG_VIEW_CASCADES) },
    { .name = "NYA_POST_DEBUG_VIEW_COUNT", .value = (s64)(NYA_POST_DEBUG_VIEW_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostDebugView = {
    .name = "NYA_PostDebugView",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_PostDebugView),
    .alignment = alignof(NYA_PostDebugView),
    .primitive = (sizeof(NYA_PostDebugView) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_PostDebugView) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_PostDebugView) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_PostDebugView_VARIANTS,
    .variant_count = 7,
    .is_bitflags = false,
};

const NYA_TypeReflection* const NYA_REFLECT_TYPES[NYA_REFLECT_TYPE_COUNT] = {
    &_NYA_REFLECT_GNY_ConfigRobots,
    &_NYA_REFLECT_GNY_ConfigGame,
    &_NYA_REFLECT_GNY_Config,
    &_NYA_REFLECT_GNY_EntityKind,
    &_NYA_REFLECT_GNY_EntityFlags,
    &_NYA_REFLECT_NYA_ConfigEngineRenderer,
    &_NYA_REFLECT_NYA_ConfigEnginePhysics,
    &_NYA_REFLECT_NYA_ConfigEngine,
    &_NYA_REFLECT_NYA_NetChatMessage,
    &_NYA_REFLECT_NYA_NetPeerId,
    &_NYA_REFLECT_NYA_Render3DDecals,
    &_NYA_REFLECT_NYA_Color,
    &_NYA_REFLECT_NYA_RenderOutput,
    &_NYA_REFLECT_NYA_PostInk,
    &_NYA_REFLECT_NYA_PostAmbientOcclusion,
    &_NYA_REFLECT_NYA_PostAntialias,
    &_NYA_REFLECT_NYA_PostFocus,
    &_NYA_REFLECT_NYA_PostDepthOfField,
    &_NYA_REFLECT_NYA_PostSpeedLines,
    &_NYA_REFLECT_NYA_PostDebugView,
};

const NYA_TypeReflection* nya_reflect_find(NYA_ConstCString name) {
    if (name == nullptr) return nullptr;

    for (u32 i = 0; i < NYA_REFLECT_TYPE_COUNT; i++) {
        if (nya_string_equals(NYA_REFLECT_TYPES[i]->name, name)) return NYA_REFLECT_TYPES[i];
    }

    return nullptr;
}
