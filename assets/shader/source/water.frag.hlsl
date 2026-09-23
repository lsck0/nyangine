// Fragment stage for a flowing water surface. See nya_render3d_water in src/nyangine/renderer/render3d.h.
//
// The look is built from four things:
//
//  - Flowing ripples. Two procedural ripple-normal fields are scrolled along the current half a cycle out of
//    step and blended on a triangular weight (nya_water_flow), so the scroll never visibly jumps at the wrap.
//    This is the classic dual flow-map trick; the ripples are synthesised from summed sines rather than a
//    normal-map texture so the shader stays texture-free and ESSL-300 safe. Dropping in an authored water
//    normal map later would sample it at the same two offsets. They perturb the vertex stage's wave normal.
//
//  - A deep-to-shallow body colour, driven by the shore weight the mesh authored in vertex-colour alpha, and,
//    where the scene was drawn into a render texture, the refraction of what is behind — the same capture the
//    glass material reads (NYA_Render3DMaterial.refraction). Drawn straight to the window there is no mid-frame
//    capture, so the body falls back to the depth-blended colour, exactly as glass falls back to plain blending.
//
//  - A Fresnel blend toward a flat reflection tint at grazing angles. A cheap stand-in for a real reflection;
//    planar reflection / SSR is roadmap line 1104. TODO: replace the constant tint with a sampled reflection.
//
//  - Foam: a soft band where the surface meets the banks (the shore weight approaches one — a stand-in for a
//    true depth-difference shoreline, which needs a depth capture the colour capture does not carry) plus a
//    little along the wave crests. TODO: capture scene depth alongside colour for a real shoreline band.
//
// ## ESSL-300 safe
//
// mesh3d_shade/_fog/_tonemap are reused for the lighting, but mesh3d_shadow is NOT called: it is the only
// thing in mesh3d_shading.hlsli that uses Gather, which GLSL ES 300 has no equivalent for, so leaving it
// unreferenced lets the compiler strip it and the shader cross-compiles. Water receives no cast shadow as a
// result, which reads fine on a bright reflective surface. Keep this free of Gather and compute.

#include "mesh3d_shading.hlsli"
#include "mesh3d_normals.hlsli"

/* The captured opaque scene at t0. Water declares no shadow sampler, since it does not read the shadow map. */
Texture2D scene : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

// a second fragment block at b1, so the lit pipelines without water state do not carry its size. Matches
// NYA_ShaderWaterFragUniform in uniforms.h.
cbuffer WaterUniform : register(b1, space3) {
  // scene_texel.xy (one capture texel in uv), refraction (0..1), has_refraction (1 when the capture is live).
  float4 scene_params;

  // the deep water colour; alpha is how murky the deep body is (how much it hides the refraction).
  float4 deep;

  // the shallow water colour; alpha is the shallow body's murk, usually lower so banks read clearer.
  float4 shallow;

  // shore foam band width (0..1 of the shore weight), crest foam threshold, foam softness, surface opacity.
  float4 foam;

  // Fresnel exponent, then the reflection tint rgb the surface leans toward at grazing angles.
  float4 look;

  // flow_dir.x, flow_dir.z, flow cycle seconds (the ripple wrap), time.
  float4 flow_time;

  // ripple spatial scale, ripple normal strength, ripple travel speed, pad.
  float4 ripple;
};

struct FragInput {
  float4 position : SV_POSITION;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
  float3 world_position : TEXCOORD1;
  float2 shore_crest : TEXCOORD2;
};

/** How far the refraction lookup is displaced at full refraction, in uv. Small, or the image tears. */
static const float WATER_MAX_OFFSET = 0.05;

/** White foam, lifted a little past one so a bloom threshold catches sunlit surf. */
static const float3 WATER_FOAM_COLOUR = float3(1.05, 1.08, 1.10);

/**
 * The slope of a small procedural ripple field at a point, in the ground plane. Four directional sines summed;
 * the returned float2 is the horizontal gradient, which tilts the surface normal. A stand-in for sampling a
 * water normal map, kept analytic so there is no texture and no Gather.
 * */
float2 ripple_gradient(float2 p) {
  // four directions that are not axis aligned and not simple multiples of each other, so the sum does not
  // read as a grid or beat as one wave.
  const float2 d0 = float2(0.952, 0.306);
  const float2 d1 = float2(-0.447, 0.894);
  const float2 d2 = float2(0.707, -0.707);
  const float2 d3 = float2(-0.174, -0.985);

  const float4 k = float4(1.0, 1.7, 2.3, 3.1);   // per-direction wavenumbers
  const float4 a = float4(0.5, 0.3, 0.13, 0.07); // amplitudes, falling with wavenumber

  float2 g = 0.0;
  g += d0 * (cos(dot(p, d0) * k.x) * a.x * k.x);
  g += d1 * (cos(dot(p, d1) * k.y) * a.y * k.y);
  g += d2 * (cos(dot(p, d2) * k.z) * a.z * k.z);
  g += d3 * (cos(dot(p, d3) * k.w) * a.w * k.w);

  return g;
}

/**
 * The scene behind this fragment, offset along the surface normal's screen projection — the same refraction
 * the glass material does. Clamped to the target so a wrapped sampler cannot pull in the opposite edge.
 * */
float3 refracted_scene(float2 screen_uv, float3 normal) {
  float2 offset = normal.xy * scene_params.z * WATER_MAX_OFFSET;
  float2 at = clamp(screen_uv + offset, scene_params.xy, 1.0 - scene_params.xy);

  return scene.Sample(scene_sampler, at).rgb;
}

Mesh3DOutput main(FragInput input) {
  float shore = saturate(input.shore_crest.x);
  float crest = input.shore_crest.y;

  float time  = flow_time.w;
  float cycle = flow_time.z > 0.0 ? flow_time.z : 1.0;

  // the current's heading, for scrolling the ripple field along the flow.
  float2 flow = flow_time.xy;
  if (dot(flow, flow) < 1e-8) flow = float2(1.0, 0.0);
  flow = normalize(flow);

  // the two flow-map phases and the triangular blend, identical to nya_water_flow. The ripple field is
  // scrolled by each phase, so a layer's wrap discontinuity is hidden behind the other layer's weight.
  float t = time / cycle;
  float phase_a = frac(t);
  float phase_b = frac(t + 0.5);
  float blend   = abs(1.0 - (2.0 * phase_a));

  float2 p = input.world_position.xz * ripple.x;
  float2 scroll = flow * ripple.z;

  // two scrolled ripple gradients, blended: the flowing micro-detail on top of the vertex stage's waves.
  float2 grad_a = ripple_gradient(p - (scroll * phase_a));
  float2 grad_b = ripple_gradient(p - (scroll * phase_b));
  float2 grad   = lerp(grad_a, grad_b, blend);

  // perturb the vertex wave normal by the ripple slope, then renormalise.
  float3 normal = normalize(input.normal + float3(-grad.x, 0.0, -grad.y) * ripple.y);

  float3 view = normalize(camera_position - input.world_position);

  // SV_POSITION holds window coordinates here, the uv to read the capture at.
  float2 screen_uv = input.position.xy * scene_params.xy;

  // shallow at the banks (shore -> 1), deep in the channel. Alpha is murk: how much the body hides the scene.
  float shallowness = shore;
  float3 water_colour = lerp(deep.rgb, shallow.rgb, shallowness);
  float murk = lerp(deep.a, shallow.a, shallowness);

  // the view through the water: the refracted scene tinted toward the water colour by murk, or the plain
  // depth-blended colour where there is no capture to refract.
  float3 behind = scene_params.w > 0.5 ? refracted_scene(screen_uv, normal) : water_colour;
  float3 body   = lerp(behind, water_colour, scene_params.w > 0.5 ? murk : 1.0);

  // Fresnel toward the reflection tint at grazing angles: little reflection looking straight down, most at
  // the horizon. A cheap stand-in until planar reflection / SSR lands (roadmap line 1104).
  float fresnel = pow(1.0 - saturate(dot(normal, view)), max(look.x, 0.5));
  float3 colour = lerp(body, look.yzw, fresnel);

  // the surface's own shading, added the way glass adds it: the highlight and rim on top of the composed
  // colour without the diffuse washing over the view. shadow = 1: water receives no cast shadow here.
  float3 lit = mesh3d_shade(colour, normal, input.world_position, 1.0);
  float3 flat_surface = colour * ambient;
  colour = colour + max(lit - flat_surface, 0.0);

  // foam: a soft band at the banks (shore weight near one) plus the wave crests, shimmered by the ripples so
  // the foam line is not a clean arc. `foam.x` is the band width, `foam.y` the crest threshold, `foam.z` its softness.
  float shore_band = smoothstep(1.0 - max(foam.x, 1e-3), 1.0, shore);
  float crest_foam = smoothstep(foam.y, foam.y + max(foam.z, 1e-3), crest);
  float shimmer    = 0.6 + (0.4 * sin((p.x + p.y) * 2.0 + (time * 3.0)));
  float foam_mask  = saturate(max(shore_band, crest_foam) * shimmer);

  colour = lerp(colour, WATER_FOAM_COLOUR, foam_mask);

  // opaque where the refraction already shows what is behind, otherwise the caller's opacity so the fallback
  // blends; foam is always solid so the surf reads over water and bank alike.
  float alpha = lerp(scene_params.w > 0.5 ? 1.0 : saturate(foam.w), 1.0, foam_mask);

  // tonemapped and fogged like every surface. Fogged at the surface's distance, the same slight double-fog
  // glass accepts, since the capture already carries its own fog and there is no backdrop depth to do better.
  return mesh3d_output(float4(mesh3d_fog(mesh3d_tonemap(colour), input.world_position), alpha), normal, input.world_position);
}
