// Ambient occlusion at half resolution, raw: how much nearby geometry faces this surface, in red. See
// NYA_PostAmbientOcclusion. Eight samples on a spiral turned per pixel in a 4x4 pattern, which
// effect_occlusion_apply.frag.hlsl blurs away.

#include "effect_scene.hlsli"

Texture2D normals : register(t0, space2);
SamplerState normals_sampler : register(s0, space2);

// Matches NYA_ShaderAmbientOcclusionUniform.
cbuffer OcclusionUniform : register(b0, space3) {
  SceneView view;

  float radius;
  float strength;
  float band;
  float min_radius;
};

static const int OCCLUSION_SAMPLES = 8;

/** The golden angle, so successive samples never line up. */
static const float OCCLUSION_TURN = 2.39996;

/** Reach limits in full resolution pixels: under two the taps read the centre, past forty they stop agreeing. */
static const float OCCLUSION_REACH_MIN = 2.0;
static const float OCCLUSION_REACH_MAX = 40.0;

/**
 * Scales the average onto [0, 1]. Half the taps of a contact point land on open ground, and the rest face it at
 * an angle, so a cube's foot averages about a fifth.
 * */
static const float OCCLUSION_GAIN = 5.0;

float4 main(FragInput input) : SV_Target {
  // the full resolution texel under this half resolution pixel.
  float2 pixel = floor(scene_uv(input) / view.texel) + 0.5;

  float4 centre = scene_texel(normals, normals_sampler, view, pixel, float2(0.0, 0.0));

  if (centre.a <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);

  float3 normal = normalize(centre.rgb);
  float3 position = scene_position(view, pixel * view.texel, centre.a);

  // never under min_radius pixels across, so a distant scene still gathers enough of what stands on it to band.
  float pixel_size = scene_pixel_size(view, centre.a);
  float world_radius = max(radius, min_radius * pixel_size);

  float reach = clamp(world_radius / pixel_size, OCCLUSION_REACH_MIN, OCCLUSION_REACH_MAX);

  // a rotation from a 4x4 ordered pattern, so the apply pass's 4x4 blur sees every rotation once and cancels it.
  const float pattern[16] = { 0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0 };

  uint2 cell = (uint2)input.position.xy % 4;
  float noise = pattern[cell.y * 4 + cell.x] / 16.0;

  float occlusion = 0.0;

  [unroll]
  for (int i = 0; i < OCCLUSION_SAMPLES; i++) {
    float angle = ((float)i * OCCLUSION_TURN) + (noise * 6.28318);
    float spread = sqrt(((float)i + 0.5) / (float)OCCLUSION_SAMPLES) * reach;

    // whole pixels, so every tap reads one texel's distance rather than a blend across an edge.
    float2 offset = round(float2(cos(angle), sin(angle)) * spread);

    float4 tap = scene_texel(normals, normals_sampler, view, pixel, offset);

    if (tap.a <= 0.0) continue;

    float3 toward = scene_position(view, (pixel + offset) * view.texel, tap.a) - position;
    float length_toward = length(toward);

    // facing the surface, lifted slightly so a flat floor does not occlude itself, and gone past the radius.
    float facing = saturate(dot(normal, toward / max(length_toward, 1e-4)) - 0.15);

    float near = saturate(length_toward / world_radius);

    occlusion += facing * (1.0 - (near * near));
  }

  return float4(saturate((occlusion / (float)OCCLUSION_SAMPLES) * OCCLUSION_GAIN), 0.0, 0.0, 1.0);
}
