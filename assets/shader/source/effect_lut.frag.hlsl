// Colour grading through a 3D lookup table: each pixel's colour is looked up in a `.cube` table and blended back in
// by `strength`. See NYA_ASSET_TYPE_LUT and NYA_ShaderLutUniform.
//
// A 3D texture rather than a 2D strip: every SDL GPU backend samples RGBA8 3D textures, and the hardware then
// interpolates all three channels in one fetch where a strip needs two fetches and a manual blend.

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

Texture3D<float4> lut : register(t1, space2);
SamplerState lut_sampler : register(s1, space2);

cbuffer LutUniform : register(b0, space3) {
  // How much of the graded colour replaces the original. One is the table as authored.
  float strength;
  float3 pad;
};

float4 main(FragInput input) : SV_TARGET {
  float4 original = source.Sample(source_sampler, input.uv);

  uint width, height, depth;
  lut.GetDimensions(width, height, depth);

  // entries sit at texel centres, so zero and one map to the first and last centre rather than the texture's edges.
  float size = (float)width;
  float3 coordinate = saturate(original.rgb) * ((size - 1.0) / size) + (0.5 / size);

  float3 graded = lut.Sample(lut_sampler, coordinate).rgb;

  return float4(lerp(original.rgb, graded, strength), original.a) * input.color;
}
