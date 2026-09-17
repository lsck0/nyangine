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

#include "nyangine/base/base_types.h"

// For NYA_RENDER3D_MAX_POINT_LIGHTS and NYA_RENDER3D_SHADOW_CASCADES, which size the arrays below.
#include "nyangine/renderer/render3d.h"

typedef struct NYA_ShaderBlurUniform     NYA_ShaderBlurUniform;
typedef struct NYA_ShaderPixelateUniform NYA_ShaderPixelateUniform;
typedef struct NYA_ShaderBloomUniform    NYA_ShaderBloomUniform;
typedef struct NYA_ShaderLutUniform      NYA_ShaderLutUniform;
typedef struct NYA_ShaderCrtUniform      NYA_ShaderCrtUniform;
typedef struct NYA_ShaderSkyUniform      NYA_ShaderSkyUniform;
typedef struct NYA_ShaderGlassUniform    NYA_ShaderGlassUniform;

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

/** effect_bloom.frag.hlsl. */
struct NYA_ShaderBloomUniform {
    /** One texel in uv, so the sample offsets are resolution independent. */
    f32 texel_x, texel_y;

    /** Luminance above which a pixel contributes to the glow. Around 0.6 for text on a dark panel. */
    f32 threshold;

    /** How strongly the glow is added back. 1.0 is a soft halo; past 2 it blows out. */
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
    f32 fog_pad;

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
    f32 pad;
};

/** effect_antialias.frag.hlsl. See NYA_PostAntialias. */
struct NYA_ShaderAntialiasUniform {
    f32 texel_x, texel_y;
    f32 subpixel;
    f32 threshold;
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
