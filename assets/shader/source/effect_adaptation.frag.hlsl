// Eye adaptation, second half: the image exposed toward the key by the brightness the eye is used to. See
// NYA_PostEyeAdaptation.

#include "effect_adaptation.hlsli"

float4 main(FragInput input) : SV_Target {
  float4 colour = source.SampleLevel(source_sampler, float2(input.uv.x, 1.0 - input.uv.y), 0.0);

  float used_to = pow(2.0, adapted.SampleLevel(adapted_sampler, float2(0.5, 0.5), 0.0).r);
  float exposure = clamp(key / used_to, exposure_min, exposure_max);

  // the mid tones move and white stays white, so the flat palette is lifted or deepened without washing out. above
  // one, emission keeps its own brightness for the bloom.
  float luma = dot(colour.rgb, LUMA);
  float exposed = luma * exposure / (1.0 + (exposure - 1.0) * saturate(luma));

  float3 result = colour.rgb * (exposed / max(luma, 1e-4));

  // opened up for the dark, colour fades as it does at dusk; closed down in the light it deepens.
  float amount = 1.0 - saturation * clamp(log2(exposure), -1.0, 1.0);

  return float4(max(lerp(float3(exposed, exposed, exposed), result, amount), 0.0), colour.a);
}
