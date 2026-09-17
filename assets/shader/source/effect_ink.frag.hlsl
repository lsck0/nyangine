// Screen-space ink over the scene. See NYA_PostInk, and scene_ink for where the lines go.

#include "effect_scene.hlsli"

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

Texture2D normals : register(t1, space2);
SamplerState normals_sampler : register(s1, space2);

// Matches NYA_ShaderInkUniform.
cbuffer InkUniform : register(b0, space3) {
  SceneView view;

  float4 ink_color;

  float ink_width;
  float crease_cosine;
  float fade_start;
  float fade_end;
};

float4 main(FragInput input) : SV_Target {
  float4 colour = source.SampleLevel(source_sampler, scene_uv(input), 0.0);

  float ink = scene_ink(normals, normals_sampler, view, input.position.xy, ink_width, crease_cosine, fade_start, fade_end);

  colour.rgb = lerp(colour.rgb, ink_color.rgb, ink * ink_color.a);

  return colour;
}
