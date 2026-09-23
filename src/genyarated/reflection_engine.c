/* THIS FILE IS GENERATED. DO NYAT TOUCH. */

#include "nyangine/nyangine.h"

#include "genyarated/reflection_engine.h"

/*
 * Every size and offset below is an expression rather than a number, so the compiler that is
 * already compiling these structs is what computes the layout. See src/build/pp/reflection.h.
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
    { .name = "features", .type = &_NYA_REFLECT_NYA_RenderFeatures, .offset = nya_offsetof(NYA_ConfigEngineRenderer, features), .hint = NYA_HINT_NONE },
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
    { .name = "motion_blur", .type = &_NYA_REFLECT_NYA_PostMotionBlur, .offset = nya_offsetof(NYA_ConfigEngineRenderer, motion_blur), .hint = NYA_HINT_NONE },
    { .name = "fog", .type = &_NYA_REFLECT_NYA_Render3DFog, .offset = nya_offsetof(NYA_ConfigEngineRenderer, fog), .hint = NYA_HINT_NONE },
    { .name = "haze", .type = &_NYA_REFLECT_NYA_Render2DHaze, .offset = nya_offsetof(NYA_ConfigEngineRenderer, haze), .hint = NYA_HINT_NONE },
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
    .field_count = 21,
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
    { .name = "http_log", .type = &_NYA_REFLECT_NYA_HttpLogConfig, .offset = nya_offsetof(NYA_ConfigEngine, http_log), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_ConfigEngine = {
    .name = "NYA_ConfigEngine",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_ConfigEngine),
    .alignment = alignof(NYA_ConfigEngine),
    .fields = _NYA_REFLECT_NYA_ConfigEngine_FIELDS,
    .field_count = 5,
};

/* NYA_ConfigDocument, src/nyangine/core/core_config.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_ConfigDocument_FIELDS[] = {
    { .name = "engine", .type = &_NYA_REFLECT_NYA_ConfigEngine, .offset = nya_offsetof(NYA_ConfigDocument, engine), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_ConfigDocument = {
    .name = "NYA_ConfigDocument",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_ConfigDocument),
    .alignment = alignof(NYA_ConfigDocument),
    .fields = _NYA_REFLECT_NYA_ConfigDocument_FIELDS,
    .field_count = 1,
};

/* NYA_EntityState, src/nyangine/core/core_entity.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_EntityState_VARIANTS[] = {
    { .name = "NYA_ENTITY_STATE_NONE", .value = (s64)(NYA_ENTITY_STATE_NONE) },
    { .name = "NYA_ENTITY_STATE_ACTIVE", .value = (s64)(NYA_ENTITY_STATE_ACTIVE) },
    { .name = "NYA_ENTITY_STATE_VISIBLE", .value = (s64)(NYA_ENTITY_STATE_VISIBLE) },
    { .name = "NYA_ENTITY_STATE_STATIC", .value = (s64)(NYA_ENTITY_STATE_STATIC) },
    { .name = "NYA_ENTITY_STATE_DESPAWNING", .value = (s64)(NYA_ENTITY_STATE_DESPAWNING) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_EntityState = {
    .name = "NYA_EntityState",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_EntityState),
    .alignment = alignof(NYA_EntityState),
    .primitive = (sizeof(NYA_EntityState) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_EntityState) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_EntityState) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_EntityState_VARIANTS,
    .variant_count = 5,
    .is_bitflags = true,
};

/* NYA_EntityVisualKind, src/nyangine/core/core_entity.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_EntityVisualKind_VARIANTS[] = {
    { .name = "NYA_ENTITY_VISUAL_NONE", .value = (s64)(NYA_ENTITY_VISUAL_NONE) },
    { .name = "NYA_ENTITY_VISUAL_SPRITE", .value = (s64)(NYA_ENTITY_VISUAL_SPRITE) },
    { .name = "NYA_ENTITY_VISUAL_ANIMATION", .value = (s64)(NYA_ENTITY_VISUAL_ANIMATION) },
    { .name = "NYA_ENTITY_VISUAL_CUBE", .value = (s64)(NYA_ENTITY_VISUAL_CUBE) },
    { .name = "NYA_ENTITY_VISUAL_KIND_COUNT", .value = (s64)(NYA_ENTITY_VISUAL_KIND_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_EntityVisualKind = {
    .name = "NYA_EntityVisualKind",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_EntityVisualKind),
    .alignment = alignof(NYA_EntityVisualKind),
    .primitive = (sizeof(NYA_EntityVisualKind) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_EntityVisualKind) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_EntityVisualKind) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_EntityVisualKind_VARIANTS,
    .variant_count = 5,
    .is_bitflags = false,
};

/* NYA_PluginPermission, src/nyangine/core/core_plugin.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_PluginPermission_VARIANTS[] = {
    { .name = "NYA_PLUGIN_PERMISSION_NONE", .value = (s64)(NYA_PLUGIN_PERMISSION_NONE) },
    { .name = "NYA_PLUGIN_PERMISSION_UI", .value = (s64)(NYA_PLUGIN_PERMISSION_UI) },
    { .name = "NYA_PLUGIN_PERMISSION_INPUT", .value = (s64)(NYA_PLUGIN_PERMISSION_INPUT) },
    { .name = "NYA_PLUGIN_PERMISSION_KEYBINDING", .value = (s64)(NYA_PLUGIN_PERMISSION_KEYBINDING) },
    { .name = "NYA_PLUGIN_PERMISSION_ENTITIES", .value = (s64)(NYA_PLUGIN_PERMISSION_ENTITIES) },
    { .name = "NYA_PLUGIN_PERMISSION_AUDIO", .value = (s64)(NYA_PLUGIN_PERMISSION_AUDIO) },
    { .name = "NYA_PLUGIN_PERMISSION_ASSETS", .value = (s64)(NYA_PLUGIN_PERMISSION_ASSETS) },
    { .name = "NYA_PLUGIN_PERMISSION_FILESYSTEM", .value = (s64)(NYA_PLUGIN_PERMISSION_FILESYSTEM) },
    { .name = "NYA_PLUGIN_PERMISSION_NETWORK", .value = (s64)(NYA_PLUGIN_PERMISSION_NETWORK) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PluginPermission = {
    .name = "NYA_PluginPermission",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_PluginPermission),
    .alignment = alignof(NYA_PluginPermission),
    .primitive = (sizeof(NYA_PluginPermission) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_PluginPermission) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_PluginPermission) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_PluginPermission_VARIANTS,
    .variant_count = 9,
    .is_bitflags = true,
};

/* NYA_PluginDependency, src/nyangine/core/core_plugin.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_PluginDependency_name_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_PluginDependency*)nullptr)->name),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_PLUGIN_NAME_MAX),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_PluginDependency_version_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_PluginDependency*)nullptr)->version),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_PLUGIN_VERSION_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_PluginDependency_FIELDS[] = {
    { .name = "name", .type = &_NYA_REFLECT_NYA_PluginDependency_name_ARRAY, .offset = nya_offsetof(NYA_PluginDependency, name), .hint = NYA_HINT_NONE },
    { .name = "version", .type = &_NYA_REFLECT_NYA_PluginDependency_version_ARRAY, .offset = nya_offsetof(NYA_PluginDependency, version), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PluginDependency = {
    .name = "NYA_PluginDependency",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PluginDependency),
    .alignment = alignof(NYA_PluginDependency),
    .fields = _NYA_REFLECT_NYA_PluginDependency_FIELDS,
    .field_count = 2,
};

/* NYA_PluginManifest, src/nyangine/core/core_plugin.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_PluginManifest_name_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_PluginManifest*)nullptr)->name),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_PLUGIN_NAME_MAX),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_PluginManifest_version_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_PluginManifest*)nullptr)->version),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_PLUGIN_VERSION_MAX),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_PluginManifest_engine_version_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_PluginManifest*)nullptr)->engine_version),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_PLUGIN_VERSION_MAX),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_PluginManifest_author_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_PluginManifest*)nullptr)->author),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_PLUGIN_AUTHOR_MAX),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_PluginManifest_license_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_PluginManifest*)nullptr)->license),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_PLUGIN_LICENSE_MAX),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_PluginManifest_description_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_PluginManifest*)nullptr)->description),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_PLUGIN_DESCRIPTION_MAX),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_PluginManifest_repository_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_PluginManifest*)nullptr)->repository),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_PLUGIN_URL_MAX),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_PluginManifest_dependencies_ARRAY = {
    .name = "NYA_PluginDependency[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_PluginManifest*)nullptr)->dependencies),
    .alignment = alignof(NYA_PluginDependency),
    .element = &_NYA_REFLECT_NYA_PluginDependency, .element_count = (NYA_PLUGIN_DEPENDENCY_MAX),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_PluginManifest_conflicts_ARRAY = {
    .name = "NYA_PluginDependency[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_PluginManifest*)nullptr)->conflicts),
    .alignment = alignof(NYA_PluginDependency),
    .element = &_NYA_REFLECT_NYA_PluginDependency, .element_count = (NYA_PLUGIN_DEPENDENCY_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_PluginManifest_FIELDS[] = {
    { .name = "name", .type = &_NYA_REFLECT_NYA_PluginManifest_name_ARRAY, .offset = nya_offsetof(NYA_PluginManifest, name), .hint = NYA_HINT_NONE },
    { .name = "version", .type = &_NYA_REFLECT_NYA_PluginManifest_version_ARRAY, .offset = nya_offsetof(NYA_PluginManifest, version), .hint = NYA_HINT_NONE },
    { .name = "engine_version", .type = &_NYA_REFLECT_NYA_PluginManifest_engine_version_ARRAY, .offset = nya_offsetof(NYA_PluginManifest, engine_version), .hint = NYA_HINT_NONE },
    { .name = "author", .type = &_NYA_REFLECT_NYA_PluginManifest_author_ARRAY, .offset = nya_offsetof(NYA_PluginManifest, author), .hint = NYA_HINT_NONE },
    { .name = "license", .type = &_NYA_REFLECT_NYA_PluginManifest_license_ARRAY, .offset = nya_offsetof(NYA_PluginManifest, license), .hint = NYA_HINT_NONE },
    { .name = "description", .type = &_NYA_REFLECT_NYA_PluginManifest_description_ARRAY, .offset = nya_offsetof(NYA_PluginManifest, description), .hint = NYA_HINT_NONE },
    { .name = "repository", .type = &_NYA_REFLECT_NYA_PluginManifest_repository_ARRAY, .offset = nya_offsetof(NYA_PluginManifest, repository), .hint = NYA_HINT_NONE },
    { .name = "permissions", .type = &_NYA_REFLECT_NYA_PluginPermission, .offset = nya_offsetof(NYA_PluginManifest, permissions), .hint = NYA_HINT_NONE },
    { .name = "dependencies", .type = &_NYA_REFLECT_NYA_PluginManifest_dependencies_ARRAY, .offset = nya_offsetof(NYA_PluginManifest, dependencies), .hint = NYA_HINT_NONE },
    { .name = "conflicts", .type = &_NYA_REFLECT_NYA_PluginManifest_conflicts_ARRAY, .offset = nya_offsetof(NYA_PluginManifest, conflicts), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PluginManifest = {
    .name = "NYA_PluginManifest",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PluginManifest),
    .alignment = alignof(NYA_PluginManifest),
    .fields = _NYA_REFLECT_NYA_PluginManifest_FIELDS,
    .field_count = 10,
};

/* NYA_SceneVisual, src/nyangine/core/core_scene.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_SceneVisual_sprite_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_SceneVisual*)nullptr)->sprite),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_SCENE_ASSET_MAX),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_SceneVisual_atlas_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_SceneVisual*)nullptr)->atlas),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_SCENE_ASSET_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_SceneVisual_FIELDS[] = {
    { .name = "kind", .type = &_NYA_REFLECT_NYA_EntityVisualKind, .offset = nya_offsetof(NYA_SceneVisual, kind), .hint = NYA_HINT_NONE },
    { .name = "sprite", .type = &_NYA_REFLECT_NYA_SceneVisual_sprite_ARRAY, .offset = nya_offsetof(NYA_SceneVisual, sprite), .hint = NYA_HINT_ASSET },
    { .name = "source", .type = &_NYA_REFLECT_f32x4, .offset = nya_offsetof(NYA_SceneVisual, source), .hint = NYA_HINT_NONE },
    { .name = "origin", .type = &_NYA_REFLECT_f32x2, .offset = nya_offsetof(NYA_SceneVisual, origin), .hint = NYA_HINT_NONE },
    { .name = "sprite_scale", .type = &_NYA_REFLECT_f32x2, .offset = nya_offsetof(NYA_SceneVisual, sprite_scale), .hint = NYA_HINT_SCALE },
    { .name = "sprite_rotation", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_SceneVisual, sprite_rotation), .hint = NYA_HINT_NONE },
    { .name = "flip_x", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_SceneVisual, flip_x), .hint = NYA_HINT_NONE },
    { .name = "flip_y", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_SceneVisual, flip_y), .hint = NYA_HINT_NONE },
    { .name = "tint", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_SceneVisual, tint), .hint = NYA_HINT_COLOR },
    { .name = "atlas", .type = &_NYA_REFLECT_NYA_SceneVisual_atlas_ARRAY, .offset = nya_offsetof(NYA_SceneVisual, atlas), .hint = NYA_HINT_ASSET },
    { .name = "frame_width", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_SceneVisual, frame_width), .hint = NYA_HINT_NONE },
    { .name = "frame_height", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_SceneVisual, frame_height), .hint = NYA_HINT_NONE },
    { .name = "columns", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_SceneVisual, columns), .hint = NYA_HINT_NONE },
    { .name = "rows", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_SceneVisual, rows), .hint = NYA_HINT_NONE },
    { .name = "spacing", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_SceneVisual, spacing), .hint = NYA_HINT_NONE },
    { .name = "margin", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_SceneVisual, margin), .hint = NYA_HINT_NONE },
    { .name = "size", .type = &_NYA_REFLECT_f32x3, .offset = nya_offsetof(NYA_SceneVisual, size), .hint = NYA_HINT_SCALE },
    { .name = "color", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_SceneVisual, color), .hint = NYA_HINT_COLOR },
    { .name = "z_order", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_SceneVisual, z_order), .hint = NYA_HINT_NONE },
    { .name = "y_sorted", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_SceneVisual, y_sorted), .hint = NYA_HINT_NONE },
    { .name = "y_sort_anchor", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_SceneVisual, y_sort_anchor), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_SceneVisual = {
    .name = "NYA_SceneVisual",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_SceneVisual),
    .alignment = alignof(NYA_SceneVisual),
    .fields = _NYA_REFLECT_NYA_SceneVisual_FIELDS,
    .field_count = 21,
};

/* NYA_SceneEntity, src/nyangine/core/core_scene.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_SceneEntity_name_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_SceneEntity*)nullptr)->name),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_SCENE_NAME_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_SceneEntity_FIELDS[] = {
    { .name = "id", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_SceneEntity, id), .hint = NYA_HINT_NONE },
    { .name = "parent_id", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_SceneEntity, parent_id), .hint = NYA_HINT_NONE },
    { .name = "parented", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_SceneEntity, parented), .hint = NYA_HINT_NONE },
    { .name = "name", .type = &_NYA_REFLECT_NYA_SceneEntity_name_ARRAY, .offset = nya_offsetof(NYA_SceneEntity, name), .hint = NYA_HINT_NONE },
    { .name = "type", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_SceneEntity, type), .hint = NYA_HINT_NONE },
    { .name = "flags", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_SceneEntity, flags), .hint = NYA_HINT_NONE },
    { .name = "state", .type = &_NYA_REFLECT_NYA_EntityState, .offset = nya_offsetof(NYA_SceneEntity, state), .hint = NYA_HINT_NONE },
    { .name = "position", .type = &_NYA_REFLECT_f32x3, .offset = nya_offsetof(NYA_SceneEntity, position), .hint = NYA_HINT_POSITION },
    { .name = "rotation", .type = &_NYA_REFLECT_NYA_Quaternion, .offset = nya_offsetof(NYA_SceneEntity, rotation), .hint = NYA_HINT_NONE },
    { .name = "scale", .type = &_NYA_REFLECT_f32x3, .offset = nya_offsetof(NYA_SceneEntity, scale), .hint = NYA_HINT_SCALE },
    { .name = "velocity", .type = &_NYA_REFLECT_f32x3, .offset = nya_offsetof(NYA_SceneEntity, velocity), .hint = NYA_HINT_NONE },
    { .name = "angular_velocity", .type = &_NYA_REFLECT_f32x3, .offset = nya_offsetof(NYA_SceneEntity, angular_velocity), .hint = NYA_HINT_NONE },
    { .name = "visual", .type = &_NYA_REFLECT_NYA_SceneVisual, .offset = nya_offsetof(NYA_SceneEntity, visual), .hint = NYA_HINT_NONE },
    { .name = "light", .type = &_NYA_REFLECT_NYA_Light2D, .offset = nya_offsetof(NYA_SceneEntity, light), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_SceneEntity = {
    .name = "NYA_SceneEntity",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_SceneEntity),
    .alignment = alignof(NYA_SceneEntity),
    .fields = _NYA_REFLECT_NYA_SceneEntity_FIELDS,
    .field_count = 14,
};

/* NYA_SettingsVolumes, src/nyangine/core/core_settings.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_SettingsVolumes_FIELDS[] = {
    { .name = "master", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_SettingsVolumes, master), .hint = NYA_HINT_NONE },
    { .name = "sound", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_SettingsVolumes, sound), .hint = NYA_HINT_NONE },
    { .name = "music", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_SettingsVolumes, music), .hint = NYA_HINT_NONE },
    { .name = "voice", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_SettingsVolumes, voice), .hint = NYA_HINT_NONE },
    { .name = "ui", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_SettingsVolumes, ui), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_SettingsVolumes = {
    .name = "NYA_SettingsVolumes",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_SettingsVolumes),
    .alignment = alignof(NYA_SettingsVolumes),
    .fields = _NYA_REFLECT_NYA_SettingsVolumes_FIELDS,
    .field_count = 5,
};

/* NYA_GraphicsQuality, src/nyangine/core/core_settings.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_GraphicsQuality_VARIANTS[] = {
    { .name = "NYA_GRAPHICS_QUALITY_OFF", .value = (s64)(NYA_GRAPHICS_QUALITY_OFF) },
    { .name = "NYA_GRAPHICS_QUALITY_LOW", .value = (s64)(NYA_GRAPHICS_QUALITY_LOW) },
    { .name = "NYA_GRAPHICS_QUALITY_MEDIUM", .value = (s64)(NYA_GRAPHICS_QUALITY_MEDIUM) },
    { .name = "NYA_GRAPHICS_QUALITY_HIGH", .value = (s64)(NYA_GRAPHICS_QUALITY_HIGH) },
    { .name = "NYA_GRAPHICS_QUALITY_COUNT", .value = (s64)(NYA_GRAPHICS_QUALITY_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_GraphicsQuality = {
    .name = "NYA_GraphicsQuality",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_GraphicsQuality),
    .alignment = alignof(NYA_GraphicsQuality),
    .primitive = (sizeof(NYA_GraphicsQuality) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_GraphicsQuality) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_GraphicsQuality) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_GraphicsQuality_VARIANTS,
    .variant_count = 5,
    .is_bitflags = false,
};

/* NYA_SettingsGraphics, src/nyangine/core/core_settings.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_SettingsGraphics_FIELDS[] = {
    { .name = "msaa_samples", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_SettingsGraphics, msaa_samples), .hint = NYA_HINT_NONE },
    { .name = "fxaa", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_SettingsGraphics, fxaa), .hint = NYA_HINT_NONE },
    { .name = "ambient_occlusion", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_SettingsGraphics, ambient_occlusion), .hint = NYA_HINT_NONE },
    { .name = "bloom", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_SettingsGraphics, bloom), .hint = NYA_HINT_NONE },
    { .name = "depth_of_field", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_SettingsGraphics, depth_of_field), .hint = NYA_HINT_NONE },
    { .name = "eye_adaptation", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_SettingsGraphics, eye_adaptation), .hint = NYA_HINT_NONE },
    { .name = "light_shafts", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_SettingsGraphics, light_shafts), .hint = NYA_HINT_NONE },
    { .name = "motion_blur", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_SettingsGraphics, motion_blur), .hint = NYA_HINT_NONE },
    { .name = "shadows", .type = &_NYA_REFLECT_NYA_GraphicsQuality, .offset = nya_offsetof(NYA_SettingsGraphics, shadows), .hint = NYA_HINT_NONE },
    { .name = "fov", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_SettingsGraphics, fov), .hint = NYA_HINT_NONE },
    { .name = "render_scale", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_SettingsGraphics, render_scale), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_SettingsGraphics = {
    .name = "NYA_SettingsGraphics",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_SettingsGraphics),
    .alignment = alignof(NYA_SettingsGraphics),
    .fields = _NYA_REFLECT_NYA_SettingsGraphics_FIELDS,
    .field_count = 11,
};

/* NYA_HttpMetricsDto, src/nyangine/debug/debug_metrics.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpMetricsDto_FIELDS[] = {
    { .name = "measured_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpMetricsDto, measured_at_s), .hint = NYA_HINT_NONE },
    { .name = "uptime_ns", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpMetricsDto, uptime_ns), .hint = NYA_HINT_NONE },
    { .name = "fps", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_HttpMetricsDto, fps), .hint = NYA_HINT_NONE },
    { .name = "delta_time_s", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_HttpMetricsDto, delta_time_s), .hint = NYA_HINT_NONE },
    { .name = "work_ns", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpMetricsDto, work_ns), .hint = NYA_HINT_NONE },
    { .name = "sleep_ns", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpMetricsDto, sleep_ns), .hint = NYA_HINT_NONE },
    { .name = "elapsed_ns", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpMetricsDto, elapsed_ns), .hint = NYA_HINT_NONE },
    { .name = "min_frame_time_ns", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpMetricsDto, min_frame_time_ns), .hint = NYA_HINT_NONE },
    { .name = "connection_count", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpMetricsDto, connection_count), .hint = NYA_HINT_NONE },
    { .name = "request_count", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpMetricsDto, request_count), .hint = NYA_HINT_NONE },
    { .name = "accounting_enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_HttpMetricsDto, accounting_enabled), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpMetricsDto = {
    .name = "NYA_HttpMetricsDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpMetricsDto),
    .alignment = alignof(NYA_HttpMetricsDto),
    .fields = _NYA_REFLECT_NYA_HttpMetricsDto_FIELDS,
    .field_count = 11,
};

/* NYA_HttpCeilingDto, src/nyangine/debug/debug_metrics.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpCeilingDto_name_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpCeilingDto*)nullptr)->name),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_METRICS_MAX_NAME),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpCeilingDto_FIELDS[] = {
    { .name = "name", .type = &_NYA_REFLECT_NYA_HttpCeilingDto_name_ARRAY, .offset = nya_offsetof(NYA_HttpCeilingDto, name), .hint = NYA_HINT_NONE },
    { .name = "capacity", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpCeilingDto, capacity), .hint = NYA_HINT_NONE },
    { .name = "live", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpCeilingDto, live), .hint = NYA_HINT_NONE },
    { .name = "fullness", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_HttpCeilingDto, fullness), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpCeilingDto = {
    .name = "NYA_HttpCeilingDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpCeilingDto),
    .alignment = alignof(NYA_HttpCeilingDto),
    .fields = _NYA_REFLECT_NYA_HttpCeilingDto_FIELDS,
    .field_count = 4,
};

/* NYA_HttpCeilingsDto, src/nyangine/debug/debug_metrics.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpCeilingsDto_rows_ARRAY = {
    .name = "NYA_HttpCeilingDto[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpCeilingsDto*)nullptr)->rows),
    .alignment = alignof(NYA_HttpCeilingDto),
    .element = &_NYA_REFLECT_NYA_HttpCeilingDto, .element_count = (NYA_HTTP_METRICS_MAX_ROWS),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpCeilingsDto_FIELDS[] = {
    { .name = "count", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpCeilingsDto, count), .hint = NYA_HINT_NONE },
    { .name = "truncated", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpCeilingsDto, truncated), .hint = NYA_HINT_NONE },
    { .name = "rows", .type = &_NYA_REFLECT_NYA_HttpCeilingsDto_rows_ARRAY, .offset = nya_offsetof(NYA_HttpCeilingsDto, rows), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpCeilingsDto = {
    .name = "NYA_HttpCeilingsDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpCeilingsDto),
    .alignment = alignof(NYA_HttpCeilingsDto),
    .fields = _NYA_REFLECT_NYA_HttpCeilingsDto_FIELDS,
    .field_count = 3,
};

/* NYA_HttpArenaDto, src/nyangine/debug/debug_metrics.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpArenaDto_name_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpArenaDto*)nullptr)->name),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_METRICS_MAX_NAME),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpArenaDto_FIELDS[] = {
    { .name = "name", .type = &_NYA_REFLECT_NYA_HttpArenaDto_name_ARRAY, .offset = nya_offsetof(NYA_HttpArenaDto, name), .hint = NYA_HINT_NONE },
    { .name = "region_count", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpArenaDto, region_count), .hint = NYA_HINT_NONE },
    { .name = "used_bytes", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpArenaDto, used_bytes), .hint = NYA_HINT_NONE },
    { .name = "reserved_bytes", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpArenaDto, reserved_bytes), .hint = NYA_HINT_NONE },
    { .name = "free_list_bytes", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpArenaDto, free_list_bytes), .hint = NYA_HINT_NONE },
    { .name = "fragmentation", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_HttpArenaDto, fragmentation), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpArenaDto = {
    .name = "NYA_HttpArenaDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpArenaDto),
    .alignment = alignof(NYA_HttpArenaDto),
    .fields = _NYA_REFLECT_NYA_HttpArenaDto_FIELDS,
    .field_count = 6,
};

/* NYA_HttpArenasDto, src/nyangine/debug/debug_metrics.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpArenasDto_rows_ARRAY = {
    .name = "NYA_HttpArenaDto[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpArenasDto*)nullptr)->rows),
    .alignment = alignof(NYA_HttpArenaDto),
    .element = &_NYA_REFLECT_NYA_HttpArenaDto, .element_count = (NYA_HTTP_METRICS_MAX_ROWS),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpArenasDto_FIELDS[] = {
    { .name = "count", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpArenasDto, count), .hint = NYA_HINT_NONE },
    { .name = "truncated", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpArenasDto, truncated), .hint = NYA_HINT_NONE },
    { .name = "rows", .type = &_NYA_REFLECT_NYA_HttpArenasDto_rows_ARRAY, .offset = nya_offsetof(NYA_HttpArenasDto, rows), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpArenasDto = {
    .name = "NYA_HttpArenasDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpArenasDto),
    .alignment = alignof(NYA_HttpArenasDto),
    .fields = _NYA_REFLECT_NYA_HttpArenasDto_FIELDS,
    .field_count = 3,
};

/* NYA_HttpOwnerDto, src/nyangine/debug/debug_metrics.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpOwnerDto_name_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpOwnerDto*)nullptr)->name),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_METRICS_MAX_NAME),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpOwnerDto_FIELDS[] = {
    { .name = "name", .type = &_NYA_REFLECT_NYA_HttpOwnerDto_name_ARRAY, .offset = nya_offsetof(NYA_HttpOwnerDto, name), .hint = NYA_HINT_NONE },
    { .name = "system_count", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpOwnerDto, system_count), .hint = NYA_HINT_NONE },
    { .name = "enabled_count", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpOwnerDto, enabled_count), .hint = NYA_HINT_NONE },
    { .name = "time_ns", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpOwnerDto, time_ns), .hint = NYA_HINT_NONE },
    { .name = "memory_bytes", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpOwnerDto, memory_bytes), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpOwnerDto = {
    .name = "NYA_HttpOwnerDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpOwnerDto),
    .alignment = alignof(NYA_HttpOwnerDto),
    .fields = _NYA_REFLECT_NYA_HttpOwnerDto_FIELDS,
    .field_count = 5,
};

/* NYA_HttpSystemsDto, src/nyangine/debug/debug_metrics.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpSystemsDto_rows_ARRAY = {
    .name = "NYA_HttpOwnerDto[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpSystemsDto*)nullptr)->rows),
    .alignment = alignof(NYA_HttpOwnerDto),
    .element = &_NYA_REFLECT_NYA_HttpOwnerDto, .element_count = (NYA_SYSTEM_OWNER_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpSystemsDto_FIELDS[] = {
    { .name = "count", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpSystemsDto, count), .hint = NYA_HINT_NONE },
    { .name = "truncated", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpSystemsDto, truncated), .hint = NYA_HINT_NONE },
    { .name = "accounting_enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_HttpSystemsDto, accounting_enabled), .hint = NYA_HINT_NONE },
    { .name = "rows", .type = &_NYA_REFLECT_NYA_HttpSystemsDto_rows_ARRAY, .offset = nya_offsetof(NYA_HttpSystemsDto, rows), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpSystemsDto = {
    .name = "NYA_HttpSystemsDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpSystemsDto),
    .alignment = alignof(NYA_HttpSystemsDto),
    .fields = _NYA_REFLECT_NYA_HttpSystemsDto_FIELDS,
    .field_count = 4,
};

/* NYA_HttpAccountingDto, src/nyangine/debug/debug_metrics.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpAccountingDto_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_HttpAccountingDto, enabled), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpAccountingDto = {
    .name = "NYA_HttpAccountingDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpAccountingDto),
    .alignment = alignof(NYA_HttpAccountingDto),
    .fields = _NYA_REFLECT_NYA_HttpAccountingDto_FIELDS,
    .field_count = 1,
};

/* NYA_HttpScope, src/nyangine/http/http_auth.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_HttpScope_VARIANTS[] = {
    { .name = "NYA_HTTP_SCOPE_NONE", .value = (s64)(NYA_HTTP_SCOPE_NONE) },
    { .name = "NYA_HTTP_SCOPE_READ", .value = (s64)(NYA_HTTP_SCOPE_READ) },
    { .name = "NYA_HTTP_SCOPE_WRITE", .value = (s64)(NYA_HTTP_SCOPE_WRITE) },
    { .name = "NYA_HTTP_SCOPE_ADMIN", .value = (s64)(NYA_HTTP_SCOPE_ADMIN) },
    { .name = "NYA_HTTP_SCOPE_SECOND_FACTOR", .value = (s64)(NYA_HTTP_SCOPE_SECOND_FACTOR) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpScope = {
    .name = "NYA_HttpScope",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_HttpScope),
    .alignment = alignof(NYA_HttpScope),
    .primitive = (sizeof(NYA_HttpScope) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_HttpScope) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_HttpScope) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_HttpScope_VARIANTS,
    .variant_count = 5,
    .is_bitflags = true,
};

/* NYA_HttpIdentity, src/nyangine/http/http_auth.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpIdentity_subject_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpIdentity*)nullptr)->subject),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_MAX_SUBJECT),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpIdentity_FIELDS[] = {
    { .name = "subject", .type = &_NYA_REFLECT_NYA_HttpIdentity_subject_ARRAY, .offset = nya_offsetof(NYA_HttpIdentity, subject), .hint = NYA_HINT_NONE },
    { .name = "scope", .type = &_NYA_REFLECT_NYA_HttpScope, .offset = nya_offsetof(NYA_HttpIdentity, scope), .hint = NYA_HINT_BITFLAGS },
    { .name = "issued_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpIdentity, issued_at_s), .hint = NYA_HINT_NONE },
    { .name = "expires_at_s", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(NYA_HttpIdentity, expires_at_s), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpIdentity = {
    .name = "NYA_HttpIdentity",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpIdentity),
    .alignment = alignof(NYA_HttpIdentity),
    .fields = _NYA_REFLECT_NYA_HttpIdentity_FIELDS,
    .field_count = 4,
};

/* NYA_HttpLogLevel, src/nyangine/http/http_log.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_HttpLogLevel_VARIANTS[] = {
    { .name = "NYA_HTTP_LOG_SUMMARY", .value = (s64)(NYA_HTTP_LOG_SUMMARY) },
    { .name = "NYA_HTTP_LOG_HEADERS", .value = (s64)(NYA_HTTP_LOG_HEADERS) },
    { .name = "NYA_HTTP_LOG_BODIES", .value = (s64)(NYA_HTTP_LOG_BODIES) },
    { .name = "NYA_HTTP_LOG_LEVEL_COUNT", .value = (s64)(NYA_HTTP_LOG_LEVEL_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpLogLevel = {
    .name = "NYA_HttpLogLevel",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_HttpLogLevel),
    .alignment = alignof(NYA_HttpLogLevel),
    .primitive = (sizeof(NYA_HttpLogLevel) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_HttpLogLevel) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_HttpLogLevel) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_HttpLogLevel_VARIANTS,
    .variant_count = 4,
    .is_bitflags = false,
};

/* NYA_HttpLogAddress, src/nyangine/http/http_log.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_HttpLogAddress_VARIANTS[] = {
    { .name = "NYA_HTTP_LOG_ADDRESS_NETWORK", .value = (s64)(NYA_HTTP_LOG_ADDRESS_NETWORK) },
    { .name = "NYA_HTTP_LOG_ADDRESS_FULL", .value = (s64)(NYA_HTTP_LOG_ADDRESS_FULL) },
    { .name = "NYA_HTTP_LOG_ADDRESS_NONE", .value = (s64)(NYA_HTTP_LOG_ADDRESS_NONE) },
    { .name = "NYA_HTTP_LOG_ADDRESS_COUNT", .value = (s64)(NYA_HTTP_LOG_ADDRESS_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpLogAddress = {
    .name = "NYA_HttpLogAddress",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_HttpLogAddress),
    .alignment = alignof(NYA_HttpLogAddress),
    .primitive = (sizeof(NYA_HttpLogAddress) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_HttpLogAddress) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_HttpLogAddress) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_HttpLogAddress_VARIANTS,
    .variant_count = 4,
    .is_bitflags = false,
};

/* NYA_HttpLogConfig, src/nyangine/http/http_log.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpLogConfig_deny_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpLogConfig*)nullptr)->deny),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_LOG_MAX_DENY_BYTES),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpLogConfig_FIELDS[] = {
    { .name = "level", .type = &_NYA_REFLECT_NYA_HttpLogLevel, .offset = nya_offsetof(NYA_HttpLogConfig, level), .hint = NYA_HINT_NONE },
    { .name = "address", .type = &_NYA_REFLECT_NYA_HttpLogAddress, .offset = nya_offsetof(NYA_HttpLogConfig, address), .hint = NYA_HINT_NONE },
    { .name = "deny", .type = &_NYA_REFLECT_NYA_HttpLogConfig_deny_ARRAY, .offset = nya_offsetof(NYA_HttpLogConfig, deny), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpLogConfig = {
    .name = "NYA_HttpLogConfig",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpLogConfig),
    .alignment = alignof(NYA_HttpLogConfig),
    .fields = _NYA_REFLECT_NYA_HttpLogConfig_FIELDS,
    .field_count = 3,
    .on_apply = _nya_http_log_config_apply,
};

/* NYA_HttpProblem, src/nyangine/http/http_router.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpProblem_error_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpProblem*)nullptr)->error),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (48),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpProblem_detail_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpProblem*)nullptr)->detail),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (192),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpProblem_FIELDS[] = {
    { .name = "status", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(NYA_HttpProblem, status), .hint = NYA_HINT_NONE },
    { .name = "error", .type = &_NYA_REFLECT_NYA_HttpProblem_error_ARRAY, .offset = nya_offsetof(NYA_HttpProblem, error), .hint = NYA_HINT_NONE },
    { .name = "detail", .type = &_NYA_REFLECT_NYA_HttpProblem_detail_ARRAY, .offset = nya_offsetof(NYA_HttpProblem, detail), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpProblem = {
    .name = "NYA_HttpProblem",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpProblem),
    .alignment = alignof(NYA_HttpProblem),
    .fields = _NYA_REFLECT_NYA_HttpProblem_FIELDS,
    .field_count = 3,
};

/* NYA_HttpTotpSubmission, src/nyangine/http/http_totp.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpSubmission_code_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpTotpSubmission*)nullptr)->code),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_TOTP_RECOVERY_TEXT_BYTES),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpTotpSubmission_FIELDS[] = {
    { .name = "code", .type = &_NYA_REFLECT_NYA_HttpTotpSubmission_code_ARRAY, .offset = nya_offsetof(NYA_HttpTotpSubmission, code), .hint = NYA_HINT_NONE, .is_redacted = true },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpSubmission = {
    .name = "NYA_HttpTotpSubmission",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpTotpSubmission),
    .alignment = alignof(NYA_HttpTotpSubmission),
    .fields = _NYA_REFLECT_NYA_HttpTotpSubmission_FIELDS,
    .field_count = 1,
};

/* NYA_HttpTotpRecoveryDto, src/nyangine/http/http_totp.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpRecoveryDto_code_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpTotpRecoveryDto*)nullptr)->code),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_TOTP_RECOVERY_TEXT_BYTES),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpTotpRecoveryDto_FIELDS[] = {
    { .name = "code", .type = &_NYA_REFLECT_NYA_HttpTotpRecoveryDto_code_ARRAY, .offset = nya_offsetof(NYA_HttpTotpRecoveryDto, code), .hint = NYA_HINT_NONE, .is_redacted = true },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpRecoveryDto = {
    .name = "NYA_HttpTotpRecoveryDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpTotpRecoveryDto),
    .alignment = alignof(NYA_HttpTotpRecoveryDto),
    .fields = _NYA_REFLECT_NYA_HttpTotpRecoveryDto_FIELDS,
    .field_count = 1,
};

/* NYA_HttpTotpEnrolmentDto, src/nyangine/http/http_totp.h */

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpEnrolmentDto_uri_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpTotpEnrolmentDto*)nullptr)->uri),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_TOTP_URI_BYTES),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpEnrolmentDto_secret_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpTotpEnrolmentDto*)nullptr)->secret),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_HTTP_TOTP_SECRET_TEXT_BYTES),
};

static const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpEnrolmentDto_recovery_ARRAY = {
    .name = "NYA_HttpTotpRecoveryDto[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_HttpTotpEnrolmentDto*)nullptr)->recovery),
    .alignment = alignof(NYA_HttpTotpRecoveryDto),
    .element = &_NYA_REFLECT_NYA_HttpTotpRecoveryDto, .element_count = (NYA_HTTP_TOTP_RECOVERY_CODES),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_HttpTotpEnrolmentDto_FIELDS[] = {
    { .name = "uri", .type = &_NYA_REFLECT_NYA_HttpTotpEnrolmentDto_uri_ARRAY, .offset = nya_offsetof(NYA_HttpTotpEnrolmentDto, uri), .hint = NYA_HINT_NONE, .is_redacted = true },
    { .name = "secret", .type = &_NYA_REFLECT_NYA_HttpTotpEnrolmentDto_secret_ARRAY, .offset = nya_offsetof(NYA_HttpTotpEnrolmentDto, secret), .hint = NYA_HINT_NONE, .is_redacted = true },
    { .name = "recovery", .type = &_NYA_REFLECT_NYA_HttpTotpEnrolmentDto_recovery_ARRAY, .offset = nya_offsetof(NYA_HttpTotpEnrolmentDto, recovery), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_HttpTotpEnrolmentDto = {
    .name = "NYA_HttpTotpEnrolmentDto",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_HttpTotpEnrolmentDto),
    .alignment = alignof(NYA_HttpTotpEnrolmentDto),
    .fields = _NYA_REFLECT_NYA_HttpTotpEnrolmentDto_FIELDS,
    .field_count = 3,
};

/* NYA_Quaternion, src/nyangine/math/math_quaternion.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_Quaternion_FIELDS[] = {
    { .name = "x", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Quaternion, x), .hint = NYA_HINT_NONE },
    { .name = "y", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Quaternion, y), .hint = NYA_HINT_NONE },
    { .name = "z", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Quaternion, z), .hint = NYA_HINT_NONE },
    { .name = "w", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Quaternion, w), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_Quaternion = {
    .name = "NYA_Quaternion",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_Quaternion),
    .alignment = alignof(NYA_Quaternion),
    .fields = _NYA_REFLECT_NYA_Quaternion_FIELDS,
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

/* NYA_Light2D, src/nyangine/renderer/render2d.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_Light2D_FIELDS[] = {
    { .name = "radius", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Light2D, radius), .hint = NYA_HINT_NONE },
    { .name = "intensity", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Light2D, intensity), .hint = NYA_HINT_NONE },
    { .name = "color", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_Light2D, color), .hint = NYA_HINT_NONE },
    { .name = "offset", .type = &_NYA_REFLECT_f32x2, .offset = nya_offsetof(NYA_Light2D, offset), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_Light2D = {
    .name = "NYA_Light2D",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_Light2D),
    .alignment = alignof(NYA_Light2D),
    .fields = _NYA_REFLECT_NYA_Light2D_FIELDS,
    .field_count = 4,
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

/* NYA_RenderToggle, src/nyangine/renderer/render_features.h */

static const NYA_ReflectVariant _NYA_REFLECT_NYA_RenderToggle_VARIANTS[] = {
    { .name = "NYA_RENDER_TOGGLE_DEFAULT", .value = (s64)(NYA_RENDER_TOGGLE_DEFAULT) },
    { .name = "NYA_RENDER_TOGGLE_ON", .value = (s64)(NYA_RENDER_TOGGLE_ON) },
    { .name = "NYA_RENDER_TOGGLE_OFF", .value = (s64)(NYA_RENDER_TOGGLE_OFF) },
    { .name = "NYA_RENDER_TOGGLE_COUNT", .value = (s64)(NYA_RENDER_TOGGLE_COUNT) },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_RenderToggle = {
    .name = "NYA_RenderToggle",
    .kind = NYA_REFLECT_ENUM,
    .size = sizeof(NYA_RenderToggle),
    .alignment = alignof(NYA_RenderToggle),
    .primitive = (sizeof(NYA_RenderToggle) == 8 ? NYA_TYPE_S64
                : sizeof(NYA_RenderToggle) == 2 ? NYA_TYPE_S16
                : sizeof(NYA_RenderToggle) == 1 ? NYA_TYPE_S8
                                  : NYA_TYPE_S32),
    .variants = _NYA_REFLECT_NYA_RenderToggle_VARIANTS,
    .variant_count = 4,
    .is_bitflags = false,
};

/* NYA_RenderFeatures, src/nyangine/renderer/render_features.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_RenderFeatures_FIELDS[] = {
    { .name = "frustum_culling", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, frustum_culling), .hint = NYA_HINT_NONE },
    { .name = "occlusion_culling", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, occlusion_culling), .hint = NYA_HINT_NONE },
    { .name = "backface_culling", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, backface_culling), .hint = NYA_HINT_NONE },
    { .name = "depth_test", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, depth_test), .hint = NYA_HINT_NONE },
    { .name = "draw_sorting", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, draw_sorting), .hint = NYA_HINT_NONE },
    { .name = "transparency", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, transparency), .hint = NYA_HINT_NONE },
    { .name = "shadows", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, shadows), .hint = NYA_HINT_NONE },
    { .name = "lighting", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, lighting), .hint = NYA_HINT_NONE },
    { .name = "point_lights", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, point_lights), .hint = NYA_HINT_NONE },
    { .name = "reflections", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, reflections), .hint = NYA_HINT_NONE },
    { .name = "textures", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, textures), .hint = NYA_HINT_NONE },
    { .name = "fog", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, fog), .hint = NYA_HINT_NONE },
    { .name = "sky", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, sky), .hint = NYA_HINT_NONE },
    { .name = "decals", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, decals), .hint = NYA_HINT_NONE },
    { .name = "lod", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, lod), .hint = NYA_HINT_NONE },
    { .name = "particles", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, particles), .hint = NYA_HINT_NONE },
    { .name = "haze", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, haze), .hint = NYA_HINT_NONE },
    { .name = "post", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, post), .hint = NYA_HINT_NONE },
    { .name = "ink", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, ink), .hint = NYA_HINT_NONE },
    { .name = "ambient_occlusion", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, ambient_occlusion), .hint = NYA_HINT_NONE },
    { .name = "antialias", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, antialias), .hint = NYA_HINT_NONE },
    { .name = "depth_of_field", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, depth_of_field), .hint = NYA_HINT_NONE },
    { .name = "speed_lines", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, speed_lines), .hint = NYA_HINT_NONE },
    { .name = "bloom", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, bloom), .hint = NYA_HINT_NONE },
    { .name = "light_shafts", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, light_shafts), .hint = NYA_HINT_NONE },
    { .name = "motion_blur", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, motion_blur), .hint = NYA_HINT_NONE },
    { .name = "eye_adaptation", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, eye_adaptation), .hint = NYA_HINT_NONE },
    { .name = "grade", .type = &_NYA_REFLECT_NYA_RenderToggle, .offset = nya_offsetof(NYA_RenderFeatures, grade), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_RenderFeatures = {
    .name = "NYA_RenderFeatures",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_RenderFeatures),
    .alignment = alignof(NYA_RenderFeatures),
    .fields = _NYA_REFLECT_NYA_RenderFeatures_FIELDS,
    .field_count = 28,
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

/* NYA_PostMotionBlur, src/nyangine/renderer/render_post.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_PostMotionBlur_FIELDS[] = {
    { .name = "enabled", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_PostMotionBlur, enabled), .hint = NYA_HINT_NONE },
    { .name = "strength", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_PostMotionBlur, strength), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_PostMotionBlur = {
    .name = "NYA_PostMotionBlur",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_PostMotionBlur),
    .alignment = alignof(NYA_PostMotionBlur),
    .fields = _NYA_REFLECT_NYA_PostMotionBlur_FIELDS,
    .field_count = 2,
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

/* NYA_Render2DHaze, src/nyangine/renderer/renderer.h */

static const NYA_ReflectField _NYA_REFLECT_NYA_Render2DHaze_FIELDS[] = {
    { .name = "color", .type = &_NYA_REFLECT_NYA_Color, .offset = nya_offsetof(NYA_Render2DHaze, color), .hint = NYA_HINT_NONE },
    { .name = "density", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Render2DHaze, density), .hint = NYA_HINT_NONE },
    { .name = "falloff", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_Render2DHaze, falloff), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_NYA_Render2DHaze = {
    .name = "NYA_Render2DHaze",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(NYA_Render2DHaze),
    .alignment = alignof(NYA_Render2DHaze),
    .fields = _NYA_REFLECT_NYA_Render2DHaze_FIELDS,
    .field_count = 3,
};

/* NYA_NetChatMessage, src/nyangine/replicate/replicate_chat.h */

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

static const NYA_TypeReflection _NYA_REFLECT_NYA_UIStyle_icon_sheet_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((NYA_UIStyle*)nullptr)->icon_sheet),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_UI_SKIN_TEXTURE_MAX),
};

static const NYA_ReflectField _NYA_REFLECT_NYA_UIStyle_FIELDS[] = {
    { .name = "font", .type = &_NYA_REFLECT_NYA_UIStyle_font_ARRAY, .offset = nya_offsetof(NYA_UIStyle, font), .hint = NYA_HINT_NONE },
    { .name = "title_font", .type = &_NYA_REFLECT_NYA_UIStyle_title_font_ARRAY, .offset = nya_offsetof(NYA_UIStyle, title_font), .hint = NYA_HINT_NONE },
    { .name = "body_size", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, body_size), .hint = NYA_HINT_NONE },
    { .name = "small_size", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, small_size), .hint = NYA_HINT_NONE },
    { .name = "title_size", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, title_size), .hint = NYA_HINT_NONE },
    { .name = "scale", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, scale), .hint = NYA_HINT_NONE },
    { .name = "follow_display_scale", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(NYA_UIStyle, follow_display_scale), .hint = NYA_HINT_NONE },
    { .name = "margin", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, margin), .hint = NYA_HINT_NONE },
    { .name = "padding", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, padding), .hint = NYA_HINT_NONE },
    { .name = "spacing", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, spacing), .hint = NYA_HINT_NONE },
    { .name = "radius", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, radius), .hint = NYA_HINT_NONE },
    { .name = "outline", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, outline), .hint = NYA_HINT_NONE },
    { .name = "depth", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, depth), .hint = NYA_HINT_NONE },
    { .name = "pop", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, pop), .hint = NYA_HINT_NONE },
    { .name = "focus_bar", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(NYA_UIStyle, focus_bar), .hint = NYA_HINT_NONE },
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
    { .name = "icon_sheet", .type = &_NYA_REFLECT_NYA_UIStyle_icon_sheet_ARRAY, .offset = nya_offsetof(NYA_UIStyle, icon_sheet), .hint = NYA_HINT_NONE },
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
    .field_count = 33,
};

const NYA_TypeReflection* const NYA_REFLECT_ENGINE_TYPES[NYA_REFLECT_ENGINE_TYPE_COUNT] = {
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
    &_NYA_REFLECT_NYA_ConfigDocument,
    &_NYA_REFLECT_NYA_EntityState,
    &_NYA_REFLECT_NYA_EntityVisualKind,
    &_NYA_REFLECT_NYA_PluginPermission,
    &_NYA_REFLECT_NYA_PluginDependency,
    &_NYA_REFLECT_NYA_PluginManifest,
    &_NYA_REFLECT_NYA_SceneVisual,
    &_NYA_REFLECT_NYA_SceneEntity,
    &_NYA_REFLECT_NYA_SettingsVolumes,
    &_NYA_REFLECT_NYA_GraphicsQuality,
    &_NYA_REFLECT_NYA_SettingsGraphics,
    &_NYA_REFLECT_NYA_HttpMetricsDto,
    &_NYA_REFLECT_NYA_HttpCeilingDto,
    &_NYA_REFLECT_NYA_HttpCeilingsDto,
    &_NYA_REFLECT_NYA_HttpArenaDto,
    &_NYA_REFLECT_NYA_HttpArenasDto,
    &_NYA_REFLECT_NYA_HttpOwnerDto,
    &_NYA_REFLECT_NYA_HttpSystemsDto,
    &_NYA_REFLECT_NYA_HttpAccountingDto,
    &_NYA_REFLECT_NYA_HttpScope,
    &_NYA_REFLECT_NYA_HttpIdentity,
    &_NYA_REFLECT_NYA_HttpLogLevel,
    &_NYA_REFLECT_NYA_HttpLogAddress,
    &_NYA_REFLECT_NYA_HttpLogConfig,
    &_NYA_REFLECT_NYA_HttpProblem,
    &_NYA_REFLECT_NYA_HttpTotpSubmission,
    &_NYA_REFLECT_NYA_HttpTotpRecoveryDto,
    &_NYA_REFLECT_NYA_HttpTotpEnrolmentDto,
    &_NYA_REFLECT_NYA_Quaternion,
    &_NYA_REFLECT_NYA_EaseType,
    &_NYA_REFLECT_NYA_NetPeerId,
    &_NYA_REFLECT_NYA_Light2D,
    &_NYA_REFLECT_NYA_Render3DFog,
    &_NYA_REFLECT_NYA_Render3DDecals,
    &_NYA_REFLECT_NYA_Color,
    &_NYA_REFLECT_NYA_RenderToggle,
    &_NYA_REFLECT_NYA_RenderFeatures,
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
    &_NYA_REFLECT_NYA_PostMotionBlur,
    &_NYA_REFLECT_NYA_PostDebugView,
    &_NYA_REFLECT_NYA_Render2DHaze,
    &_NYA_REFLECT_NYA_NetChatMessage,
    &_NYA_REFLECT_NYA_UIOverflow,
    &_NYA_REFLECT_NYA_UIStateColors,
    &_NYA_REFLECT_NYA_UISkin,
    &_NYA_REFLECT_NYA_UIStateSkins,
    &_NYA_REFLECT_NYA_UIStyle,
};
