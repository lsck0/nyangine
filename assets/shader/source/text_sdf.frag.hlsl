// Fragment stage for text drawn from a signed distance field. See nya_font_sdf_set in
// src/nyangine/renderer/render_font.h, which is what puts a field in the atlas for this to read.
//
// The other half of the SDF path, and the half that was missing: turning the mode on made FreeType
// rasterise a distance field into the glyph atlas, and the ordinary textured pipeline then drew that
// field as if it were a picture — a grey blur with the glyph's shape only faintly in it. This
// thresholds it back into a shape.
//
// Registers match the textured pipeline it stands in for, because it is fed by the same vertex stage
// and the same batch: batch2d.vert.hlsl at space0, one sampler at t0/s0 in space2.

Texture2D glyph_field : register(t0, space2);
SamplerState glyph_sampler : register(s0, space2);

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

/*
 * Where the outline is in the field's value range.
 *
 * FreeType's SDF renderer writes the distance rescaled into an unsigned byte with the outline at the
 * middle of the range, so a half is "exactly on the edge", below it is outside and above it is in.
 */
static const float TEXT_SDF_EDGE = 0.5;

/**
 * The lower bound on how wide the threshold is smoothed, in field units.
 *
 * `fwidth` gives the right width wherever the field is being magnified or minified — that is the whole
 * point of a distance field, and the reason this is not a fixed constant like the SDL_ttf example's
 * one sixteenth. But it goes to nearly zero when a glyph is drawn at close to the size it was
 * rasterised at, and a zero-width smoothstep is a hard `step`: every edge aliases, which is worse than
 * the blur this path exists to remove. A floor of about a quarter of a field unit keeps one pixel of
 * anti-aliasing at 1:1 and gets out of the way as soon as the derivative exceeds it.
 * */
static const float TEXT_SDF_MIN_SMOOTHING = 0.0625;

float4 main(FragInput input) : SV_Target {
  /*
   * Red, not alpha.
   *
   * The atlas is an R8G8B8A8 surface that the glyph blit fills with the coverage — or here the field —
   * in every channel, so any one of them carries it. Red is the channel the textured pipeline's
   * multiply already reads as luminance, which keeps the two shaders describing the same texture the
   * same way. (The SDL_ttf example reads alpha because its own atlas is built differently.)
   */
  float distance = glyph_field.Sample(glyph_sampler, input.uv).r;

  // Half the screen-space change per pixel, which is the distance from the centre of a pixel to its
  // edge — the width over which the edge should fade rather than the width of the whole pixel.
  float smoothing = max(fwidth(distance) * 0.5, TEXT_SDF_MIN_SMOOTHING);

  float alpha = smoothstep(TEXT_SDF_EDGE - smoothing, TEXT_SDF_EDGE + smoothing, distance);

  // The vertex colour tints and fades the glyph, exactly as it does for a coverage atlas. Only the
  // alpha comes from the field: a distance field carries a shape, not a colour.
  return float4(input.color.rgb, input.color.a * alpha);
}
