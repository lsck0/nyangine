// Screen-space reflections: each surface reflects the scene by marching its reflection ray through the normal buffer
// and reading the colour where the ray first crosses behind stored geometry. A ray that leaves the screen, or finds
// nothing, falls back to a fresnel-weighted sky/ground tint, so a grazing edge gains a soft environment reflection
// rather than turning black. Composited over the scene by the fresnel term. See NYA_PostSsr.
//
// World-space marching, not a depth-buffer walk: the normal buffer carries eye distance in its alpha and a world
// normal in its rgb (NYA_RENDER3D_NORMAL_FORMAT), the same buffer effect_ssao reads, so a step is a world point
// projected back to the screen and tested against the distance stored there, inverting scene_position for the
// perspective and orthographic cases exactly as the ssao gather does. Kept to SampleLevel and one bounded loop so
// SPIRV-Cross lowers it straight to GLSL ES 300 for the web backend, with no textureGather to refuse.

#include "effect_scene.hlsli"

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

Texture2D normals : register(t1, space2);
SamplerState normals_sampler : register(s1, space2);

// Matches NYA_ShaderSsrUniform.
cbuffer SsrUniform : register(b0, space3) {
  SceneView view;

  float max_distance;
  float thickness;
  float strength;
  float fresnel;

  float steps;
  float3 ssr_pad;

  // the ambient environment a missed ray reflects, sky above the horizon and ground below.
  float3 sky;
  float sky_pad;

  float3 ground;
  float ground_pad;
};

/** The most steps the march ever takes, whatever the uniform asks; the uniform is clamped to this on the C side. */
static const int SSR_MAX_STEPS = 64;

/** How far into the screen a hit has to sit before its colour is trusted; nearer the border it fades to the tint. */
static const float SSR_EDGE = 0.12;

float4 main(FragInput input) : SV_Target {
  float2 uv = scene_uv(input);

  float4 colour = source.SampleLevel(source_sampler, uv, 0.0);
  float4 centre = normals.SampleLevel(normals_sampler, uv, 0.0);

  // the sky, or anything the 3D pass left blank, reflects nothing.
  if (centre.a <= 0.0) return colour;

  float3 normal   = normalize(centre.rgb);
  float  distance = centre.a;
  float3 position = scene_position(view, uv, distance);

  // from the eye to the surface, then mirrored about the normal: the way the surface throws a reflection.
  float3 incident   = normalize(position - view.eye);
  float3 reflection = reflect(incident, normal);

  // schlick from the reflectivity head-on: a grazing angle reflects nearly all, straight down nearly none, which is
  // what turns a flat floor into a mirror only toward the horizon.
  float head_on      = saturate(dot(normal, -incident));
  float reflectivity = saturate((fresnel + (1.0 - fresnel) * pow(1.0 - head_on, 5.0)) * strength);

  // blended by how far up the reflection points, so a ray toward the sky reads sky and one toward the floor reads ground.
  float3 environment = lerp(ground, sky, saturate((reflection.y * 0.5) + 0.5));

  int   count = clamp((int)steps, 1, SSR_MAX_STEPS);
  float span  = max_distance / (float)count;

  // lift the walk off the surface so the first steps do not read this pixel's own texel as an occluder.
  float3 walk = position + (normal * (distance * 0.01));

  float3 reflected = environment;

  [loop]
  for (int i = 1; i <= count; i++) {
    float3 sample_position = walk + (reflection * (span * (float)i));

    // where that world point lands on screen, and how far it is from the eye there.
    float3 toward          = sample_position - view.eye;
    float  sample_distance = length(toward);

    float2 ndc;

    if (view.half_height > 0.0) {
      ndc.x = dot(toward, view.right) / (view.half_height * view.aspect);
      ndc.y = dot(toward, view.up) / view.half_height;
    } else {
      float along = dot(toward, view.forward);

      // behind the eye: the ray has left what the buffer knows.
      if (along <= 1e-4) break;

      ndc.x = (dot(toward, view.right) / along) / (view.tangent * view.aspect);
      ndc.y = (dot(toward, view.up) / along) / view.tangent;
    }

    float2 sample_uv = float2((ndc.x * 0.5) + 0.5, 0.5 - (ndc.y * 0.5));

    // off the screen: nothing more to read along this ray.
    if (sample_uv.x < 0.0 || sample_uv.x > 1.0 || sample_uv.y < 0.0 || sample_uv.y > 1.0) break;

    float stored = normals.SampleLevel(normals_sampler, sample_uv, 0.0).a;

    // the sky stores no distance; the ray passes in front of it and keeps going.
    if (stored <= 0.0) continue;

    // the marched point sits behind stored geometry by less than the thickness: the ray struck that surface.
    float behind = sample_distance - stored;

    if (behind > 0.0 && behind < thickness) {
      // fade the hit toward the environment as it nears the border, where the reflected colour runs off the screen.
      float2 fade = smoothstep(0.0, SSR_EDGE, sample_uv) * smoothstep(0.0, SSR_EDGE, 1.0 - sample_uv);

      reflected = lerp(environment, source.SampleLevel(source_sampler, sample_uv, 0.0).rgb, fade.x * fade.y);
      break;
    }
  }

  colour.rgb = lerp(colour.rgb, reflected, reflectivity);

  return colour;
}
