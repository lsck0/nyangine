// Fragment stage for text drawn from a coverage atlas. See NYA_FontAtlas in
// src/nyangine/renderer/render2d.c, which is what bakes the coverage this reads.
//
// Its own pipeline rather than the textured one, because the atlas is a single channel. The textured
// shader multiplies all four, so an R8 atlas would reach it as (coverage, 0, 0, 1) and draw every glyph
// as a red box. text_sdf.frag.hlsl is the same shape with a threshold on top; this one has no curve to
// apply, the ramp SDL_ttf rasterised already being the answer.
//
// Registers match the textured pipeline it stands in for, because it is fed by the same vertex stage
// and the same batch: batch2d.vert.hlsl at space0, one sampler at t0/s0 in space2.

Texture2D glyph_coverage : register(t0, space2);
SamplerState glyph_sampler : register(s0, space2);

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

float4 main(FragInput input) : SV_Target {
  float coverage = glyph_coverage.Sample(glyph_sampler, input.uv).r;

  // The vertex colour carries the whole of the glyph's colour, the atlas only how much of the texel the
  // ink covers. Multiplied into alpha rather than into rgb, so a half-covered texel is half transparent
  // instead of half black — the latter draws dark fringes wherever text sits on a light background.
  return float4(input.color.rgb, input.color.a * coverage);
}
