// Fragment stage for textured draws: sprites, render textures, and eventually glyph atlases.
//
// Shares the vertex stage and the vertex layout with the untextured shape shader, so both pipelines
// draw out of the same batch and the same buffer. The only difference is that this one has a
// sampler bound and multiplies by what it reads.

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

float4 main(FragInput input) : SV_TARGET {
  // Vertex colour is a tint, not a replacement: white leaves the texture untouched, and anything
  // else multiplies through — which is what lets one white sprite be drawn in any colour, and what
  // makes a fade a colour change rather than a second pipeline.
  //
  // Alpha multiplies too, so a tint alpha of 0.5 draws the texture at half opacity. Straight alpha,
  // matching the SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend state the pipeline sets.
  return source.Sample(source_sampler, input.uv) * input.color;
}
