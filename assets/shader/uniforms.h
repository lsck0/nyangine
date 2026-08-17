/**
 * @file uniforms.h
 *
 * The C side of every shader's uniform block, in one place.
 *
 * A uniform block is a contract between two files that no compiler checks: the `cbuffer` in the
 * HLSL and the struct the caller pushes with nya_render2d_shader_set_uniform have to agree on field
 * order, type and padding, and nothing says so if they drift. Getting it wrong is not a build error
 * and not a validation error — the shader reads whatever the bytes happen to mean.
 *
 * These structs used to be declared inline at each call site, so the same block was written out
 * again wherever it was used and each copy had to be kept in step with the HLSL independently. One
 * declaration per shader, next to the shaders, is the only arrangement where a change to a `cbuffer`
 * has an obvious single place to be mirrored.
 *
 * ## Rules for adding one
 *
 * Name it `NYA_Shader<Name>Uniform` and keep the field order identical to the `cbuffer`. HLSL packs
 * constant buffers into four-component vectors and will not split a member across a boundary, so
 * group scalars in fours or pad deliberately — a `float2` after three floats does not land where a C
 * struct would put it. Every block here is currently four floats or fewer for exactly that reason.
 *
 * ## Why this is not generated
 *
 * It could be parsed out of the HLSL, and that would be one more thing in the build to go wrong for
 * a handful of structs. This file is not compiled *into* the shaders either — it is a header for the
 * C side only, which is why the build excludes it from the asset index rather than treating it as
 * something to load at runtime.
 * */
#pragma once

#include "nyangine/base/base_types.h"

// For NYA_RENDER3D_MAX_POINT_LIGHTS and NYA_RENDER3D_SHADOW_CASCADES, which size the arrays below.
#include "nyangine/renderer/render3d.h"

typedef struct NYA_ShaderBlurUniform     NYA_ShaderBlurUniform;
typedef struct NYA_ShaderPixelateUniform NYA_ShaderPixelateUniform;
typedef struct NYA_ShaderBloomUniform    NYA_ShaderBloomUniform;
typedef struct NYA_ShaderCrtUniform      NYA_ShaderCrtUniform;
typedef struct NYA_ShaderSkyUniform      NYA_ShaderSkyUniform;
typedef struct NYA_ShaderOutlineUniform  NYA_ShaderOutlineUniform;
typedef struct NYA_ShaderGlassUniform    NYA_ShaderGlassUniform;

/** effect_blur.frag.hlsl. One directional pass; run it twice, transposed, for a real gaussian. */
struct NYA_ShaderBlurUniform {
    /** One texel in uv, so the sample offsets are resolution independent. */
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
 * mesh3d.frag.hlsl. The metallic-roughness material and the one light shading it.
 *
 * Field order and padding are load-bearing: HLSL packs a cbuffer into sixteen-byte rows and will not
 * split a float3 across one, so a float3 followed by a float fills a row exactly. The four rows
 * below are that rule obeyed four times; reordering these fields silently reads the wrong ones.
 *
 * Per flush rather than per draw, which is what keeps a batch a batch — and is also how a real
 * renderer sorts, by material. Base colour stays per vertex, so one material can cover many
 * differently coloured objects in a single draw call.
 * */
struct NYA_ShaderMesh3DUniform {
    /** From the surface toward the light, normalized. See the shader for why this direction. */
    f32 light_direction_x, light_direction_y, light_direction_z;

    /** Stands in for image based lighting. Around 0.25 reads as a lit room; 0 is airless. */
    f32 ambient;

    f32 light_color_r, light_color_g, light_color_b;

    /** Scales the direct term only. One is neutral. */
    f32 intensity;

    /** Where the view is from. A specular highlight is a function of this; a Lambert one is not. */
    f32 camera_x, camera_y, camera_z;

    /** 0 dielectric, 1 metal. In between is a blend, not a material. */
    f32 metallic;

    /** Perceptual roughness in [0, 1]. Squared into the GGX alpha by the shader. */
    f32 roughness;

    /** Rim strength on the silhouette. See NYA_Render3DMaterial.reflectance. */
    f32 reflectance;

    /** How much of the base colour is added regardless of light. See NYA_Render3DMaterial.emission. */
    f32 emission;

    /**
     * How many of the arrays below are live, as a float.
     *
     * A float rather than a u32 because it shares a sixteen byte row with three floats, and HLSL reading
     * an `int` out of a row of floats is a reinterpretation waiting to be got wrong. It costs a cast in
     * the shader and removes a packing question.
     * */
    f32 point_light_count;

    /**
     * Point lights, split across two arrays rather than an array of structs.
     *
     * HLSL pads every element of a constant buffer array up to sixteen bytes, so a struct of a float3 and
     * two floats would occupy thirty-two with four wasted. Two parallel float4 arrays — position with
     * range, colour with intensity — waste nothing and are what the shader indexes anyway.
     *
     * Has to match NYA_RENDER3D_MAX_POINT_LIGHTS and MESH3D_MAX_POINT_LIGHTS in mesh3d_shading.hlsli.
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
     * One light view-projection per cascade, matching what each shadow pass rendered with.
     *
     * f32_4x4 rather than sixteen floats, and safe for the same reason the vertex stage's
     * view_projection is pushed as one: the matrix extension's layout is what HLSL reads as a row-major
     * float4x4, and this is the same value that pass already used.
     *
     * An array now rather than one matrix, because a fragment picks its cascade from how far away it is
     * and then needs *that* cascade's matrix. Unfilled entries are whatever the previous frame left, which
     * is harmless: `cascade_count` bounds the selection, so nothing indexes past the ones that ran.
     * */
    f32_4x4 light_view_projection[NYA_RENDER3D_SHADOW_CASCADES];

    /**
     * How far each cascade reaches from the shadow volume's centre, in world units.
     *
     * The split scheme, in the only form the shader needs it: a fragment takes the first cascade whose
     * distance covers it. Stored as a float4 rather than an array of floats because HLSL pads every
     * cbuffer array element out to sixteen bytes, which would make four floats occupy sixty-four.
     * */
    f32 cascade_extent[4];

    /** How many cascades actually ran this frame. A float, for the reason `point_light_count` is one. */
    f32 cascade_count;

    /** Padding to close the row. HLSL will not split the next member across a sixteen-byte boundary. */
    f32 cascade_pad[3];
};

/** Lights one nya_render2d_lights_apply may pass. Matches MAX_LIGHTS in light2d.frag.hlsl. */
#define NYA_SHADER_LIGHT2D_MAX 16

/**
 * Bones one skinned draw may use. Must equal NYA_SKELETON_MAX_BONES.
 *
 * Declared here as well as in core_skeleton.h because this header is shared with the shaders, which
 * cannot include engine headers. The two are checked against each other at compile time; see the
 * static assertion beside nya_render3d_skinned_mesh.
 * */
#define NYA_SHADER_SKIN_MAX_BONES 64

/**
 * light2d.frag.hlsl. The 2D light map's lights and its ambient floor.
 *
 * Two parallel arrays rather than an array of structs, because HLSL pads every cbuffer array element
 * out to sixteen bytes — so a struct of a float3 and a float2 would occupy thirty-two and waste half
 * of it. Packed this way the whole block is 544 bytes, which is one uniform push.
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
 * sky3d.frag.hlsl. The 3D sky's gradient, sun and camera basis.
 *
 * Every row is a float3 followed by a scalar, which is not a stylistic choice: HLSL packs a constant
 * buffer into four-component vectors and refuses to split a member across one, so a float3 is followed by
 * exactly sixteen bytes' worth of members or by padding. Pairing each direction with the scalar that
 * belongs to it costs nothing and puts the padding to work.
 *
 * Built by nya_render3d_sky_draw, which fills the camera rows from the batch — a caller supplies only the
 * colours and the sun, since the basis is not theirs to get wrong.
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

    /** What is below the horizon. Not a lit surface — the colour the world reads as at a distance. */
    f32 ground_r, ground_g, ground_b;
    f32 pad;
};

/**
 * mesh3d_outline.vert.hlsl. The ink's colour and how far the hull is pushed out.
 *
 * Its own block rather than a pair of fields on the mesh uniform, because the outline pipeline runs a
 * fragment shader that reads no light, no material and no shadow map — giving it the scene's block would
 * mean pushing a hundred and fifty bytes of lighting per outline draw for two values.
 * */
struct NYA_ShaderOutlineUniform {
    f32 color_r, color_g, color_b, color_a;

    /** World units at the model's own scale. See nya_render3d_outline_set. */
    f32 thickness;

    /** HLSL will not split a float3 across a sixteen-byte boundary; this is the rest of the row. */
    f32 pad[3];
};

/**
 * mesh3d_glass.frag.hlsl. Where the captured scene is, and how far the glass bends and blurs it.
 *
 * A second fragment block at b1, beside the shading block at b0. Separate because the shading block is
 * shared with two pipelines that have no capture to sample — adding these to it would push four bytes of
 * glass state through every ordinary mesh draw in the frame.
 * */
struct NYA_ShaderGlassUniform {
    /** One texel of the capture in uv. Also what converts SV_POSITION into a lookup coordinate. */
    f32 texel_x, texel_y;

    /** See NYA_Render3DMaterial.refraction and .blur. */
    f32 refraction;
    f32 blur;
};

/**
 * mesh3d_skinned.vert.hlsl. The bone palette, as linear blend skinning consumes it.
 *
 * A vertex block at b1, beside the view-projection at b0, for the same reason the outline's is: the
 * shared block is what every mesh pipeline pushes, and three kibibytes of bone matrices have no
 * business travelling with a static prop's draw.
 *
 * **Three rows, not four.** A bone transform is affine, so its last row is always (0, 0, 0, 1) and
 * sending it costs a quarter of the block to say nothing. The shader rebuilds the row it needs. At
 * sixty four bones this is 3 KiB rather than 4.
 * */
struct NYA_ShaderSkinUniform {
    /**
     * Row major, three rows per bone: `bones[i][0..2]` are the rows of bone `i`'s matrix.
     *
     * Written by nya_skeleton_palette and copied here, rather than the palette being this shape to
     * begin with — the palette is composed with full matrix multiplies, and a three-row type would
     * have to reconstitute the fourth row for every one of them.
     * */
    f32 bones[NYA_SHADER_SKIN_MAX_BONES][3][4];

    /** Multiplied into the vertex colour. A float4 of its own, since a cbuffer will not split one. */
    f32 tint_r, tint_g, tint_b, tint_a;
};
