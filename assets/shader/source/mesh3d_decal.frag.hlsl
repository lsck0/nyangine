// Fragment stage for decals. See src/nyangine/renderer/render3d_decal.h.
//
// The textured mesh shading with the texture's alpha cut at one half, so a decal ends in an edge one pixel
// wide instead of a soft fade, and its colour steps through the same light bands as the ground under it.

#include "mesh3d_shading.hlsli"
#include "mesh3d_normals.hlsli"

Texture2D decal_texture : register(t0, space2);
SamplerState decal_sampler : register(s0, space2);

Texture2D shadow_map : register(t1, space2);
SamplerState shadow_sampler : register(s1, space2);

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
  float3 world_position : TEXCOORD1;
};

Mesh3DOutput main(FragInput input) {
  float4 sampled = decal_texture.Sample(decal_sampler, input.uv);

  // a grid vertex that found no ground has a zero normal, so the length falls toward it and the same cut
  // ends the decal halfway there. the normals of one decal are close to parallel, so the length is the mix.
  float shape = min(sampled.a, length(input.normal));

  float coverage = saturate((shape - 0.5) / max(fwidth(shape), 0.0001) + 0.5);

  if (coverage <= 0.0) discard;

  float3 normal = normalize(input.normal);
  float shadow = mesh3d_shadow(shadow_map, shadow_sampler, input.world_position, normal);
  float3 colour = mesh3d_shade(input.color.rgb * sampled.rgb, normal, input.world_position, shadow);

  // the pipeline writes no depth, so the normal output is masked and the surface below keeps its own.
  return mesh3d_output(float4(mesh3d_fog(mesh3d_tonemap(colour), input.world_position), coverage * input.color.a), normal, input.world_position);
}
