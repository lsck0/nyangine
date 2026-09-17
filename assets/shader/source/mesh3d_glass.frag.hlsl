// Fragment stage for refractive glass. See NYA_Render3DMaterial.refraction.
//
// The base colour is a capture of the opaque scene behind the fragment, offset and blurred, then shaded. The
// result is opaque: blending over the target as well would count what is behind the glass twice. Glass behind
// glass sees a capture taken before either pane, so it shows the unrefracted backdrop.

#include "mesh3d_shading.hlsli"
#include "mesh3d_normals.hlsli"

/* The captured opaque scene at t0 and the shadow map at t1, last, so mesh3d_shadow is handed its register. */
Texture2D scene : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

Texture2D shadow_map : register(t1, space2);
SamplerState shadow_sampler : register(s1, space2);

// a second fragment block at b1, so the two pipelines without a capture do not carry its size.
cbuffer GlassUniform : register(b1, space3) {
  // One texel of the capture in uv, so the offsets below are resolution independent.
  float2 scene_texel;

  // See NYA_Render3DMaterial.refraction and .blur.
  float refraction;
  float blur;
};

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
  float3 world_position : TEXCOORD1;
};

/** How far the lookup is displaced at full refraction, in uv. Small: a large one tears the image. */
static const float GLASS_MAX_OFFSET = 0.06;

/** How wide the blur kernel reaches at full blur, in uv. */
static const float GLASS_MAX_BLUR = 0.02;

/**
 * The scene behind this fragment, offset by the normal's screen-space x and y and blurred. A face toward the
 * camera refracts little and one turned away refracts most; an approximation of Snell's law, not a derivation.
 * Thirteen taps in a plus and diagonal pattern instead of a 5x5 box, which looks the same at these radii.
 * */
float3 refracted_scene(float2 screen_uv, float3 normal) {
  float2 offset = normal.xy * refraction * GLASS_MAX_OFFSET;

  float radius = blur * GLASS_MAX_BLUR;

  /*
   * Clamped to the target: the shared sampler may wrap, and a wrapped sample puts the opposite screen edge inside
   * the glass.
   */
  float2 base = clamp(screen_uv + offset, scene_texel, 1.0 - scene_texel);

  if (radius <= 0.0) return scene.Sample(scene_sampler, base).rgb;

  float3 sum = 0.0;

  // the diagonal ring is pulled in by root two so every tap is about the same distance out.
  const float2 taps[13] = {
    float2(0.0, 0.0),
    float2(1.0, 0.0),  float2(-1.0, 0.0),  float2(0.0, 1.0),  float2(0.0, -1.0),
    float2(0.7, 0.7),  float2(-0.7, 0.7),  float2(0.7, -0.7), float2(-0.7, -0.7),
    float2(2.0, 0.0),  float2(-2.0, 0.0),  float2(0.0, 2.0),  float2(0.0, -2.0),
  };

  [unroll]
  for (int i = 0; i < 13; i++) {
    float2 tap = clamp(base + (taps[i] * radius), scene_texel, 1.0 - scene_texel);

    sum += scene.Sample(scene_sampler, tap).rgb;
  }

  return sum / 13.0;
}

Mesh3DOutput main(FragInput input) {
  float3 normal = normalize(input.normal);

  // SV_POSITION holds window coordinates here, which is the uv to read the capture at.
  float2 screen_uv = input.position.xy * scene_texel;

  float3 behind = refracted_scene(screen_uv, normal);

  float shadow = mesh3d_shadow(shadow_map, shadow_sampler, input.world_position, normal);

  /*
   * The glass tints what passes through by its colour and alpha: zero leaves the scene untouched, one replaces it.
   * The same curve blending would give, computed here so the pipeline writes opaque.
   */
  float3 tinted = lerp(behind, behind * input.color.rgb, input.color.a);

  /*
   * The surface's own shading, added: the difference between shaded and unshaded keeps the highlight and rim and
   * drops the diffuse, which would wash colour over the view.
   */
  float3 lit = mesh3d_shade(input.color.rgb, normal, input.world_position, shadow);
  float3 flat_surface = input.color.rgb * ambient;

  float3 colour = tinted + max(lit - flat_surface, 0.0);

  /*
   * Opaque and tonemapped like every surface. Fogged at the pane's distance, which slightly double-fogs the
   * backdrop (the capture already carries its fog); unfogged glass would stand out far more. Doing it properly
   * needs the backdrop's depth.
   */
  return mesh3d_output(float4(mesh3d_fog(mesh3d_tonemap(colour), input.world_position), 1.0), normal, input.world_position);
}
