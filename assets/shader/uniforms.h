/**
 * @file uniforms.h
 *
 * The C side of every shader's uniform block, kept beside the shaders.
 *
 * A cbuffer and the struct pushed for it must agree on field order, type and padding, and no compiler checks
 * that. Name a block `NYA_Shader<Name>Uniform` and keep the `cbuffer`'s field order. HLSL packs constant
 * buffers into four-component rows and never splits a member across one, so a `float2` after three floats is
 * not where C puts it; group scalars in fours or pad.
 *
 * Not generated from the HLSL, which would add build machinery for a handful of structs, and not compiled into
 * the shaders; the build keeps it out of the asset index.
 * */
#pragma once

#include "nyangine-std/base/base_types.h"

// For NYA_RENDER3D_MAX_POINT_LIGHTS and NYA_RENDER3D_SHADOW_CASCADES, which size the arrays below.
#include "nyangine-core/renderer/render3d.h"

typedef struct NYA_ShaderBlurUniform     NYA_ShaderBlurUniform;
typedef struct NYA_ShaderPixelateUniform NYA_ShaderPixelateUniform;
typedef struct NYA_ShaderBloomUniform    NYA_ShaderBloomUniform;
typedef struct NYA_ShaderLutUniform      NYA_ShaderLutUniform;
typedef struct NYA_ShaderCrtUniform      NYA_ShaderCrtUniform;
typedef struct NYA_ShaderSkyUniform      NYA_ShaderSkyUniform;
typedef struct NYA_ShaderGlassUniform    NYA_ShaderGlassUniform;
typedef struct NYA_ShaderFoliageUniform  NYA_ShaderFoliageUniform;
typedef struct NYA_ShaderWaterVertexUniform NYA_ShaderWaterVertexUniform;
typedef struct NYA_ShaderWaterFragUniform   NYA_ShaderWaterFragUniform;

/** effect_blur.frag.hlsl. One directional pass; run it twice, transposed, for a real gaussian. */
struct NYA_ShaderBlurUniform {
    /** One texel in uv, so sample offsets are resolution independent. */
    f32 texel_x, texel_y;

    /** Which way the kernel steps. {1,0} is horizontal, {0,1} vertical. */
    f32 direction_x, direction_y;
};

/** effect_pixelate.frag.hlsl. */
struct NYA_ShaderPixelateUniform {
    /** Blocks across and down the sampled region. Larger is finer; 1 would be a single flat block. */
    f32 blocks_x, blocks_y;
};

/** effect_bloom_gather.frag.hlsl and effect_bloom.frag.hlsl. See NYA_PostBloom. */
struct NYA_ShaderBloomUniform {
    /** The spacing between the gather's taps, in uv of the full image. */
    f32 spread_x, spread_y;

    f32 threshold;
    f32 intensity;
};

/** effect_lut.frag.hlsl. The table itself is the pass's texture; see NYA_PostPass.texture. */
struct NYA_ShaderLutUniform {
    /** How much of the graded colour replaces the original, in [0, 1]. */
    f32 strength;

    f32 _padding[3];
};

/** effect_crt.frag.hlsl. */
struct NYA_ShaderCrtUniform {
    /** How far the screen bulges. 0 is flat, 0.1 is a gentle tube, past 0.3 is a fishbowl. */
    f32 curvature;

    /** Scanlines down the image, usually the target's height in pixels for one line per row. */
    f32 scanline_count;

    /** How dark the gaps between scanlines go. 0 disables them. */
    f32 scanline_strength;

    /** Colour fringing at the edges, in texels. Zero disables it. */
    f32 aberration;
};

/**
 * mesh3d.frag.hlsl: the material and the light shading it.
 *
 * Field order and padding matter: a float3 followed by a float fills one sixteen-byte row, and reordering reads
 * the wrong fields. Per flush rather than per draw, so a batch stays a batch; base colour is per vertex.
 * */
struct NYA_ShaderMesh3DUniform {
    /** From the surface toward the light, normalized. See the shader for why this direction. */
    f32 light_direction_x, light_direction_y, light_direction_z;

    /** Brightness of surfaces facing away from the light. Around 0.6 for the cartoon look. */
    f32 ambient;

    f32 light_color_r, light_color_g, light_color_b;

    /** Scales the direct term only. One is neutral. */
    f32 intensity;

    /** Where the view is from, for the highlight and the rim. */
    f32 camera_x, camera_y, camera_z;

    /** Highlight strength. See NYA_Render3DMaterial.metallic. */
    f32 metallic;

    /** Band softness. See NYA_Render3DMaterial.roughness. */
    f32 roughness;

    /** Rim strength on the silhouette. See NYA_Render3DMaterial.reflectance. */
    f32 reflectance;

    /** How much of the base colour is added regardless of light. See NYA_Render3DMaterial.emission. */
    f32 emission;

    /** How many of the arrays below are live, as a float, so it shares a row of floats without a reinterpretation. */
    f32 point_light_count;

    /**
     * Point lights as two parallel float4 arrays (position and range, colour and intensity). HLSL pads each array
     * element to sixteen bytes, so a struct array would waste four per light. Must match
     * NYA_RENDER3D_MAX_POINT_LIGHTS and MESH3D_MAX_POINT_LIGHTS.
     * */
    f32 point_light_position_range[NYA_RENDER3D_MAX_POINT_LIGHTS][4];
    f32 point_light_color_intensity[NYA_RENDER3D_MAX_POINT_LIGHTS][4];

    /** How strongly curved edges are darkened. See NYA_Render3DMaterial.edge. */
    f32 edge;

    /** How dark a shadowed surface goes, in [0, 1]. Zero disables the lookup in the shader entirely. */
    f32 shadow_strength;

    /** One shadow map texel in UV, so the filter can step by texels without knowing the resolution. */
    f32 shadow_texel;

    /** Depth slack against shadow acne. See mesh3d_shadow in mesh3d_shading.hlsli. */
    f32 shadow_bias;

    /**
     * One light view-projection per cascade, as each shadow pass rendered with. Entries past `cascade_count` are
     * stale and never read.
     * */
    f32_4x4 light_view_projection[NYA_RENDER3D_SHADOW_CASCADES];

    /**
     * Each cascade's reach from the shadow volume's centre, in world units. Not used for selection (see
     * mesh3d_cascade_for); it converts a shadow texel to a world distance for the normal offset. A float4, since
     * HLSL pads array elements to sixteen bytes.
     * */
    f32 cascade_extent[4];

    /** How many cascades actually ran this frame. A float, for the reason `point_light_count` is one. */
    f32 cascade_count;

    /** How many cascades the atlas strip is divided into, which can exceed how many ran. */
    f32 atlas_cascades;

    /** Padding to close the row. */
    f32 cascade_pad[2];

    /* Fog, two rows. See NYA_Render3DFog. */

    f32 fog_color_r, fog_color_g, fog_color_b;

    /** Zero means no fog, and the shader returns before touching anything else here. */
    f32 fog_density;

    f32 fog_height_falloff;
    f32 fog_height_base;
    f32 fog_sun_amount;
    f32 fog_aerial;

    /* Colour of the ambient and the shade, three rows. See NYA_Render3DLight.sky and NYA_Render3DShadowOptions.color. */

    f32 ambient_sky_r, ambient_sky_g, ambient_sky_b;
    f32 ambient_pad;

    f32 ambient_ground_r, ambient_ground_g, ambient_ground_b;
    f32 ambient_ground_pad;

    /** Multiplied into fully shaded light: the shade colour at unit brightness, or white for none. */
    f32 shade_tint_r, shade_tint_g, shade_tint_b;
    f32 shade_tint_pad;
};

/*
 * The cbuffer layout, checked against the rows mesh3d_shading.hlsli declares. Inserting a field or losing a
 * float3's companion scalar moves an offset, and the shader would silently read the wrong member.
 */
static_assert(offsetof(struct NYA_ShaderMesh3DUniform, point_light_position_range) == 64, "the four scalar rows come to 64 bytes");
static_assert(offsetof(struct NYA_ShaderMesh3DUniform, edge) == 192, "the two point light arrays are 64 bytes each");
static_assert(offsetof(struct NYA_ShaderMesh3DUniform, light_view_projection) == 208, "edge/shadow_strength/shadow_texel/shadow_bias are one row");
static_assert(offsetof(struct NYA_ShaderMesh3DUniform, cascade_extent) == 208 + (16 * 4 * NYA_RENDER3D_SHADOW_CASCADES),
              "one float4x4 per cascade, and nothing between them");
static_assert(sizeof(struct NYA_ShaderMesh3DUniform) == offsetof(struct NYA_ShaderMesh3DUniform, cascade_extent) + 112,
              "cascade_extent, the cascade row, two fog rows and three colour rows close the block");

/** Lights one nya_render2d_lights_apply may pass. Matches MAX_LIGHTS in light2d.frag.hlsl. */
#define NYA_SHADER_LIGHT2D_MAX 16

/**
 * Bones one skinned draw may use. Must equal NYA_SKELETON_MAX_BONES; the shaders cannot include engine
 * headers, so the two are checked beside nya_render3d_skinned_mesh.
 * */
#define NYA_SHADER_SKIN_MAX_BONES 64

/**
 * light2d.frag.hlsl: the 2D light map's lights and ambient floor. Two parallel arrays, since HLSL pads each
 * element to sixteen bytes. The block is 544 bytes, one push.
 * */
struct NYA_ShaderLight2DUniform {
    /** Per light: x, y in target pixels, z radius in the same units, w intensity. */
    f32 lights[NYA_SHADER_LIGHT2D_MAX][4];

    /** Per light: r, g, b, and one float of padding the shader ignores. */
    f32 colors[NYA_SHADER_LIGHT2D_MAX][4];

    /** How lit an unlit pixel is. The floor the falloff never goes below. */
    f32 ambient_r, ambient_g, ambient_b;

    /** How many entries above are real. A float because the shader reads it out of a float4 row. */
    f32 count;

    /** Pixels across the target, so a uv can be turned back into light coordinates. */
    f32 target_width, target_height;

    f32 _padding[2];
};

/**
 * sky3d.frag.hlsl: gradient, sun and camera basis. Every row is a float3 and its related scalar, which fills
 * the row. nya_render3d_sky_draw fills the camera rows from the batch.
 * */
struct NYA_ShaderSkyUniform {
    /** Camera basis, world space, unit length. */
    f32 camera_right_x, camera_right_y, camera_right_z;

    /** tan(fov_y / 2). Sets how wide the reconstructed rays fan out; the orthographic case passes zero. */
    f32 tangent;

    f32 camera_up_x, camera_up_y, camera_up_z;
    f32 aspect;

    f32 camera_forward_x, camera_forward_y, camera_forward_z;

    /** Exponent on the elevation ramp. One is linear; higher keeps the horizon colour further up. */
    f32 horizon_softness;

    /** Toward the sun, not the way its light travels. Matches the mesh shading's convention. */
    f32 sun_direction_x, sun_direction_y, sun_direction_z;

    /** Cosine of the disc's angular radius. 0.9995 is roughly the real sun; lower is a larger disc. */
    f32 sun_size;

    f32 zenith_r, zenith_g, zenith_b;

    /** Exponent on the halo. Low is a wide glow across the sky, high is a tight ring around the disc. */
    f32 sun_sharpness;

    f32 horizon_r, horizon_g, horizon_b;

    /** How far either side of level the sky fades into the ground colour. In sine-of-elevation units. */
    f32 ground_blend;

    f32 sun_r, sun_g, sun_b;
    f32 sun_intensity;

    /** Below the horizon: the colour distant ground reads as, not a lit surface. */
    f32 ground_r, ground_g, ground_b;
    f32 pad;
};

/**
 * effect_scene.hlsli: what a screen-space pass needs to turn a normal buffer texel back into a world position.
 * Embedded first in every block that reads the normal buffer. Filled by nya_post_end from the 3D batch.
 * */
struct NYA_ShaderSceneView {
    f32 right_x, right_y, right_z;

    /** tan(fov_y / 2), zero for an orthographic camera. */
    f32 tangent;

    f32 up_x, up_y, up_z;

    /** Width over height. */
    f32 aspect;

    f32 forward_x, forward_y, forward_z;

    /** Half the orthographic view's height, zero for a perspective camera. */
    f32 half_height;

    f32 eye_x, eye_y, eye_z;

    /** See NYA_Render3DFog. Zero is no fog. */
    f32 fog_density;

    f32 fog_height_falloff;
    f32 fog_height_base;

    /** One texel of the normal buffer in uv. */
    f32 texel_x, texel_y;
};

/** effect_ink.frag.hlsl. See NYA_PostInk; every field already has its default applied. */
struct NYA_ShaderInkUniform {
    struct NYA_ShaderSceneView view;

    f32 color_r, color_g, color_b, color_a;

    f32 width;

    /** The cosine of NYA_PostInk.crease, which is what a dot product compares against. */
    f32 crease_cosine;

    f32 fade_start;
    f32 fade_end;
};

/** effect_occlusion.frag.hlsl and effect_occlusion_apply.frag.hlsl. See NYA_PostAmbientOcclusion. */
struct NYA_ShaderAmbientOcclusionUniform {
    struct NYA_ShaderSceneView view;

    f32 radius;
    f32 strength;
    f32 band;
    f32 min_radius;

    f32 softness;
    f32 pad_0, pad_1, pad_2;
};

/** effect_ssao.frag.hlsl and effect_ssao_blur.frag.hlsl. See NYA_PostSsao. */
struct NYA_ShaderSsaoUniform {
    struct NYA_ShaderSceneView view;

    f32 radius;
    f32 bias;
    f32 strength;

    /** The hemisphere sample count, as a float for the row. The gather clamps it to SSAO_MAX_SAMPLES. */
    f32 samples;
};

/** effect_ssr.frag.hlsl. See NYA_PostSsr; every field already has its default applied. */
struct NYA_ShaderSsrUniform {
    struct NYA_ShaderSceneView view;

    /** How far a reflection ray travels, in world units, before it gives up. */
    f32 max_distance;

    /** How far behind stored geometry a marched point may sit and still count as a hit, in world units. */
    f32 thickness;

    /** How strongly the reflection composites over the scene, in [0, 1]. */
    f32 strength;

    /** The reflectivity head-on, the Schlick F0: grazing angles reflect more, straight-down surfaces this much. */
    f32 fresnel;

    /** March steps, as a float for the row. The march clamps it to SSR_MAX_STEPS. */
    f32 steps;
    f32 pad_0, pad_1, pad_2;

    /** The sky tint a missed ray reflects, above the horizon. */
    f32 sky_r, sky_g, sky_b, sky_pad;

    /** The ground tint a missed ray reflects, below the horizon. */
    f32 ground_r, ground_g, ground_b, ground_pad;
};

/** effect_antialias.frag.hlsl. See NYA_PostAntialias. */
struct NYA_ShaderAntialiasUniform {
    f32 texel_x, texel_y;
    f32 subpixel;
    f32 threshold;
};

/** effect_depth_of_field_blur.frag.hlsl and its composite. See NYA_PostDepthOfField. */
struct NYA_ShaderDepthOfFieldUniform {
    /** One texel of the full image, since the blur is measured in those. */
    f32 texel_x, texel_y;

    f32 radius;

    /** A NYA_PostFocus, as a float for the row. */
    f32 focus;

    /** Tilt shift, in uv: where the sharp band is centred and half its height. */
    f32 band_center, band;

    f32 falloff;
    f32 layers;

    f32 focus_distance, focus_range;
    f32 pad[2];
};

/** effect_adaptation_measure.frag.hlsl and effect_adaptation.frag.hlsl. See NYA_PostEyeAdaptation. */
struct NYA_ShaderEyeAdaptationUniform {
    f32 key;
    f32 exposure_min, exposure_max;
    f32 saturation;

    /** How much of the way to the measured brightness this frame moves, darker and brighter. One starts over. */
    f32 rate_dark, rate_bright;

    /** Where this frame's grid of taps sits within a cell, in [0, 1). */
    f32 jitter_x, jitter_y;
};

/** effect_light_shafts.frag.hlsl. See NYA_PostLightShafts; the composite is effect_bloom.frag.hlsl's. */
struct NYA_ShaderLightShaftsUniform {
    /** Where the sun is on screen, in uv, which can be off it. */
    f32 sun_x, sun_y;

    f32 length;
    f32 threshold;

    /** Width over height, so the glow around the sun stays round. */
    f32 aspect;

    f32 pad[3];
};

/** effect_motion_blur.frag.hlsl. See NYA_PostMotionBlur. */
struct NYA_ShaderMotionBlurUniform {
    struct NYA_ShaderSceneView view;

    /** The last frame's camera, which a point is projected through to find where it was. */
    f32_4x4 previous_view_projection;

    /** What the motion since the last frame is multiplied by, the frame's time and the strength in it. */
    f32 scale;

    /** The longest smear, in uv. */
    f32 longest;

    f32 pad[2];
};

/** effect_output_hdr.frag.hlsl, from NYA_RenderOutput with the defaults resolved. */
struct NYA_ShaderOutputUniform {
    /** 0 for extended linear sRGB, 1 for HDR10. */
    f32 encoding;

    f32 peak;
    f32 highlight;

    /** SDR white in nits, which HDR10 needs and linear output ignores. */
    f32 paper_white;

    /** 1 when the frame's alpha marks the scene, so only where alpha is low is lifted. See nya_render_output_scene_end. */
    f32 masked;

    f32 pad[3];
};

/** effect_speed_lines.frag.hlsl. See NYA_PostSpeedLines. */
struct NYA_ShaderSpeedLinesUniform {
    /** Where the lines converge, in uv. */
    f32 center_x, center_y;

    /** Width over height, so the lines stay round on a wide target. */
    f32 aspect;

    /** Which drawing this is: whole numbers, stepped at NYA_POST_SPEED_LINES_RATE. */
    f32 frame;

    f32 amount;

    /** Lines around the circle. Whole, so the last wedge meets the first. */
    f32 density;

    f32 clear_radius;

    /** One pixel in units of the target's height. */
    f32 pixel;

    f32 color_r, color_g, color_b, color_a;
};

/** effect_scene_debug.frag.hlsl: the ink's settings for the ink view, then the view and the cascades. */
struct NYA_ShaderSceneDebugUniform {
    struct NYA_ShaderInkUniform ink;

    /** A NYA_PostDebugView, as a float for the row. */
    f32 view;

    /** How many entries below are real. */
    f32 cascade_count;

    f32 pad[2];

    f32_4x4 light_view_projection[NYA_RENDER3D_SHADOW_CASCADES];
};

static_assert(sizeof(struct NYA_ShaderSceneView) == 80, "five rows, matching SceneView in effect_scene.hlsli");
static_assert(sizeof(struct NYA_ShaderSsrUniform) == 144, "SceneView and four rows, matching SsrUniform in effect_ssr.frag.hlsl");
static_assert(offsetof(struct NYA_ShaderSceneDebugUniform, light_view_projection) == 128, "the ink block and one row");

/**
 * mesh3d_glass.frag.hlsl: capture texel size, refraction and blur. A block at b1 so ordinary mesh draws do not
 * carry glass state.
 * */
struct NYA_ShaderGlassUniform {
    /** One texel of the capture in uv. Also what converts SV_POSITION into a lookup coordinate. */
    f32 texel_x, texel_y;

    /** See NYA_Render3DMaterial.refraction and .blur. */
    f32 refraction;
    f32 blur;
};

/**
 * mesh3d_skinned.vert.hlsl: the bone palette at b1, so static props do not carry it. Three rows per bone,
 * since the fourth row of an affine transform is always (0, 0, 0, 1); 3 KiB at sixty-four bones.
 * */
struct NYA_ShaderSkinUniform {
    /** Row major, three rows per bone. Copied from nya_skeleton_palette, which works in full matrices. */
    f32 bones[NYA_SHADER_SKIN_MAX_BONES][3][4];

    /** Multiplied into the vertex colour. Its own float4, since a cbuffer will not split one. */
    f32 tint_r, tint_g, tint_b, tint_a;
};

/**
 * foliage.vert.hlsl: the plant's placement, the sampled wind, and the sway parameters, at b1 so static
 * meshes do not carry it. The vertex stage bends model-space geometry about its base (the model origin)
 * before view-projecting, reusing mesh3d.frag for shading. Field order matters: each row below is one
 * float4, and the matrix is four rows, so nothing crosses a sixteen-byte boundary.
 * */
struct NYA_ShaderFoliageUniform {
    /** Row major, the same convention as light_view_projection: mul(model, vertex) in the shader. */
    f32_4x4 model;

    /** The wind's displacement/force at the plant, from nya_wind_sample, and the field's time in w. */
    f32 wind_x, wind_y, wind_z;
    f32 time;

    /** Tip sway as a fraction of height, the primary bend rate, 0..1 rigidity, and leaf-flutter amplitude. */
    f32 amplitude, frequency, stiffness, flutter;

    /** Flutter rate, a per-object phase offset, one over the plant's height, and one float of padding. */
    f32 detail_frequency, phase, height_scale, pad;

    /** Multiplied into the vertex colour, so one authored plant draws in many tints. */
    f32 tint_r, tint_g, tint_b, tint_a;

    /**
     * The nearest disturbers to this plant: each row is a world position in xyz and its radius in w.
     * The plant bends away from any it sits inside. Entries past `disturber_count` are stale and ignored.
     * See nya_render3d_foliage_disturb and NYA_RENDER3D_FOLIAGE_DISTURBERS.
     * */
    f32 disturber_position_radius[NYA_RENDER3D_FOLIAGE_DISTURBERS][4];

    /** One strength per disturber, packed into a single row since there are four of them. */
    f32 disturber_strength[4];

    /** How many of the rows above are live, as a float so it shares a float4 row. */
    f32 disturber_count;
    f32 disturber_pad[3];
};

static_assert(sizeof(struct NYA_ShaderFoliageUniform) == 224,
              "a float4x4, four float4 rows, four disturber rows, a strength row and a count row, matching foliage.vert.hlsl");

/**
 * water.vert.hlsl: the surface's placement, the current, the wave shape and the sampled wind, at b1 so a
 * static mesh does not carry it. The vertex stage lifts model-space vertices into travelling waves about the
 * still plane (y = 0) before view-projecting, reusing the lit fragment path for shading. Each row below is one
 * float4 and the matrix is four rows, so nothing crosses a sixteen-byte boundary — std140-expressible, which
 * is what lets it cross-compile to GLSL ES 300.
 * */
struct NYA_ShaderWaterVertexUniform {
    /** Row major, the same convention as foliage's model: mul(model, vertex) in the shader. */
    f32_4x4 model;

    /** The current's heading on the ground (x, z), how fast crests travel, and the field's time in w. */
    f32 flow_x, flow_z, flow_speed, time;

    /** Wave height, wavelength (as a frequency), travel speed and the Gerstner choppiness that sharpens crests. */
    f32 amplitude, frequency, wave_speed, choppiness;

    /** The wind's horizontal push (x, z) from nya_wind_sample, how much it drives the chop, and one float of padding. */
    f32 wind_x, wind_z, wind_influence, wind_pad;
};

static_assert(sizeof(struct NYA_ShaderWaterVertexUniform) == 112,
              "a float4x4 and three float4 rows, matching the Water cbuffer in water.vert.hlsl");

/**
 * water.frag.hlsl: the refraction capture's texel size and strength, the deep and shallow body colours, the
 * foam and opacity, the Fresnel reflection tint, the flow, and the ripple detail. A block at b1 so the plain
 * lit pipelines do not carry water state. Each row is one float4, matching the cbuffer field for field.
 * */
struct NYA_ShaderWaterFragUniform {
    /** One capture texel in uv (x, y), the refraction strength, and 1 when the capture is live (0 falls back). */
    f32 texel_x, texel_y, refraction, has_refraction;

    /** The deep channel colour; alpha is how murky the deep body is (how much it hides the refracted scene). */
    f32 deep_r, deep_g, deep_b, deep_murk;

    /** The shallow bank colour; alpha is the shallow body's murk, usually lower so the banks read clearer. */
    f32 shallow_r, shallow_g, shallow_b, shallow_murk;

    /** Shore foam band width (in shore-weight), the crest foam threshold, the crest foam softness, and the surface opacity. */
    f32 shore_width, crest_threshold, foam_softness, opacity;

    /** The Fresnel exponent, then the reflection tint the surface leans toward at grazing angles. */
    f32 fresnel_power, reflection_r, reflection_g, reflection_b;

    /** The current's heading (x, z), the ripple flow-map cycle in seconds, and the field's time. */
    f32 flow_x, flow_z, flow_cycle, flow_time;

    /**
     * The ripple field's spatial scale, its normal strength, its travel speed, and the planar-reflection blend:
     * zero samples no reflection texture and the surface keeps its flat Fresnel tint, a positive value samples
     * the mirrored-sky reflection capture at the fragment's screen position and Fresnel-blends it in.
     * */
    f32 ripple_scale, ripple_strength, ripple_speed, reflection;

    /**
     * The depth-difference shoreline foam: how much the true water depth over the bed drives the shore foam (0
     * keeps the authored shore band), the world-space depth over which that foam fades from full at the waterline
     * to none, 1 when the scene distance buffer is live (0 falls back to the authored band), and one float of
     * padding. See scene_distance in water.frag.hlsl.
     * */
    f32 depth_strength, depth_shore, has_depth, depth_pad;
};

static_assert(sizeof(struct NYA_ShaderWaterFragUniform) == 128,
              "eight float4 rows, matching the WaterUniform cbuffer in water.frag.hlsl");
