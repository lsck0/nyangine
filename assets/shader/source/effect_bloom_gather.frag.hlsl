// The bright parts of the image, blurred, at half resolution. effect_bloom.frag.hlsl adds them back. See NYA_PostBloom.

struct FragInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

// Matches NYA_ShaderBloomUniform.
cbuffer BloomUniform : register(b0, space3) {
  float2 spread;
  float threshold;
  float intensity;
};

float3 bright_part(float2 uv) {
  float3 colour = source.SampleLevel(source_sampler, uv, 0.0).rgb;

  // Rec. 601 luma rather than a flat average: the eye weights green far above blue, and averaging makes a
  // saturated blue glow as strongly as a much brighter green.
  float luma = dot(colour, float3(0.299, 0.587, 0.114));

  // smooth rather than a hard cut, or the glow's edge becomes a contour wherever the image crosses the threshold.
  return colour * smoothstep(threshold, threshold + 0.25, luma);
}

float4 main(FragInput input) : SV_Target {
  float2 uv = float2(input.uv.x, 1.0 - input.uv.y);

  float3 glow = 0.0;

  // four by four taps, each read with the linear filter, so the box covers the spread without gaps between taps.
  [unroll]
  for (int y = 0; y < 4; y++) {
    [unroll]
    for (int x = 0; x < 4; x++) {
      glow += bright_part(uv + ((float2((float)x, (float)y) - 1.5) * spread));
    }
  }

  return float4(glow / 16.0, 1.0);
}
