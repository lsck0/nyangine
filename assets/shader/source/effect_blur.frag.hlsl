// A separable gaussian blur, and the worked example of nya_render2d_shader_set_uniform.
//
// Separable means the two dimensional blur is done as two one dimensional passes — across, then
// down. Nine taps each way is eighteen samples total, against eighty-one for the same kernel done in
// one pass, and the result is identical because a gaussian is the product of its axes.
//
// The direction is a uniform rather than two shaders, so the same pipeline runs both passes with a
// different value pushed between them. That is what the custom uniform slot exists for: the vertex
// layout cannot carry it and the projection occupies vertex slot 0.

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

// space3 is the fragment stage's uniform space in SDL_GPU; b0 is the slot
// nya_render2d_shader_set_uniform pushes to.
cbuffer BlurUniform : register(b0, space3) {
  // One texel, in uv. The shader cannot know the texture's size on its own, and stepping by a
  // guessed amount blurs by a different radius on every resolution.
  float2 texel;

  // (1,0) for the horizontal pass, (0,1) for the vertical one.
  float2 direction;
};

float4 main(FragInput input) : SV_TARGET {
  // Pascal's triangle row 8, normalized: a gaussian close enough that nobody can tell, and cheap to
  // write down.
  const float weights[5] = { 0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162 };

  float2 step = texel * direction;

  float4 total = source.Sample(source_sampler, input.uv) * weights[0];

  [unroll]
  for (int i = 1; i < 5; i++) {
    float2 offset = step * (float)i;

    // Both sides of the centre. The sampler clamps to the edge, so a tap that walks off the texture
    // repeats the border pixel rather than wrapping to the far side.
    total += source.Sample(source_sampler, input.uv + offset) * weights[i];
    total += source.Sample(source_sampler, input.uv - offset) * weights[i];
  }

  return total * input.color;
}
