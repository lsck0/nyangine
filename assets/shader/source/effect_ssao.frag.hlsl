// Classic screen-space ambient occlusion at half resolution, raw: how much a hemisphere of nearby geometry closes in
// around this surface, in red. See NYA_PostSsao. A ring of samples is lifted into the surface's hemisphere, each one
// projected back to the screen and tested against the distance the normal buffer stored there; the ones that fall
// behind standing geometry count as occluders. effect_ssao_blur.frag.hlsl denoises and darkens by the result.
//
// This is the textbook hemisphere-kernel SSAO, distinct from effect_occlusion's stylised spiral: it reconstructs a
// world position per sample and compares true eye distances, so a crevice darkens by how much of its hemisphere is
// filled rather than by how its neighbours face it.

#include "effect_scene.hlsli"

Texture2D normals : register(t0, space2);
SamplerState normals_sampler : register(s0, space2);

// Matches NYA_ShaderSsaoUniform.
cbuffer SsaoUniform : register(b0, space3) {
  SceneView view;

  float radius;
  float bias;
  float strength;
  float samples;
};

/** The most samples the loop will ever take, whatever the uniform asks; the uniform is clamped to this on the C side. */
static const int SSAO_MAX_SAMPLES = 32;

/** The golden angle, so the spiral of samples never lines up with itself. */
static const float SSAO_TURN = 2.39996;

float4 main(FragInput input) : SV_Target {
  // the full resolution texel under this half resolution pixel.
  float2 pixel = floor(scene_uv(input) / view.texel) + 0.5;

  float4 centre = scene_texel(normals, normals_sampler, view, pixel, float2(0.0, 0.0));

  // the sky, or anything the 3D pass left blank, has no surface to occlude.
  if (centre.a <= 0.0) return float4(0.0, 0.0, 0.0, 1.0);

  float3 normal = normalize(centre.rgb);
  float centre_distance = centre.a;
  float3 position = scene_position(view, pixel * view.texel, centre_distance);

  // a rotation from a 4x4 ordered pattern, so the blur's 4x4 box sees every rotation once and averages the banding out.
  const float pattern[16] = { 0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0 };

  uint2 cell   = (uint2)input.position.xy % 4;
  float angle  = (pattern[cell.y * 4 + cell.x] / 16.0) * 6.28318;
  float2 turn  = float2(cos(angle), sin(angle));

  // a tangent basis on the surface, spun by the per-pixel rotation. The reference axis avoids the pole so the cross
  // never collapses, and the rotation lives in the tangent plane, which is what decorrelates neighbouring pixels.
  float3 reference = abs(normal.z) < 0.99 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
  float3 tangent_0 = normalize(cross(reference, normal));
  float3 bitangent_0 = cross(normal, tangent_0);

  float3 tangent   = (tangent_0 * turn.x) + (bitangent_0 * turn.y);
  float3 bitangent = cross(normal, tangent);

  int count = clamp((int)samples, 1, SSAO_MAX_SAMPLES);

  float occlusion = 0.0;

  [loop]
  for (int i = 0; i < count; i++) {
    // a Fibonacci hemisphere: the golden angle spirals the ring, k lifts it from the surface toward the normal, and
    // the squared scale clusters the samples near the centre where contact shading matters most.
    float k      = ((float)i + 0.5) / (float)count;
    float phi    = (float)i * SSAO_TURN;
    float radial = sqrt(k);

    float3 hemisphere = float3(cos(phi) * radial, sin(phi) * radial, sqrt(1.0 - k));
    float  scale      = lerp(0.1, 1.0, k * k);

    float3 offset = ((tangent * hemisphere.x) + (bitangent * hemisphere.y) + (normal * hemisphere.z)) * (radius * scale);
    float3 sample_position = position + offset;

    // where that world point lands on screen, inverting scene_position for the perspective and orthographic cases.
    float3 toward       = sample_position - view.eye;
    float  sample_distance = length(toward);

    float2 ndc;

    if (view.half_height > 0.0) {
      ndc.x = dot(toward, view.right) / (view.half_height * view.aspect);
      ndc.y = dot(toward, view.up) / view.half_height;
    } else {
      float along = dot(toward, view.forward);

      // behind the eye: nothing to read.
      if (along <= 1e-4) continue;

      ndc.x = (dot(toward, view.right) / along) / (view.tangent * view.aspect);
      ndc.y = (dot(toward, view.up) / along) / view.tangent;
    }

    float2 uv = float2((ndc.x * 0.5) + 0.5, 0.5 - (ndc.y * 0.5));

    float occluder = normals.SampleLevel(normals_sampler, uv, 0.0).a;

    // the sky occludes nothing, and a far occluder fades so a distant wall does not darken a near surface through it.
    if (occluder <= 0.0) continue;

    float range = smoothstep(0.0, 1.0, radius / max(abs(centre_distance - occluder), 1e-4));

    // the stored surface sits in front of the lifted sample by more than the bias: the sample is buried.
    occlusion += (occluder <= sample_distance - bias ? 1.0 : 0.0) * range;
  }

  return float4(saturate(occlusion / (float)count), 0.0, 0.0, 1.0);
}
