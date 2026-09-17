// Adds the half resolution glow from effect_bloom_gather.frag.hlsl back over the image. See NYA_PostBloom.

struct FragInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

Texture2D glow_image : register(t1, space2);
SamplerState glow_sampler : register(s1, space2);

// Matches NYA_ShaderBloomUniform.
cbuffer BloomUniform : register(b0, space3) {
  float2 spread;
  float threshold;
  float intensity;
};

float4 main(FragInput input) : SV_Target {
  float2 uv = float2(input.uv.x, 1.0 - input.uv.y);

  float4 original = source.SampleLevel(source_sampler, uv, 0.0);
  float3 glow = glow_image.SampleLevel(glow_sampler, uv, 0.0).rgb * intensity;

  // a halo spreads past what cast it, onto pixels a 2D world left transparent, where alpha zero would hide it.
  float glow_alpha = saturate(dot(glow, float3(0.299, 0.587, 0.114)));

  // added, not blended: light accumulates.
  return float4(original.rgb + glow, saturate(original.a + glow_alpha));
}
