// Blurs the half resolution occlusion back up to full size and darkens the scene by it, as a band or a gradient. See
// NYA_PostAmbientOcclusion.
//
// The blur weighs each tap by how close its depth is to this pixel's, so occlusion on a wall does not bleed onto
// the floor in front of it, which a plain upsample would do along every silhouette.

#include "effect_scene.hlsli"

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

Texture2D normals : register(t1, space2);
SamplerState normals_sampler : register(s1, space2);

Texture2D occlusion : register(t2, space2);
SamplerState occlusion_sampler : register(s2, space2);

// Matches NYA_ShaderAmbientOcclusionUniform.
cbuffer OcclusionUniform : register(b0, space3) {
  SceneView view;

  float radius;
  float strength;
  float band;
  float min_radius;

  float softness;
  float3 occlusion_pad;
};

float4 main(FragInput input) : SV_Target {
  float2 uv = scene_uv(input);

  float4 colour = source.SampleLevel(source_sampler, uv, 0.0);
  float distance = scene_texel(normals, normals_sampler, view, input.position.xy, float2(0.0, 0.0)).a;

  if (distance <= 0.0) return colour;

  float total = 0.0;
  float weights = 0.0;

  [unroll]
  for (int y = 0; y < 4; y++) {
    [unroll]
    for (int x = 0; x < 4; x++) {
      // four by four taps a texel apart, one per rotation of the occlusion's pattern.
      float2 tap = uv + ((float2((float)x, (float)y) - 1.5) * view.texel * 2.0);

      float tap_distance = normals.SampleLevel(normals_sampler, tap, 0.0).a;

      // five percent of the distance apart counts as another surface.
      float weight = tap_distance > 0.0 ? saturate(1.0 - (abs(tap_distance - distance) / (distance * 0.05))) : 0.0;

      total += occlusion.SampleLevel(occlusion_sampler, tap, 0.0).r * weight;
      weights += weight;
    }
  }

  float occluded = weights > 0.0 ? total / weights : 0.0;

  float shade = smoothstep(band - softness, band + softness, occluded) * strength;

  float3 position = scene_position(view, uv, distance);

  colour.rgb *= 1.0 - (shade * (1.0 - scene_fog(view, position, distance)));

  return colour;
}
