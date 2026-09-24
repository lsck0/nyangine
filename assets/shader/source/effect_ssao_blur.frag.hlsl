// Blurs the half resolution SSAO back up to full size and darkens the scene by it, as a gradient. See NYA_PostSsao.
//
// The blur weighs each tap by how close its depth is to this pixel's, the same bilateral trick effect_occlusion_apply
// uses, so occlusion on a wall does not bleed onto the floor in front of it, and the 4x4 box cancels the rotation the
// gather turned per pixel.

#include "effect_scene.hlsli"

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

Texture2D normals : register(t1, space2);
SamplerState normals_sampler : register(s1, space2);

Texture2D ssao : register(t2, space2);
SamplerState ssao_sampler : register(s2, space2);

// Matches NYA_ShaderSsaoUniform.
cbuffer SsaoUniform : register(b0, space3) {
  SceneView view;

  float radius;
  float bias;
  float strength;
  float samples;
};

float4 main(FragInput input) : SV_Target {
  float2 uv = scene_uv(input);

  float4 colour   = source.SampleLevel(source_sampler, uv, 0.0);
  float  distance = scene_texel(normals, normals_sampler, view, input.position.xy, float2(0.0, 0.0)).a;

  if (distance <= 0.0) return colour;

  float total   = 0.0;
  float weights = 0.0;

  [unroll]
  for (int y = 0; y < 4; y++) {
    [unroll]
    for (int x = 0; x < 4; x++) {
      // four by four taps a texel apart, one per rotation of the gather's pattern.
      float2 tap = uv + ((float2((float)x, (float)y) - 1.5) * view.texel * 2.0);

      float tap_distance = normals.SampleLevel(normals_sampler, tap, 0.0).a;

      // five percent of the distance apart counts as another surface, so the blur does not cross a silhouette.
      float weight = tap_distance > 0.0 ? saturate(1.0 - (abs(tap_distance - distance) / (distance * 0.05))) : 0.0;

      total   += ssao.SampleLevel(ssao_sampler, tap, 0.0).r * weight;
      weights += weight;
    }
  }

  float occluded = weights > 0.0 ? total / weights : 0.0;

  float3 position = scene_position(view, uv, distance);

  // a plain multiply rather than a band, so the shading reads as a soft gradient into the crevices, and fog takes it
  // over the distance it takes the surface.
  colour.rgb *= 1.0 - (occluded * strength * (1.0 - scene_fog(view, position, distance)));

  return colour;
}
