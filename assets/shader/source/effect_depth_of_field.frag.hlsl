// Depth of field, second half: the sharp scene where it is in focus, the half resolution blur elsewhere. See
// NYA_PostDepthOfField.
//
// How blurred a pixel is gets decided again here at full resolution, so an object in focus keeps a clean silhouette
// against the blur behind it instead of the half resolution staircase.

#include "effect_depth_of_field.hlsli"

Texture2D scene : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

// the scene normal buffer. tilt shift binds the image here and never reads it.
Texture2D normals : register(t1, space2);
SamplerState normals_sampler : register(s1, space2);

Texture2D blurred : register(t2, space2);
SamplerState blurred_sampler : register(s2, space2);

float4 main(FragInput input) : SV_Target {
  float2 uv = float2(input.uv.x, 1.0 - input.uv.y);

  float4 sharp = scene.SampleLevel(scene_sampler, uv, 0.0);

  // any layer of blur switches fully, so the step between sharp and blurred is a cut.
  float mix = saturate(depth_of_field_amount(normals, normals_sampler, uv) * layers);

  if (mix <= 0.0) return sharp;

  // the scene's own alpha, since the chain composites over whatever is under it.
  return float4(lerp(sharp.rgb, blurred.SampleLevel(blurred_sampler, uv, 0.0).rgb, mix), sharp.a);
}
