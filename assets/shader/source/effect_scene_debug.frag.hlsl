// Shows one of the buffers the cartoon passes read, in place of the image. See NYA_PostDebugView, whose values the
// view numbers below are.

#include "effect_scene.hlsli"

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

Texture2D normals : register(t1, space2);
SamplerState normals_sampler : register(s1, space2);

Texture2D occlusion : register(t2, space2);
SamplerState occlusion_sampler : register(s2, space2);

/** Has to match NYA_RENDER3D_SHADOW_CASCADES, like MESH3D_SHADOW_CASCADES. */
#define DEBUG_SHADOW_CASCADES 3

// Matches NYA_ShaderSceneDebugUniform.
cbuffer SceneDebugUniform : register(b0, space3) {
  SceneView view;

  float4 ink_color;
  float ink_width;
  float crease_cosine;
  float fade_start;
  float fade_end;

  float debug_view;
  float cascade_count;
  float2 debug_pad;

  float4x4 light_view_projection[DEBUG_SHADOW_CASCADES];
};

/** The distance shown as mid grey, in world units. */
static const float DEBUG_DEPTH_MIDDLE = 20.0;

float4 main(FragInput input) : SV_Target {
  float2 uv = scene_uv(input);

  float4 colour = source.SampleLevel(source_sampler, uv, 0.0);
  float4 centre = scene_texel(normals, normals_sampler, view, input.position.xy, float2(0.0, 0.0));

  int mode = (int)debug_view;

  if (mode == 1) return float4(centre.a > 0.0 ? (centre.rgb * 0.5) + 0.5 : 0.0, 1.0);

  if (mode == 2) return float4((centre.a > 0.0 ? DEBUG_DEPTH_MIDDLE / (centre.a + DEBUG_DEPTH_MIDDLE) : 0.0).xxx, 1.0);

  if (mode == 3) return float4((1.0 - occlusion.SampleLevel(occlusion_sampler, uv, 0.0).r).xxx, 1.0);

  if (mode == 4) {
    float ink = scene_ink(normals, normals_sampler, view, input.position.xy, ink_width, crease_cosine, fade_start, fade_end);

    return float4((1.0 - ink).xxx, 1.0);
  }

  if (centre.a <= 0.0) return colour;

  // the first cascade whose volume holds the point, as mesh3d_cascade_for picks without its filter inset.
  float3 position = scene_position(view, uv, centre.a);

  const float3 tints[DEBUG_SHADOW_CASCADES] = { float3(1.0, 0.35, 0.35), float3(0.35, 1.0, 0.35), float3(0.35, 0.35, 1.0) };

  for (int i = 0; i < min((int)cascade_count, DEBUG_SHADOW_CASCADES); i++) {
    float4 clip = mul(light_view_projection[i], float4(position, 1.0));
    float3 projected = clip.xyz / clip.w;

    if (all(abs(projected.xy) <= 1.0) && projected.z >= 0.0 && projected.z <= 1.0) return float4(colour.rgb * tints[i], colour.a);
  }

  return float4(colour.rgb * 0.4, colour.a);
}
