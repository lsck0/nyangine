// The SDR frame written to an HDR swapchain. See render_output.h.
//
// The frame holds sRGB encoded colour with white at one. Decoded to linear light, it is what the compositor
// shows as SDR white, so everything comes out as it did in SDR, and only colours past `highlight`, where the
// scene tonemap squeezed lamps and fire against white, are lifted toward `peak`. Once the frame marks where its scene
// ends, alpha tells scene from what was drawn over it, and only the scene is lifted.

struct FragInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

Texture2D scene : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

cbuffer OutputUniform : register(b0, space3) {
  // 0 extended linear sRGB, 1 HDR10: BT.2020 primaries through the PQ curve.
  float encoding;

  // the brightest highlight in multiples of SDR white.
  float peak;

  // the encoded value past which a colour is a highlight.
  float highlight;

  // SDR white in nits, for HDR10, which is absolute.
  float paper_white;

  // 1 when alpha marks the scene: zero where only the scene was drawn, raised by whatever covers it.
  float masked;
  float3 pad;
};

float3 srgb_to_linear(float3 colour) {
  return lerp(pow((colour + 0.055) / 1.055, 2.4), colour / 12.92, step(colour, 0.04045));
}

float3 pq_encode(float3 nits) {
  const float m1 = 0.1593017578125;
  const float m2 = 78.84375;
  const float c1 = 0.8359375;
  const float c2 = 18.8515625;
  const float c3 = 18.6875;

  float3 y = pow(saturate(nits / 10000.0), m1);

  return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float4 main(FragInput input) : SV_Target {
  float4 frame = scene.Sample(scene_sampler, float2(input.uv.x, 1.0 - input.uv.y));
  float3 colour = saturate(frame.rgb);

  float scene_share = masked > 0.5 ? 1.0 - saturate(frame.a) : 1.0;

  // by the brightest channel, so a lifted highlight keeps its hue.
  float lift = lerp(1.0, peak, smoothstep(highlight, 1.0, max(colour.r, max(colour.g, colour.b))) * scene_share);

  float3 light = srgb_to_linear(colour) * lift;

  if (encoding < 0.5) return float4(light, 1.0);

  static const float3x3 BT709_TO_BT2020 = {
    0.6274, 0.3293, 0.0433,
    0.0691, 0.9195, 0.0114,
    0.0164, 0.0880, 0.8956,
  };

  return float4(pq_encode(mul(BT709_TO_BT2020, light) * paper_white), 1.0);
}
