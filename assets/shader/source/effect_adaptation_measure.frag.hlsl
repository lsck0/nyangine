// Eye adaptation, first half: the image's average log brightness, eased from last frame's toward it, into a one texel
// target. effect_adaptation.frag.hlsl exposes the image by it. See NYA_PostEyeAdaptation.

#include "effect_adaptation.hlsli"

static const int GRID = 8;

float4 main(FragInput input) : SV_Target {
  float sum = 0.0;
  float weight = 0.0;

  [unroll]
  for (int y = 0; y < GRID; y++) {
    [unroll]
    for (int x = 0; x < GRID; x++) {
      float4 colour = source.SampleLevel(source_sampler, (float2(x, y) + jitter) / GRID, 0.0);

      // a 2D world is captured over a transparent clear, so a texel counts by its coverage and an empty one not at all.
      float luma = dot(colour.rgb, LUMA) / max(colour.a, 1e-3);

      sum += log2(max(luma, 1e-3)) * colour.a;
      weight += colour.a;
    }
  }

  // a fresh history holds nothing to ease from.
  bool reset = rate_dark >= 1.0;

  float previous = reset ? log2(key) : adapted.SampleLevel(adapted_sampler, float2(0.5, 0.5), 0.0).r;

  if (weight < 1e-3) return float4(previous, 0.0, 0.0, 1.0);

  float measured = sum / weight;

  return float4(lerp(previous, measured, measured < previous ? rate_dark : rate_bright), 0.0, 0.0, 1.0);
}
