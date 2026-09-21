/**
 * @file render_features.h
 *
 * One switch per thing the renderer does, per window, so any of it can be turned off to see what it was
 * contributing or to get a deliberate look. Every feature that has its own options struct keeps it; this is the
 * master switch above them, and the one place that lists them all.
 *
 * ```c
 * // In a debug key handler, or from `engine.renderer.features` in the config.
 * NYA_RenderFeatures features = nya_render_features(window);
 * features.shadows = NYA_RENDER_TOGGLE_OFF;
 * nya_render_features_set(window, features);
 *
 * // On the draw path, one bit test.
 * if (nya_render_feature_enabled(window, NYA_RENDER_FEATURE_FOG)) { ... }
 * ```
 *
 * Every field is NYA_RENDER_TOGGLE_DEFAULT when zeroed, so a zeroed struct overrides nothing and a config file
 * names only what it wants changed. What "default" means is the feature's own business: frustum culling defaults
 * on, bloom defaults to whatever NYA_PostBloom says.
 *
 * Nothing here is backend specific. The switches live on the renderer's options and the backends read them
 * through nya_render_feature_enabled.
 *
 * Why a tri-state and not a `b8`: the engine's convention is that a zeroed options struct changes nothing, and
 * half of these default on. A `b8 frustum_culling` would read "off" when zeroed, and the alternative,
 * `b8 no_frustum_culling`, is a negation in a name.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

typedef struct NYA_Window NYA_Window;

typedef enum NYA_RenderToggle    NYA_RenderToggle;
typedef enum NYA_RenderFeature   NYA_RenderFeature;
typedef struct NYA_RenderFeatures NYA_RenderFeatures;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What one switch says. */
// @reflect
enum NYA_RenderToggle {
    /** Leave it to the feature's own options. The zero value, so a zeroed struct overrides nothing. */
    NYA_RENDER_TOGGLE_DEFAULT = 0,

    /** On, whatever those options say. What turns a pass on without editing the pass's own struct. */
    NYA_RENDER_TOGGLE_ON,

    /** Off. The feature's work is skipped, not done and thrown away. */
    NYA_RENDER_TOGGLE_OFF,

    NYA_RENDER_TOGGLE_COUNT,
};

/**
 * Everything switchable, in the order NYA_RenderFeatures declares it. The enum is what the draw paths ask with and
 * the struct is what a caller and a config file write; static asserts below keep the two in step.
 * */
enum NYA_RenderFeature {
    /* Geometry: what is recorded, and what the rasterizer does with it. */

    /** Rejecting draws no pass can see. Off, every pass sees everything, which is what it costs to have. */
    NYA_RENDER_FEATURE_FRUSTUM_CULLING,

    /** The camera's occlusion buffer, when one is set. See nya_render3d_occlusion. */
    NYA_RENDER_FEATURE_OCCLUSION_CULLING,

    /** Discarding triangles facing away. Off shows a model's inside, which is how an inverted normal is found. */
    NYA_RENDER_FEATURE_BACKFACE_CULLING,

    /** The depth buffer. Off draws the scene in the order it was recorded, which is the x-ray view. */
    NYA_RENDER_FEATURE_DEPTH_TEST,

    /** Ordering translucent triangles back to front and merging 2D ranges. Off shows what the sort is worth. */
    NYA_RENDER_FEATURE_DRAW_SORTING,

    /** Translucency at all. Off draws every surface opaque, glass and billboards included. */
    NYA_RENDER_FEATURE_TRANSPARENCY,

    /* Shading. */

    NYA_RENDER_FEATURE_SHADOWS,

    /** The directional light's lit term. Off leaves everything at its ambient, which is flat albedo. */
    NYA_RENDER_FEATURE_LIGHTING,

    NYA_RENDER_FEATURE_POINT_LIGHTS,

    /** The metallic highlight, the rim and refraction: everything a surface reflects rather than emits. */
    NYA_RENDER_FEATURE_REFLECTIONS,

    /** Sampled base colour. Off draws every surface in its vertex colour. */
    NYA_RENDER_FEATURE_TEXTURES,

    NYA_RENDER_FEATURE_FOG,
    NYA_RENDER_FEATURE_SKY,
    NYA_RENDER_FEATURE_DECALS,

    /** Mesh and terrain detail levels. Off draws the finest one. See render_lod.h. */
    NYA_RENDER_FEATURE_LOD,

    /** Drawing particle systems. Their simulation is the game's and keeps running. */
    NYA_RENDER_FEATURE_PARTICLES,

    /** The 2D haze between parallax planes. See nya_render2d_haze_set. */
    NYA_RENDER_FEATURE_HAZE,

    /* The post chain. POST is the master switch; the rest gate one pass each. */

    NYA_RENDER_FEATURE_POST,
    NYA_RENDER_FEATURE_INK,
    NYA_RENDER_FEATURE_AMBIENT_OCCLUSION,
    NYA_RENDER_FEATURE_ANTIALIAS,
    NYA_RENDER_FEATURE_DEPTH_OF_FIELD,
    NYA_RENDER_FEATURE_SPEED_LINES,
    NYA_RENDER_FEATURE_BLOOM,
    NYA_RENDER_FEATURE_LIGHT_SHAFTS,
    NYA_RENDER_FEATURE_MOTION_BLUR,
    NYA_RENDER_FEATURE_EYE_ADAPTATION,

    /** Colour grading through a LUT, applied by the caller's own pass. See render_lut.h. */
    NYA_RENDER_FEATURE_GRADE,

    NYA_RENDER_FEATURE_COUNT,
};

static_assert(NYA_RENDER_FEATURE_COUNT <= 32, "the resolved switches are a u32 mask");

/**
 * The switches as a caller writes them, one field per NYA_RenderFeature in the same order. Read back through
 * nya_render_features; asked about one at a time through nya_render_feature_enabled, which reads a resolved mask
 * rather than this struct.
 * */
// @reflect
struct NYA_RenderFeatures {
    NYA_RenderToggle frustum_culling;
    NYA_RenderToggle occlusion_culling;
    NYA_RenderToggle backface_culling;
    NYA_RenderToggle depth_test;
    NYA_RenderToggle draw_sorting;
    NYA_RenderToggle transparency;

    NYA_RenderToggle shadows;
    NYA_RenderToggle lighting;
    NYA_RenderToggle point_lights;
    NYA_RenderToggle reflections;
    NYA_RenderToggle textures;
    NYA_RenderToggle fog;
    NYA_RenderToggle sky;
    NYA_RenderToggle decals;
    NYA_RenderToggle lod;
    NYA_RenderToggle particles;
    NYA_RenderToggle haze;

    NYA_RenderToggle post;
    NYA_RenderToggle ink;
    NYA_RenderToggle ambient_occlusion;
    NYA_RenderToggle antialias;
    NYA_RenderToggle depth_of_field;
    NYA_RenderToggle speed_lines;
    NYA_RenderToggle bloom;
    NYA_RenderToggle light_shafts;
    NYA_RenderToggle motion_blur;
    NYA_RenderToggle eye_adaptation;
    NYA_RenderToggle grade;
};

/*
 * The struct is read as an array of switches indexed by NYA_RenderFeature, so adding a feature is one enum entry
 * and one field. These catch a field added in the wrong place or left out.
 */
static_assert(sizeof(NYA_RenderFeatures) == (u64)NYA_RENDER_FEATURE_COUNT * sizeof(NYA_RenderToggle),
              "NYA_RenderFeatures has exactly one switch per NYA_RenderFeature");

static_assert(nya_offsetof(NYA_RenderFeatures, frustum_culling) == (u64)NYA_RENDER_FEATURE_FRUSTUM_CULLING * sizeof(NYA_RenderToggle)
                  && nya_offsetof(NYA_RenderFeatures, transparency) == (u64)NYA_RENDER_FEATURE_TRANSPARENCY * sizeof(NYA_RenderToggle)
                  && nya_offsetof(NYA_RenderFeatures, haze) == (u64)NYA_RENDER_FEATURE_HAZE * sizeof(NYA_RenderToggle)
                  && nya_offsetof(NYA_RenderFeatures, grade) == (u64)NYA_RENDER_FEATURE_GRADE * sizeof(NYA_RenderToggle),
              "the fields of NYA_RenderFeatures are in NYA_RenderFeature's order");

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Replaces `window`'s switches. Cheap and idempotent, so a game can feed its config every frame; the resolved mask
 * is rebuilt here rather than on the draw path.
 * */
NYA_API void nya_render_features_set(NYA_Window* window, NYA_RenderFeatures features);

/** The switches as set. */
NYA_API NYA_RenderFeatures nya_render_features(const NYA_Window* window) __attr_no_discard;

/**
 * Whether `feature` runs on `window`. One bit test. For the features with no options of their own to ask with:
 * frustum culling, the depth test, sorting, transparency.
 * */
NYA_API b8 nya_render_feature_enabled(const NYA_Window* window, NYA_RenderFeature feature) __attr_no_discard;

/**
 * The same for a feature whose own options already say whether it wants to run: off wins, on overrides, and
 * NYA_RENDER_TOGGLE_DEFAULT passes `asked` through.
 *
 * ```c
 * if (nya_render_feature_on(window, NYA_RENDER_FEATURE_BLOOM, render->post_bloom.enabled)) { ... }
 * ```
 * */
NYA_API b8 nya_render_feature_on(const NYA_Window* window, NYA_RenderFeature feature, b8 asked) __attr_no_discard;

/** The field name, for an overlay row or a log line. "frustum culling", never null. */
NYA_API NYA_ConstCString nya_render_feature_name(NYA_RenderFeature feature) __attr_no_discard;

/** Every feature that is off, as one line of names, for the debug overlay. Empty when nothing is. */
NYA_API u32 nya_render_features_disabled_text(const NYA_Window* window, char* out, u64 size) __attr_no_discard;
