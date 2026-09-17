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

static const NYA_TypeReflection _NYA_REFLECT_GNY_ConfigGame_menu_sheet_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((GNY_ConfigGame*)nullptr)->menu_sheet),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_UI_SKIN_TEXTURE_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_GNY_ConfigGame_FIELDS[] = {
    { .name = "player_speed", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(GNY_ConfigGame, player_speed), .hint = NYA_HINT_NONE },
    { .name = "player_spawn_spacing", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(GNY_ConfigGame, player_spawn_spacing), .hint = NYA_HINT_NONE },
    { .name = "animation_speed", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(GNY_ConfigGame, animation_speed), .hint = NYA_HINT_NONE },
    { .name = "menu_sheet", .type = &_NYA_REFLECT_GNY_ConfigGame_menu_sheet_ARRAY, .offset = nya_offsetof(GNY_ConfigGame, menu_sheet), .hint = NYA_HINT_NONE },
    { .name = "robots", .type = &_NYA_REFLECT_GNY_ConfigRobots, .offset = nya_offsetof(GNY_ConfigGame, robots), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_GNY_ConfigGame = {
    .name = "GNY_ConfigGame",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(GNY_ConfigGame),
    .alignment = alignof(GNY_ConfigGame),
    .fields = _NYA_REFLECT_GNY_ConfigGame_FIELDS,
    .field_count = 5,
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

/* NYA_AudioPass, src/nyangine/core/core_audio_effects.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_AudioPass_FIELDS[] = {
    { .name = "lowpass_hz", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPass, lowpass_hz), .hint = NYA_HINT_NONE },
    { .name = "highpass_hz", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPass, highpass_hz), .hint = NYA_HINT_NONE },
    { .name = "resonance", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPass, resonance), .hint = NYA_HINT_NONE },
    { .name = "glide_ms", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPass, glide_ms), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AudioPass = {
    .name = "NYA_AudioPass",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AudioPass),
    .alignment = alignof(NYA_AudioPass),
    .fields = _NYA_REFLECT_NYA_AudioPass_FIELDS,
    .field_count = 4,
};

/* NYA_AudioEqualizer, src/nyangine/core/core_audio_effects.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_AudioEqualizer_FIELDS[] = {
    { .name = "low_db", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioEqualizer, low_db), .hint = NYA_HINT_NONE },
    { .name = "mid_db", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioEqualizer, mid_db), .hint = NYA_HINT_NONE },
    { .name = "high_db", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioEqualizer, high_db), .hint = NYA_HINT_NONE },
    { .name = "low_hz", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioEqualizer, low_hz), .hint = NYA_HINT_NONE },
    { .name = "mid_hz", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioEqualizer, mid_hz), .hint = NYA_HINT_NONE },
    { .name = "high_hz", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioEqualizer, high_hz), .hint = NYA_HINT_NONE },
    { .name = "mid_q", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioEqualizer, mid_q), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AudioEqualizer = {
    .name = "NYA_AudioEqualizer",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AudioEqualizer),
    .alignment = alignof(NYA_AudioEqualizer),
    .fields = _NYA_REFLECT_NYA_AudioEqualizer_FIELDS,
    .field_count = 7,
};

/* NYA_AudioCompressor, src/nyangine/core/core_audio_effects.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_AudioCompressor_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_AudioCompressor, enabled), .hint = NYA_HINT_NONE },
    { .name = "threshold_db", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioCompressor, threshold_db), .hint = NYA_HINT_NONE },
    { .name = "ratio", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioCompressor, ratio), .hint = NYA_HINT_NONE },
    { .name = "attack_ms", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioCompressor, attack_ms), .hint = NYA_HINT_NONE },
    { .name = "release_ms", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioCompressor, release_ms), .hint = NYA_HINT_NONE },
    { .name = "makeup_db", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioCompressor, makeup_db), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AudioCompressor = {
    .name = "NYA_AudioCompressor",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AudioCompressor),
    .alignment = alignof(NYA_AudioCompressor),
    .fields = _NYA_REFLECT_NYA_AudioCompressor_FIELDS,
    .field_count = 6,
};

/* NYA_AudioEcho, src/nyangine/core/core_audio_effects.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_AudioEcho_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_AudioEcho, enabled), .hint = NYA_HINT_NONE },
    { .name = "delay_ms", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioEcho, delay_ms), .hint = NYA_HINT_NONE },
    { .name = "feedback", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioEcho, feedback), .hint = NYA_HINT_NONE },
    { .name = "wet", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioEcho, wet), .hint = NYA_HINT_NONE },
    { .name = "lowpass_hz", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioEcho, lowpass_hz), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AudioEcho = {
    .name = "NYA_AudioEcho",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AudioEcho),
    .alignment = alignof(NYA_AudioEcho),
    .fields = _NYA_REFLECT_NYA_AudioEcho_FIELDS,
    .field_count = 5,
};

/* NYA_AudioReverb, src/nyangine/core/core_audio_effects.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_AudioReverb_FIELDS[] = {
    { .name = "room_size", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioReverb, room_size), .hint = NYA_HINT_NONE },
    { .name = "damping", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioReverb, damping), .hint = NYA_HINT_NONE },
    { .name = "wet", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioReverb, wet), .hint = NYA_HINT_NONE },
    { .name = "dry", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioReverb, dry), .hint = NYA_HINT_NONE },
    { .name = "width", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioReverb, width), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AudioReverb = {
    .name = "NYA_AudioReverb",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AudioReverb),
    .alignment = alignof(NYA_AudioReverb),
    .fields = _NYA_REFLECT_NYA_AudioReverb_FIELDS,
    .field_count = 5,
};

/* NYA_AudioLimiter, src/nyangine/core/core_audio_effects.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_AudioLimiter_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_AudioLimiter, enabled), .hint = NYA_HINT_NONE },
    { .name = "ceiling_db", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioLimiter, ceiling_db), .hint = NYA_HINT_NONE },
    { .name = "release_ms", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioLimiter, release_ms), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AudioLimiter = {
    .name = "NYA_AudioLimiter",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AudioLimiter),
    .alignment = alignof(NYA_AudioLimiter),
    .fields = _NYA_REFLECT_NYA_AudioLimiter_FIELDS,
    .field_count = 3,
};

/* NYA_AudioEffects, src/nyangine/core/core_audio_effects.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_AudioEffects_FIELDS[] = {
    { .name = "pass", .type = &_NYA_REFLECT_NYA_AudioPass, .offset = nya_offsetof(NYA_AudioEffects, pass), .hint = NYA_HINT_NONE },
    { .name = "equalizer", .type = &_NYA_REFLECT_NYA_AudioEqualizer, .offset = nya_offsetof(NYA_AudioEffects, equalizer), .hint = NYA_HINT_NONE },
    { .name = "compressor", .type = &_NYA_REFLECT_NYA_AudioCompressor, .offset = nya_offsetof(NYA_AudioEffects, compressor), .hint = NYA_HINT_NONE },
    { .name = "echo", .type = &_NYA_REFLECT_NYA_AudioEcho, .offset = nya_offsetof(NYA_AudioEffects, echo), .hint = NYA_HINT_NONE },
    { .name = "reverb", .type = &_NYA_REFLECT_NYA_AudioReverb, .offset = nya_offsetof(NYA_AudioEffects, reverb), .hint = NYA_HINT_NONE },
    { .name = "limiter", .type = &_NYA_REFLECT_NYA_AudioLimiter, .offset = nya_offsetof(NYA_AudioEffects, limiter), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AudioEffects = {
    .name = "NYA_AudioEffects",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AudioEffects),
    .alignment = alignof(NYA_AudioEffects),
    .fields = _NYA_REFLECT_NYA_AudioEffects_FIELDS,
    .field_count = 6,
};

/* NYA_AudioSpace, src/nyangine/core/core_audio_propagation.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_AudioSpace_VARIANTS[] = {
    { .name = "NYA_AUDIO_SPACE_3D", .value = (s64)(NYA_AUDIO_SPACE_3D) },
    { .name = "NYA_AUDIO_SPACE_2D", .value = (s64)(NYA_AUDIO_SPACE_2D) },
    { .name = "NYA_AUDIO_SPACE_COUNT", .value = (s64)(NYA_AUDIO_SPACE_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AudioSpace = {
    .name = "NYA_AudioSpace",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_AudioSpace),
    .alignment = alignof(NYA_AudioSpace),
    .primitive = (sizeof(NYA_AudioSpace) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_AudioSpace) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_AudioSpace) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_AudioSpace_VARIANTS,
    .variant_count = 3,
    .is_bitflags = false,
};

/* NYA_AudioPropagation, src/nyangine/core/core_audio_propagation.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_AudioPropagation_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_AudioPropagation, enabled), .hint = NYA_HINT_NONE },
    { .name = "space", .type = &_NYA_REFLECT_NYA_AudioSpace, .offset = nya_offsetof(NYA_AudioPropagation, space), .hint = NYA_HINT_NONE },
    { .name = "ray_budget", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_AudioPropagation, ray_budget), .hint = NYA_HINT_NONE },
    { .name = "voice_rays", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_AudioPropagation, voice_rays), .hint = NYA_HINT_NONE },
    { .name = "radius", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPropagation, radius), .hint = NYA_HINT_NONE },
    { .name = "lowpass_hz", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPropagation, lowpass_hz), .hint = NYA_HINT_NONE },
    { .name = "transmission", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPropagation, transmission), .hint = NYA_HINT_NONE },
    { .name = "thickness", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPropagation, thickness), .hint = NYA_HINT_NONE },
    { .name = "smoothing_ms", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPropagation, smoothing_ms), .hint = NYA_HINT_NONE },
    { .name = "diffraction", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_AudioPropagation, diffraction), .hint = NYA_HINT_NONE },
    { .name = "diffraction_reach", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPropagation, diffraction_reach), .hint = NYA_HINT_NONE },
    { .name = "environment", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_AudioPropagation, environment), .hint = NYA_HINT_NONE },
    { .name = "environment_range", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPropagation, environment_range), .hint = NYA_HINT_NONE },
    { .name = "reflections", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPropagation, reflections), .hint = NYA_HINT_NONE },
    { .name = "speed_of_sound", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_AudioPropagation, speed_of_sound), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_AudioPropagation = {
    .name = "NYA_AudioPropagation",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_AudioPropagation),
    .alignment = alignof(NYA_AudioPropagation),
    .fields = _NYA_REFLECT_NYA_AudioPropagation_FIELDS,
    .field_count = 15,
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
    { .name = "bloom", .type = &_NYA_REFLECT_NYA_PostBloom, .offset = nya_offsetof(NYA_ConfigEngineRenderer, bloom), .hint = NYA_HINT_NONE },
    { .name = "eye_adaptation", .type = &_NYA_REFLECT_NYA_PostEyeAdaptation, .offset = nya_offsetof(NYA_ConfigEngineRenderer, eye_adaptation), .hint = NYA_HINT_NONE },
    { .name = "light_shafts", .type = &_NYA_REFLECT_NYA_PostLightShafts, .offset = nya_offsetof(NYA_ConfigEngineRenderer, light_shafts), .hint = NYA_HINT_NONE },
    { .name = "fog", .type = &_NYA_REFLECT_NYA_Render3DFog, .offset = nya_offsetof(NYA_ConfigEngineRenderer, fog), .hint = NYA_HINT_NONE },
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
    .field_count = 19,
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

/* NYA_ConfigEngineAudio, src/nyangine/core/core_config.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_ConfigEngineAudio_FIELDS[] = {
    { .name = "propagation", .type = &_NYA_REFLECT_NYA_AudioPropagation, .offset = nya_offsetof(NYA_ConfigEngineAudio, propagation), .hint = NYA_HINT_NONE },
    { .name = "sound", .type = &_NYA_REFLECT_NYA_AudioEffects, .offset = nya_offsetof(NYA_ConfigEngineAudio, sound), .hint = NYA_HINT_NONE },
    { .name = "music", .type = &_NYA_REFLECT_NYA_AudioEffects, .offset = nya_offsetof(NYA_ConfigEngineAudio, music), .hint = NYA_HINT_NONE },
    { .name = "master", .type = &_NYA_REFLECT_NYA_AudioEffects, .offset = nya_offsetof(NYA_ConfigEngineAudio, master), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_ConfigEngineAudio = {
    .name = "NYA_ConfigEngineAudio",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_ConfigEngineAudio),
    .alignment = alignof(NYA_ConfigEngineAudio),
    .fields = _NYA_REFLECT_NYA_ConfigEngineAudio_FIELDS,
    .field_count = 4,
};

/* NYA_ConfigEngine, src/nyangine/core/core_config.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_ConfigEngine_FIELDS[] = {
    { .name = "renderer", .type = &_NYA_REFLECT_NYA_ConfigEngineRenderer, .offset = nya_offsetof(NYA_ConfigEngine, renderer), .hint = NYA_HINT_NONE },
    { .name = "physics", .type = &_NYA_REFLECT_NYA_ConfigEnginePhysics, .offset = nya_offsetof(NYA_ConfigEngine, physics), .hint = NYA_HINT_NONE },
    { .name = "audio", .type = &_NYA_REFLECT_NYA_ConfigEngineAudio, .offset = nya_offsetof(NYA_ConfigEngine, audio), .hint = NYA_HINT_NONE },
    { .name = "ui", .type = &_NYA_REFLECT_NYA_UIStyle, .offset = nya_offsetof(NYA_ConfigEngine, ui), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_ConfigEngine = {
    .name = "NYA_ConfigEngine",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_ConfigEngine),
    .alignment = alignof(NYA_ConfigEngine),
    .fields = _NYA_REFLECT_NYA_ConfigEngine_FIELDS,
    .field_count = 4,
};

/* NYA_EaseType, src/nyangine/math/math_tween.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_EaseType_VARIANTS[] = {
    { .name = "NYA_EASE_LINEAR", .value = (s64)(NYA_EASE_LINEAR) },
    { .name = "NYA_EASE_QUAD_IN", .value = (s64)(NYA_EASE_QUAD_IN) },
    { .name = "NYA_EASE_QUAD_OUT", .value = (s64)(NYA_EASE_QUAD_OUT) },
    { .name = "NYA_EASE_QUAD_IN_OUT", .value = (s64)(NYA_EASE_QUAD_IN_OUT) },
    { .name = "NYA_EASE_CUBIC_IN", .value = (s64)(NYA_EASE_CUBIC_IN) },
    { .name = "NYA_EASE_CUBIC_OUT", .value = (s64)(NYA_EASE_CUBIC_OUT) },
    { .name = "NYA_EASE_CUBIC_IN_OUT", .value = (s64)(NYA_EASE_CUBIC_IN_OUT) },
    { .name = "NYA_EASE_QUART_IN", .value = (s64)(NYA_EASE_QUART_IN) },
    { .name = "NYA_EASE_QUART_OUT", .value = (s64)(NYA_EASE_QUART_OUT) },
    { .name = "NYA_EASE_QUART_IN_OUT", .value = (s64)(NYA_EASE_QUART_IN_OUT) },
    { .name = "NYA_EASE_QUINT_IN", .value = (s64)(NYA_EASE_QUINT_IN) },
    { .name = "NYA_EASE_QUINT_OUT", .value = (s64)(NYA_EASE_QUINT_OUT) },
    { .name = "NYA_EASE_QUINT_IN_OUT", .value = (s64)(NYA_EASE_QUINT_IN_OUT) },
    { .name = "NYA_EASE_SINE_IN", .value = (s64)(NYA_EASE_SINE_IN) },
    { .name = "NYA_EASE_SINE_OUT", .value = (s64)(NYA_EASE_SINE_OUT) },
    { .name = "NYA_EASE_SINE_IN_OUT", .value = (s64)(NYA_EASE_SINE_IN_OUT) },
    { .name = "NYA_EASE_EXPO_IN", .value = (s64)(NYA_EASE_EXPO_IN) },
    { .name = "NYA_EASE_EXPO_OUT", .value = (s64)(NYA_EASE_EXPO_OUT) },
    { .name = "NYA_EASE_EXPO_IN_OUT", .value = (s64)(NYA_EASE_EXPO_IN_OUT) },
    { .name = "NYA_EASE_CIRC_IN", .value = (s64)(NYA_EASE_CIRC_IN) },
    { .name = "NYA_EASE_CIRC_OUT", .value = (s64)(NYA_EASE_CIRC_OUT) },
    { .name = "NYA_EASE_CIRC_IN_OUT", .value = (s64)(NYA_EASE_CIRC_IN_OUT) },
    { .name = "NYA_EASE_BACK_IN", .value = (s64)(NYA_EASE_BACK_IN) },
    { .name = "NYA_EASE_BACK_OUT", .value = (s64)(NYA_EASE_BACK_OUT) },
    { .name = "NYA_EASE_BACK_IN_OUT", .value = (s64)(NYA_EASE_BACK_IN_OUT) },
    { .name = "NYA_EASE_ELASTIC_IN", .value = (s64)(NYA_EASE_ELASTIC_IN) },
    { .name = "NYA_EASE_ELASTIC_OUT", .value = (s64)(NYA_EASE_ELASTIC_OUT) },
    { .name = "NYA_EASE_ELASTIC_IN_OUT", .value = (s64)(NYA_EASE_ELASTIC_IN_OUT) },
    { .name = "NYA_EASE_BOUNCE_IN", .value = (s64)(NYA_EASE_BOUNCE_IN) },
    { .name = "NYA_EASE_BOUNCE_OUT", .value = (s64)(NYA_EASE_BOUNCE_OUT) },
    { .name = "NYA_EASE_BOUNCE_IN_OUT", .value = (s64)(NYA_EASE_BOUNCE_IN_OUT) },
    { .name = "NYA_EASE_SMOOTHSTEP", .value = (s64)(NYA_EASE_SMOOTHSTEP) },
    { .name = "NYA_EASE_SMOOTHERSTEP", .value = (s64)(NYA_EASE_SMOOTHERSTEP) },
    { .name = "NYA_EASE_COUNT", .value = (s64)(NYA_EASE_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_EaseType = {
    .name = "NYA_EaseType",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_EaseType),
    .alignment = alignof(NYA_EaseType),
    .primitive = (sizeof(NYA_EaseType) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_EaseType) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_EaseType) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_EaseType_VARIANTS,
    .variant_count = 34,
    .is_bitflags = false,
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

/* NYA_Render3DFog, src/nyangine/renderer/render3d.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_Render3DFog_FIELDS[] = {
    { .name = "color", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_Render3DFog, color), .hint = NYA_HINT_NONE },
    { .name = "density", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Render3DFog, density), .hint = NYA_HINT_NONE },
    { .name = "height_falloff", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Render3DFog, height_falloff), .hint = NYA_HINT_NONE },
    { .name = "height_base", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Render3DFog, height_base), .hint = NYA_HINT_NONE },
    { .name = "sun_amount", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Render3DFog, sun_amount), .hint = NYA_HINT_NONE },
    { .name = "aerial", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Render3DFog, aerial), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_Render3DFog = {
    .name = "NYA_Render3DFog",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_Render3DFog),
    .alignment = alignof(NYA_Render3DFog),
    .fields = _NYA_REFLECT_NYA_Render3DFog_FIELDS,
    .field_count = 6,
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
    { .name = "min_radius", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostAmbientOcclusion, min_radius), .hint = NYA_HINT_NONE },
    { .name = "strength", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostAmbientOcclusion, strength), .hint = NYA_HINT_NONE },
    { .name = "band", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostAmbientOcclusion, band), .hint = NYA_HINT_NONE },
    { .name = "softness", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostAmbientOcclusion, softness), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostAmbientOcclusion = {
    .name = "NYA_PostAmbientOcclusion",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PostAmbientOcclusion),
    .alignment = alignof(NYA_PostAmbientOcclusion),
    .fields = _NYA_REFLECT_NYA_PostAmbientOcclusion_FIELDS,
    .field_count = 6,
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
    { .name = "motion", .type = &_NYA_REFLECT_f32x3, .offset = nya_offsetof(NYA_PostSpeedLines, motion), .hint = NYA_HINT_NONE },
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
    .field_count = 7,
};

/* NYA_PostBloom, src/nyangine/renderer/render_post.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_PostBloom_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_PostBloom, enabled), .hint = NYA_HINT_NONE },
    { .name = "threshold", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostBloom, threshold), .hint = NYA_HINT_NONE },
    { .name = "intensity", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostBloom, intensity), .hint = NYA_HINT_NONE },
    { .name = "spread", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostBloom, spread), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostBloom = {
    .name = "NYA_PostBloom",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PostBloom),
    .alignment = alignof(NYA_PostBloom),
    .fields = _NYA_REFLECT_NYA_PostBloom_FIELDS,
    .field_count = 4,
};

/* NYA_PostEyeAdaptation, src/nyangine/renderer/render_post.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_PostEyeAdaptation_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_PostEyeAdaptation, enabled), .hint = NYA_HINT_NONE },
    { .name = "key", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostEyeAdaptation, key), .hint = NYA_HINT_NONE },
    { .name = "exposure_min", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostEyeAdaptation, exposure_min), .hint = NYA_HINT_NONE },
    { .name = "exposure_max", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostEyeAdaptation, exposure_max), .hint = NYA_HINT_NONE },
    { .name = "dark_seconds", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostEyeAdaptation, dark_seconds), .hint = NYA_HINT_NONE },
    { .name = "bright_seconds", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostEyeAdaptation, bright_seconds), .hint = NYA_HINT_NONE },
    { .name = "saturation", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostEyeAdaptation, saturation), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostEyeAdaptation = {
    .name = "NYA_PostEyeAdaptation",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PostEyeAdaptation),
    .alignment = alignof(NYA_PostEyeAdaptation),
    .fields = _NYA_REFLECT_NYA_PostEyeAdaptation_FIELDS,
    .field_count = 7,
};

/* NYA_PostLightShafts, src/nyangine/renderer/render_post.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_PostLightShafts_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_PostLightShafts, enabled), .hint = NYA_HINT_NONE },
    { .name = "intensity", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostLightShafts, intensity), .hint = NYA_HINT_NONE },
    { .name = "length", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostLightShafts, length), .hint = NYA_HINT_NONE },
    { .name = "threshold", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostLightShafts, threshold), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostLightShafts = {
    .name = "NYA_PostLightShafts",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PostLightShafts),
    .alignment = alignof(NYA_PostLightShafts),
    .fields = _NYA_REFLECT_NYA_PostLightShafts_FIELDS,
    .field_count = 4,
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

/* NYA_UIOverflow, src/nyangine/ui/ui.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_UIOverflow_VARIANTS[] = {
    { .name = "NYA_UI_OVERFLOW_INHERIT", .value = (s64)(NYA_UI_OVERFLOW_INHERIT) },
    { .name = "NYA_UI_OVERFLOW_VISIBLE", .value = (s64)(NYA_UI_OVERFLOW_VISIBLE) },
    { .name = "NYA_UI_OVERFLOW_WRAP", .value = (s64)(NYA_UI_OVERFLOW_WRAP) },
    { .name = "NYA_UI_OVERFLOW_SHRINK", .value = (s64)(NYA_UI_OVERFLOW_SHRINK) },
    { .name = "NYA_UI_OVERFLOW_COUNT", .value = (s64)(NYA_UI_OVERFLOW_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_UIOverflow = {
    .name = "NYA_UIOverflow",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_UIOverflow),
    .alignment = alignof(NYA_UIOverflow),
    .primitive = (sizeof(NYA_UIOverflow) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_UIOverflow) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_UIOverflow) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_UIOverflow_VARIANTS,
    .variant_count = 5,
    .is_bitflags = false,
};

/* NYA_UIStateColors, src/nyangine/ui/ui.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_UIStateColors_FIELDS[] = {
    { .name = "normal", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_UIStateColors, normal), .hint = NYA_HINT_NONE },
    { .name = "focused", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_UIStateColors, focused), .hint = NYA_HINT_NONE },
    { .name = "pressed", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_UIStateColors, pressed), .hint = NYA_HINT_NONE },
    { .name = "disabled", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_UIStateColors, disabled), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_UIStateColors = {
    .name = "NYA_UIStateColors",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_UIStateColors),
    .alignment = alignof(NYA_UIStateColors),
    .fields = _NYA_REFLECT_NYA_UIStateColors_FIELDS,
    .field_count = 4,
};

/* NYA_UISkin, src/nyangine/ui/ui.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_UISkin_texture_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_UISkin*)nullptr)->texture),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_UI_SKIN_TEXTURE_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_UISkin_FIELDS[] = {
    { .name = "texture", .type = &_NYA_REFLECT_NYA_UISkin_texture_ARRAY, .offset = nya_offsetof(NYA_UISkin, texture), .hint = NYA_HINT_NONE },
    { .name = "source_x", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UISkin, source_x), .hint = NYA_HINT_NONE },
    { .name = "source_y", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UISkin, source_y), .hint = NYA_HINT_NONE },
    { .name = "source_width", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UISkin, source_width), .hint = NYA_HINT_NONE },
    { .name = "source_height", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UISkin, source_height), .hint = NYA_HINT_NONE },
    { .name = "left", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UISkin, left), .hint = NYA_HINT_NONE },
    { .name = "right", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UISkin, right), .hint = NYA_HINT_NONE },
    { .name = "top", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UISkin, top), .hint = NYA_HINT_NONE },
    { .name = "bottom", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UISkin, bottom), .hint = NYA_HINT_NONE },
    { .name = "tile", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_UISkin, tile), .hint = NYA_HINT_NONE },
    { .name = "hollow", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_UISkin, hollow), .hint = NYA_HINT_NONE },
    { .name = "tint", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_UISkin, tint), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_UISkin = {
    .name = "NYA_UISkin",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_UISkin),
    .alignment = alignof(NYA_UISkin),
    .fields = _NYA_REFLECT_NYA_UISkin_FIELDS,
    .field_count = 12,
};

/* NYA_UIStateSkins, src/nyangine/ui/ui.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_UIStateSkins_FIELDS[] = {
    { .name = "normal", .type = &_NYA_REFLECT_NYA_UISkin, .offset = nya_offsetof(NYA_UIStateSkins, normal), .hint = NYA_HINT_NONE },
    { .name = "focused", .type = &_NYA_REFLECT_NYA_UISkin, .offset = nya_offsetof(NYA_UIStateSkins, focused), .hint = NYA_HINT_NONE },
    { .name = "pressed", .type = &_NYA_REFLECT_NYA_UISkin, .offset = nya_offsetof(NYA_UIStateSkins, pressed), .hint = NYA_HINT_NONE },
    { .name = "disabled", .type = &_NYA_REFLECT_NYA_UISkin, .offset = nya_offsetof(NYA_UIStateSkins, disabled), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_UIStateSkins = {
    .name = "NYA_UIStateSkins",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_UIStateSkins),
    .alignment = alignof(NYA_UIStateSkins),
    .fields = _NYA_REFLECT_NYA_UIStateSkins_FIELDS,
    .field_count = 4,
};

/* NYA_UIStyle, src/nyangine/ui/ui.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_UIStyle_font_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_UIStyle*)nullptr)->font),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_UI_FONT_NAME_MAX),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_UIStyle_title_font_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_UIStyle*)nullptr)->title_font),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_UI_FONT_NAME_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_UIStyle_FIELDS[] = {
    { .name = "font", .type = &_NYA_REFLECT_NYA_UIStyle_font_ARRAY, .offset = nya_offsetof(NYA_UIStyle, font), .hint = NYA_HINT_NONE },
    { .name = "title_font", .type = &_NYA_REFLECT_NYA_UIStyle_title_font_ARRAY, .offset = nya_offsetof(NYA_UIStyle, title_font), .hint = NYA_HINT_NONE },
    { .name = "body_size", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, body_size), .hint = NYA_HINT_NONE },
    { .name = "small_size", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, small_size), .hint = NYA_HINT_NONE },
    { .name = "title_size", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, title_size), .hint = NYA_HINT_NONE },
    { .name = "scale", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, scale), .hint = NYA_HINT_NONE },
    { .name = "reference_height", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, reference_height), .hint = NYA_HINT_NONE },
    { .name = "margin", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, margin), .hint = NYA_HINT_NONE },
    { .name = "padding", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, padding), .hint = NYA_HINT_NONE },
    { .name = "spacing", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, spacing), .hint = NYA_HINT_NONE },
    { .name = "radius", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, radius), .hint = NYA_HINT_NONE },
    { .name = "outline", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, outline), .hint = NYA_HINT_NONE },
    { .name = "depth", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, depth), .hint = NYA_HINT_NONE },
    { .name = "pop", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, pop), .hint = NYA_HINT_NONE },
    { .name = "item_height", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, item_height), .hint = NYA_HINT_NONE },
    { .name = "overflow", .type = &_NYA_REFLECT_NYA_UIOverflow, .offset = nya_offsetof(NYA_UIStyle, overflow), .hint = NYA_HINT_NONE },
    { .name = "transition_s", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, transition_s), .hint = NYA_HINT_NONE },
    { .name = "appear_s", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, appear_s), .hint = NYA_HINT_NONE },
    { .name = "easing", .type = &_NYA_REFLECT_NYA_EaseType, .offset = nya_offsetof(NYA_UIStyle, easing), .hint = NYA_HINT_NONE },
    { .name = "scrim", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_UIStyle, scrim), .hint = NYA_HINT_NONE },
    { .name = "panel", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_UIStyle, panel), .hint = NYA_HINT_NONE },
    { .name = "ink", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_UIStyle, ink), .hint = NYA_HINT_NONE },
    { .name = "track", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_UIStyle, track), .hint = NYA_HINT_NONE },
    { .name = "accent", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_UIStyle, accent), .hint = NYA_HINT_NONE },
    { .name = "text_dim", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_UIStyle, text_dim), .hint = NYA_HINT_NONE },
    { .name = "button", .type = &_NYA_REFLECT_NYA_UIStateColors, .offset = nya_offsetof(NYA_UIStyle, button), .hint = NYA_HINT_NONE },
    { .name = "text", .type = &_NYA_REFLECT_NYA_UIStateColors, .offset = nya_offsetof(NYA_UIStyle, text), .hint = NYA_HINT_NONE },
    { .name = "panel_skin", .type = &_NYA_REFLECT_NYA_UISkin, .offset = nya_offsetof(NYA_UIStyle, panel_skin), .hint = NYA_HINT_NONE },
    { .name = "button_skin", .type = &_NYA_REFLECT_NYA_UIStateSkins, .offset = nya_offsetof(NYA_UIStyle, button_skin), .hint = NYA_HINT_NONE },
    { .name = "track_skin", .type = &_NYA_REFLECT_NYA_UISkin, .offset = nya_offsetof(NYA_UIStyle, track_skin), .hint = NYA_HINT_NONE },
    { .name = "knob_skin", .type = &_NYA_REFLECT_NYA_UISkin, .offset = nya_offsetof(NYA_UIStyle, knob_skin), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_UIStyle = {
    .name = "NYA_UIStyle",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_UIStyle),
    .alignment = alignof(NYA_UIStyle),
    .fields = _NYA_REFLECT_NYA_UIStyle_FIELDS,
    .field_count = 31,
};

const NYA_TypeReflection* const NYA_REFLECT_TYPES[NYA_REFLECT_TYPE_COUNT] = {
    &_NYA_REFLECT_GNY_ConfigRobots,
    &_NYA_REFLECT_GNY_ConfigGame,
    &_NYA_REFLECT_GNY_Config,
    &_NYA_REFLECT_GNY_EntityKind,
    &_NYA_REFLECT_GNY_EntityFlags,
    &_NYA_REFLECT_NYA_AudioPass,
    &_NYA_REFLECT_NYA_AudioEqualizer,
    &_NYA_REFLECT_NYA_AudioCompressor,
    &_NYA_REFLECT_NYA_AudioEcho,
    &_NYA_REFLECT_NYA_AudioReverb,
    &_NYA_REFLECT_NYA_AudioLimiter,
    &_NYA_REFLECT_NYA_AudioEffects,
    &_NYA_REFLECT_NYA_AudioSpace,
    &_NYA_REFLECT_NYA_AudioPropagation,
    &_NYA_REFLECT_NYA_ConfigEngineRenderer,
    &_NYA_REFLECT_NYA_ConfigEnginePhysics,
    &_NYA_REFLECT_NYA_ConfigEngineAudio,
    &_NYA_REFLECT_NYA_ConfigEngine,
    &_NYA_REFLECT_NYA_EaseType,
    &_NYA_REFLECT_NYA_NetChatMessage,
    &_NYA_REFLECT_NYA_NetPeerId,
    &_NYA_REFLECT_NYA_Render3DFog,
    &_NYA_REFLECT_NYA_Render3DDecals,
    &_NYA_REFLECT_NYA_Color,
    &_NYA_REFLECT_NYA_RenderOutput,
    &_NYA_REFLECT_NYA_PostInk,
    &_NYA_REFLECT_NYA_PostAmbientOcclusion,
    &_NYA_REFLECT_NYA_PostAntialias,
    &_NYA_REFLECT_NYA_PostFocus,
    &_NYA_REFLECT_NYA_PostDepthOfField,
    &_NYA_REFLECT_NYA_PostSpeedLines,
    &_NYA_REFLECT_NYA_PostBloom,
    &_NYA_REFLECT_NYA_PostEyeAdaptation,
    &_NYA_REFLECT_NYA_PostLightShafts,
    &_NYA_REFLECT_NYA_PostDebugView,
    &_NYA_REFLECT_NYA_UIOverflow,
    &_NYA_REFLECT_NYA_UIStateColors,
    &_NYA_REFLECT_NYA_UISkin,
    &_NYA_REFLECT_NYA_UIStateSkins,
    &_NYA_REFLECT_NYA_UIStyle,
};

const NYA_TypeReflection* nya_reflect_find(NYA_ConstCString name) {
    if (name == nullptr) return nullptr;

    for (u32 i = 0; i < NYA_REFLECT_TYPE_COUNT; i++) {
        if (nya_string_equals(NYA_REFLECT_TYPES[i]->name, name)) return NYA_REFLECT_TYPES[i];
    }

    return nullptr;
}
