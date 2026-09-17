// Camera motion blur: each pixel smeared along the way the point it shows moved on screen since the last frame, found
// from the normal buffer's distance and the last frame's camera. See NYA_PostMotionBlur.

#include "effect_scene.hlsli"

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

Texture2D normals : register(t1, space2);
SamplerState normals_sampler : register(s1, space2);

// Matches NYA_ShaderMotionBlurUniform.
cbuffer MotionBlurUniform : register(b0, space3) {
  SceneView view;

  float4x4 previous_view_projection;

  float scale;
  float longest;
  float2 motion_pad;
};

static const int TAPS = 8;

// the sky has no distance of its own, but still turns with the camera: far enough that moving does not shift it.
static const float SKY_DISTANCE = 1000.0;

float4 main(FragInput input) : SV_Target {
  float2 uv = scene_uv(input);

  float distance = normals.SampleLevel(normals_sampler, uv, 0.0).a;
  float3 position = scene_position(view, uv, distance > 0.0 ? distance : SKY_DISTANCE);

  // no last camera yet, or the point was behind it.
  float4 clip = mul(previous_view_projection, float4(position, 1.0));

  if (clip.w <= 1e-4) return source.SampleLevel(source_sampler, uv, 0.0);

  float2 was = float2((clip.x / clip.w) * 0.5 + 0.5, 0.5 - (clip.y / clip.w) * 0.5);
  float2 motion = (uv - was) * scale;

  float travelled = length(motion);

  if (travelled > longest) motion *= longest / travelled;

  // under half a pixel there is nothing to smear.
  if (travelled < view.texel.y * 0.5) return source.SampleLevel(source_sampler, uv, 0.0);

  float4 sum = 0.0;

  [unroll]
  for (int i = 0; i < TAPS; i++) sum += source.SampleLevel(source_sampler, uv + motion * (((float)i + 0.5) / TAPS - 0.5), 0.0);

  return sum / TAPS;
}
