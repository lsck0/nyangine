/* THIS FILE IS GENERATED. DO NYAT TOUCH. */

#include "nyangine/nyangine.h"

#include "genyarated/reflection.h"

/*
 * Every size and offset below is an expression rather than a number, so the compiler that is
 * already compiling these structs is what computes the layout. See src/build/pp/reflection.h.
 */

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
    { .name = "game", .type = &_NYA_REFLECT_GNY_ConfigGame, .offset = nya_offsetof(GNY_Config, game), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_GNY_Config = {
    .name = "GNY_Config",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(GNY_Config),
    .alignment = alignof(GNY_Config),
    .fields = _NYA_REFLECT_GNY_Config_FIELDS,
    .field_count = 1,
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

/* GNY_RobotRun, src/gnyame/robots.h */

static const NYA_TypeReflection _NYA_REFLECT_GNY_RobotRun_ended_ARRAY = {
    .name = "char[]", .kind = NYA_REFLECT_ARRAY,
    .size = sizeof(((GNY_RobotRun*)nullptr)->ended),
    .alignment = alignof(char),
    .element = &_NYA_REFLECT_char, .element_count = (NYA_CLOCK_FORMAT_MAX_LENGTH),
};

static const NYA_ReflectField _NYA_REFLECT_GNY_RobotRun_FIELDS[] = {
    { .name = "id", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(GNY_RobotRun, id), .hint = NYA_HINT_NONE, .is_key = true },
    { .name = "generations", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(GNY_RobotRun, generations), .hint = NYA_HINT_NONE },
    { .name = "fitness", .type = &_NYA_REFLECT_f64, .offset = nya_offsetof(GNY_RobotRun, fitness), .hint = NYA_HINT_NONE },
    { .name = "dqn_steps", .type = &_NYA_REFLECT_u64, .offset = nya_offsetof(GNY_RobotRun, dqn_steps), .hint = NYA_HINT_NONE },
    { .name = "dqn_score", .type = &_NYA_REFLECT_f64, .offset = nya_offsetof(GNY_RobotRun, dqn_score), .hint = NYA_HINT_NONE },
    { .name = "ended", .type = &_NYA_REFLECT_GNY_RobotRun_ended_ARRAY, .offset = nya_offsetof(GNY_RobotRun, ended), .hint = NYA_HINT_NONE },
};

const NYA_TypeReflection _NYA_REFLECT_GNY_RobotRun = {
    .name = "GNY_RobotRun",
    .kind = NYA_REFLECT_STRUCT,
    .size = sizeof(GNY_RobotRun),
    .alignment = alignof(GNY_RobotRun),
    .fields = _NYA_REFLECT_GNY_RobotRun_FIELDS,
    .field_count = 6,
};

const NYA_TypeReflection* const NYA_REFLECT_TYPES[NYA_REFLECT_TYPE_COUNT] = {
    &_NYA_REFLECT_NYA_AccountIdentity,
    &_NYA_REFLECT_NYA_AccountInvite,
    &_NYA_REFLECT_NYA_AccountRecoveryCode,
    &_NYA_REFLECT_NYA_AccountSession,
    &_NYA_REFLECT_NYA_AccountUser,
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
    &_NYA_REFLECT_GNY_ConfigRobots,
    &_NYA_REFLECT_GNY_ConfigGame,
    &_NYA_REFLECT_GNY_Config,
    &_NYA_REFLECT_GNY_EntityKind,
    &_NYA_REFLECT_GNY_EntityFlags,
    &_NYA_REFLECT_GNY_RobotRun,
};

const NYA_TypeReflection* nya_reflect_find(NYA_ConstCString name) {
    if (name == nullptr) return nullptr;

    for (u32 i = 0; i < NYA_REFLECT_TYPE_COUNT; i++) {
        if (nya_string_equals(NYA_REFLECT_TYPES[i]->name, name)) return NYA_REFLECT_TYPES[i];
    }

    return nullptr;
}
