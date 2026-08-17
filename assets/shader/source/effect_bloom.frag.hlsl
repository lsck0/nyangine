// Bright-pass and blur in one, composited over the original: a cheap bloom.
//
// A full bloom is four passes — isolate the bright parts, downsample, blur separably, add back — and
// wants a chain of render textures. This does the isolation and a single blur inline, which costs
// one pass and is enough for glowing text or a UI accent.
//
// Where it differs from the real thing: the blur is fixed radius rather than a downsample pyramid,
// so a very wide glow is expensive here and cheap there. Reach for the four pass version when the
// radius stops being small.

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer BloomUniform : register(b0, space3) {
  // One texel in uv, so the sample offsets are resolution independent.
  float2 texel;

  // Luminance above which a pixel contributes to the glow. Around 0.6 for text on a dark panel.
  float threshold;

  // How strongly the glow is added back. 1.0 is a soft halo; past 2 it blows out.
  float intensity;
};

float3 bright_part(float2 uv) {
  float3 texel_color = source.Sample(source_sampler, uv).rgb;

  // Rec. 601 luma rather than a flat average: the eye weights green far above blue, and averaging
  // makes a saturated blue glow as strongly as a much brighter green.
  float luma = dot(texel_color, float3(0.299, 0.587, 0.114));

  // Smooth rather than a hard cut, or the glow's edge becomes a visible contour wherever the image
  // crosses the threshold.
  return texel_color * smoothstep(threshold, threshold + 0.25, luma);
}

float4 main(FragInput input) : SV_TARGET {
  float4 original = source.Sample(source_sampler, input.uv);

  // A 5x5 box over the bright parts, so twenty five samples. Small on purpose: the cost is samples,
  // and this shader exists to be the cheap option.
  float3 glow = 0.0;

  [unroll]
  for (int x = -2; x <= 2; x++) {
    [unroll]
    for (int y = -2; y <= 2; y++) {
      glow += bright_part(input.uv + (texel * float2((float)x, (float)y)));
    }
  }

  glow /= 25.0;

  float3 lit = original.rgb + (glow * intensity);

  /*
   * The glow contributes alpha as well as colour.
   *
   * A halo spreads *past* the bright thing that cast it, onto pixels the scene left transparent —
   * and with straight alpha blending a pixel of alpha zero is discarded no matter what its rgb says.
   * Carrying the glow's own luminance into alpha is what makes the halo visible over whatever is
   * behind the layer, instead of stopping dead at the silhouette that produced it.
   *
   * Costs nothing where the source is already opaque, which is the full screen case: saturate of
   * one plus anything is still one.
   */
  float glow_alpha = saturate(dot(glow, float3(0.299, 0.587, 0.114)) * intensity);

  // Added, not blended: light accumulates, so a glow over a lit area is brighter than either alone.
  return float4(lit, saturate(original.a + glow_alpha)) * input.color;
}
