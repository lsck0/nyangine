// Fragment stage for the *textured* 3D mesh batch. See src/nyangine/renderer/render3d.h.
//
// Identical to mesh3d.frag.hlsl but for one expression: the base colour is the vertex colour multiplied
// by a sampled texture. Everything after that is mesh3d_shading.hlsli, shared between the two.

#include "mesh3d_shading.hlsli"

/*
 * The shadow map, at t1/s1.
 *
 * The register differs between the two pipelines because they bind a different number of textures — this
 * one has a base colour map at t0, so the shadow follows at t1. That is why mesh3d_shadow takes the texture as a
 * parameter instead of reading a global out of the shared include.
 */
/*
 * The base colour at t0 and the shadow map at t1, in that order.
 *
 * The order is the contract with the flush, which binds the pair as one array — and with the pipeline,
 * which has to declare exactly two samplers. All three have to agree; two of them disagreeing is what
 * faulted the GPU.
 */
Texture2D base_texture : register(t0, space2);
SamplerState base_sampler : register(s0, space2);

Texture2D shadow_map : register(t1, space2);
SamplerState shadow_sampler : register(s1, space2);

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
  float3 world_position : TEXCOORD1;
};

float4 main(FragInput input) : SV_Target {
  float3 normal = normalize(input.normal);

  /*
   * The texture modulates the vertex colour rather than replacing it.
   *
   * Multiplying is what makes one atlas serve many objects: a model's material colour and the tint a
   * caller passes both survive, so the same texture can be drawn warm or cool without a second copy. A
   * model that wants the atlas verbatim asks for white.
   */
  float4 sampled = base_texture.Sample(base_sampler, input.uv);

  // Sampled before shading so the factor can attenuate the directional terms inside it.
  float shadow = mesh3d_shadow(shadow_map, shadow_sampler, input.world_position, saturate(dot(normal, light_direction)));

  float3 colour = mesh3d_shade(input.color.rgb * sampled.rgb, normal, input.world_position, shadow);

  return float4(mesh3d_tonemap(colour), input.color.a * sampled.a);
}
