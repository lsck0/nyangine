// Quantizes the sample position onto a grid, so a smooth image reads as chunky pixels.
//
// Snapping the *coordinate* rather than downscaling and scaling back: the latter needs two render
// textures and two passes, this needs neither and produces the same blocks. The sampler still
// filters within a block, which is why the texture wants NYA_TEXTURE_FILTER_NEAREST for a hard look.

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer PixelateUniform : register(b0, space3) {
  // Blocks across and down the sampled region. Larger is finer; 1 would be a single flat block.
  float2 blocks;
};

float4 main(FragInput input) : SV_TARGET {
  // Guarded: a zeroed uniform would divide by zero and sample garbage across the whole quad.
  float2 grid = max(blocks, float2(1.0, 1.0));

  // The centre of the block, not its corner — sampling the corner reads the seam between two blocks
  // and picks up whichever neighbour the filter reaches.
  float2 snapped = (floor(input.uv * grid) + 0.5) / grid;

  return source.Sample(source_sampler, snapped) * input.color;
}
