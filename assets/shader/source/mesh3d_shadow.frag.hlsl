// Fragment stage for the shadow pass. Writes light-space depth into a single channel float target.
//
// A colour target rather than sampling the depth buffer directly. Sampling a depth texture needs
// SDL_GPU_TEXTUREUSAGE_SAMPLER on a depth format, which is supported unevenly across backends and
// interacts badly with the multisampling every other render target here uses. An R32_FLOAT colour target
// with its own plain depth buffer for the test uses only paths this renderer already relies on, and costs
// one texture.

struct FragInput {
  float4 position : SV_POSITION;
  float depth : TEXCOORD0;
};

float4 main(FragInput input) : SV_Target {
  /*
   * Depth in the red channel and nothing in the others.
   *
   * The target is R32_FLOAT, so green, blue and alpha are not stored and what is written to them is
   * discarded. Writing zero rather than leaving them undefined keeps the shader honest if the format is
   * ever widened.
   */
  return float4(input.depth, 0.0, 0.0, 1.0);
}
