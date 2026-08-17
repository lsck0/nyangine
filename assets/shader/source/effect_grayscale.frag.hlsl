// A custom fragment stage, to be used with nya_render2d_shader_begin.
//
// Exists as the worked example of what a game's own shader looks like: same vertex stage, same
// vertex layout, same uniform slot — only this file differs from shape_textured.frag.hlsl. Copy it
// and change the body.
//
// Desaturates whatever it samples, which is a post-process a game would apply to a paused or
// defeated frame after rendering it into a render texture.

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

float4 main(FragInput input) : SV_TARGET {
  float4 texel = source.Sample(source_sampler, input.uv) * input.color;

  // Rec. 601 luma weights rather than a flat average: the eye is far more sensitive to green than
  // to blue, and averaging makes a saturated blue and a saturated green come out the same grey.
  float luma = dot(texel.rgb, float3(0.299, 0.587, 0.114));

  return float4(luma, luma, luma, texel.a);
}
