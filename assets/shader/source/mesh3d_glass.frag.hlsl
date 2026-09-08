// Fragment stage for refractive glass. See NYA_Render3DMaterial.refraction.
//
// The other two mesh fragment shaders decide where the *base colour* comes from and share everything
// after that. This one differs later: it takes its base colour from a capture of the opaque scene, offset
// and blurred, and then shades that. What it produces is not a translucent surface blended over the
// target — it is an opaque surface whose colour happens to be a distorted view of what is behind it.
//
// ## Why that distinction matters
//
// A blended glass surface samples the destination and then blends back over the destination, which counts
// what is behind it twice: once in the sample and once in the blend. So this writes an alpha of one and
// the pipeline does not blend at all. The "transparency" is entirely in what the colour is *made of*.
//
// The cost is that glass behind glass does not compound — the second pane samples a capture taken before
// either was drawn, so it shows the first one's *unrefracted* backdrop. Fixing that means re-capturing
// between panes, which is a resolve per pane.

#include "mesh3d_shading.hlsli"

/*
 * The captured opaque scene at t0, the shadow map at t1.
 *
 * Two samplers, like the textured pipeline — and in the same order for the same reason: the shadow map is
 * declared last so mesh3d_shadow can be handed whichever register the pipeline put it at, rather than
 * reading a global that would have to sit at a different one in each shader.
 */
Texture2D scene : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

Texture2D shadow_map : register(t1, space2);
SamplerState shadow_sampler : register(s1, space2);

// A second fragment block, at b1 — the shading uniform is at b0. Its own block because the shading one is
// shared with two other pipelines that have no capture to sample and no business carrying its size.
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
 * The scene behind this fragment, offset by the surface normal and blurred.
 *
 * The offset uses the normal's *screen-space* x and y, which is what makes a face pointing at the camera
 * refract almost nothing and one turned away refract most — the same behaviour a real refractive surface
 * has, arrived at for a different reason. It is an approximation of Snell's law and not a derivation of it.
 *
 * Thirteen taps in a plus-and-diagonal arrangement rather than a full square: a 5x5 box is twenty-five
 * samples for a result barely distinguishable at these radii, and the cost is samples.
 * */
float3 refracted_scene(float2 screen_uv, float3 normal) {
  float2 offset = normal.xy * refraction * GLASS_MAX_OFFSET;

  float radius = blur * GLASS_MAX_BLUR;

  /*
   * Clamped to the target, so a fragment near the edge does not sample past it.
   *
   * The sampler's addressing mode is the renderer's, shared with every other texture, so it cannot be
   * relied on to clamp — and a wrapped sample at the screen edge puts the opposite side of the scene
   * inside the glass, which is the most obvious artefact this shader can produce.
   */
  float2 base = clamp(screen_uv + offset, scene_texel, 1.0 - scene_texel);

  if (radius <= 0.0) return scene.Sample(scene_sampler, base).rgb;

  float3 sum = 0.0;

  // Centre, axis and diagonal rings. The diagonals are pulled in by root two so every tap sits at roughly
  // the same distance, which is what stops the kernel showing as a plus sign at high blur.
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

float4 main(FragInput input) : SV_Target {
  float3 normal = normalize(input.normal);

  // SV_POSITION holds window coordinates in the fragment stage, so this is where on the target the
  // fragment landed — which is exactly the uv the capture has to be read at.
  float2 screen_uv = input.position.xy * scene_texel;

  float3 behind = refracted_scene(screen_uv, normal);

  float shadow = mesh3d_shadow(shadow_map, shadow_sampler, input.world_position, normal);

  /*
   * The glass tints what passes through it, by its own colour and by how opaque it is.
   *
   * `input.color.a` is doing the work it would have done as a blend factor, moved inside: at zero the
   * scene passes through untouched, and at one it is fully replaced by the glass colour. That is the same
   * curve alpha blending would have produced, computed here so the pipeline can write opaque.
   */
  float3 tinted = lerp(behind, behind * input.color.rgb, input.color.a);

  /*
   * The surface's own shading, added rather than blended.
   *
   * A pane of glass is mostly what is behind it plus a highlight and a rim — which is exactly what the
   * cel model's specular and rim terms are. Taking the *difference* between the shaded and unshaded
   * surface isolates those additive terms and leaves out the diffuse, which for glass would be a flat
   * wash of colour over the view rather than a reflection.
   */
  float3 lit = mesh3d_shade(input.color.rgb, normal, input.world_position, shadow);
  float3 flat_surface = input.color.rgb * ambient;

  float3 colour = tinted + max(lit - flat_surface, 0.0);

  // Opaque, and tonemapped like every other surface. See the note at the top for why the alpha is one
  // rather than the glass's own.
  return float4(mesh3d_tonemap(colour), 1.0);
}
