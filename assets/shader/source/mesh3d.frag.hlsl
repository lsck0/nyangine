// Fragment stage for the untextured 3D mesh batch. See src/nyangine/renderer/render3d.h.
//
// The shading lives in mesh3d_shading.hlsli, shared with the textured pipeline. This file is the half
// that differs: where the base colour comes from. Here it is the vertex colour alone.
//
// Two pipelines rather than one with a branch, because a shader that declares a sampler must have one
// bound on every draw and SDL_GPU has no notion of an unbound binding. The alternative was a 1x1 white
// texture bound for every untextured primitive — a real texture fetch per fragment to multiply by one.
// render2d made the same call for the same reason; see NYA_RENDER2D_PIPELINE_SHAPES against _TEXTURED.

#include "mesh3d_shading.hlsli"

/*
 * The shadow map, at t0/s0.
 *
 * The register differs between the two pipelines because they bind a different number of textures — this
 * one has no base colour map, so the shadow map is the first and only binding. That is why mesh3d_shadow takes the texture as a
 * parameter instead of reading a global out of the shared include.
 */
Texture2D shadow_map : register(t0, space2);
SamplerState shadow_sampler : register(s0, space2);

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
  float3 world_position : TEXCOORD1;
};

float4 main(FragInput input) : SV_Target {
  // Normalized once, here, rather than in the vertex stage: interpolating two unit normals across a
  // triangle gives something shorter than unit in the middle, and normalizing before the interpolation
  // cannot fix that.
  float3 normal = normalize(input.normal);

  // Sampled before shading so the factor can attenuate the directional terms inside it.
  float shadow = mesh3d_shadow(shadow_map, shadow_sampler, input.world_position, normal);

  float3 colour = mesh3d_shade(input.color.rgb, normal, input.world_position, shadow);

  /*
   * Tonemapped, not clamped.
   *
   * This said "clamped, not tonemapped" and gave the reason that nothing in the shading model produces a
   * value far past one. That was wrong about its own model: emission deliberately pushes past one — see
   * NYA_Render3DMaterial.emission, whose whole job is to lift a surface past the bloom threshold — and
   * `saturate` then threw that away, mapping an emissive lamp and a lit white wall onto the same number.
   *
   * mesh3d_tonemap is not the Reinhard curve the note was rejecting, and the objection to that curve was
   * right: it is identity below the knee, so flat authored colour survives untouched. See its definition.
   *
   * Alpha passes through untouched: the shading is a multiply on rgb, and folding it into alpha would
   * make a shaded face transparent as well as dark.
   */
  return float4(mesh3d_tonemap(colour), input.color.a);
}
