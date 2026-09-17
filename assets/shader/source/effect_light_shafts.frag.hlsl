// Light shafts at half resolution: bright sky seen past the scene's silhouettes, gathered along the line from each
// pixel back toward the sun. effect_bloom.frag.hlsl adds them over the image. See NYA_PostLightShafts.

#include "effect_scene.hlsli"

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

Texture2D normals : register(t1, space2);
SamplerState normals_sampler : register(s1, space2);

// Matches NYA_ShaderLightShaftsUniform.
cbuffer LightShaftsUniform : register(b0, space3) {
  float2 sun;
  float shaft_length;
  float threshold;

  float aspect;
  float3 shafts_pad;
};

static const int TAPS = 32;

// each tap further along counts a little less, so a shaft thins out toward the sun.
static const float DECAY = 0.97;

float4 main(FragInput input) : SV_Target {
  float2 uv = scene_uv(input);
  float2 stride = (sun - uv) * (shaft_length / TAPS);

  // an ordered offset along the line, so the steps between taps dissolve rather than repeat a silhouette.
  const float pattern[16] = { 0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0 };

  uint2 cell = (uint2)input.position.xy % 4;
  float offset = pattern[cell.y * 4 + cell.x] / 16.0;

  float3 sum = 0.0;
  float weight = 1.0;

  [unroll]
  for (int i = 0; i < TAPS; i++) {
    float2 tap = uv + stride * ((float)i + offset);

    // sky only, where no surface was drawn, and only around the sun.
    float open = normals.SampleLevel(normals_sampler, tap, 0.0).a <= 0.0 ? 1.0 : 0.0;
    float near_sun = saturate(1.0 - length((tap - sun) * float2(aspect, 1.0)) * 3.0);

    float3 colour = source.SampleLevel(source_sampler, tap, 0.0).rgb;
    float bright = smoothstep(threshold, threshold + 0.25, dot(colour, float3(0.2126, 0.7152, 0.0722)));

    sum += colour * (open * bright * near_sun * near_sun * weight);
    weight *= DECAY;
  }

  return float4(sum / TAPS, 1.0);
}
