// Fragment stage for the 2D shape batch. See src/nyangine/renderer/render2d.h.
//
// Untextured on purpose: this is the shapes half of the renderer, and it emits the interpolated
// vertex colour and nothing else. Textured quads and text get their own pipeline with a sampler
// rather than a branch here, because a sampler that some draws do not use still costs a binding on
// every one of them.
//
// uv is carried through the vertex stage but unused here, so that this and a future textured
// fragment shader share one vertex layout and one batch.

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

float4 main(FragInput input) : SV_TARGET {
  // Straight alpha, not premultiplied: the blend state the pipeline sets is SRC_ALPHA /
  // ONE_MINUS_SRC_ALPHA, which expects colour that has not already been multiplied down.
  return input.color;
}
