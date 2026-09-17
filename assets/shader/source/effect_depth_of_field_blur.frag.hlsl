// Depth of field, first half: the blurred scene at half resolution. See NYA_PostDepthOfField.
//
// A hexagon of taps with flat weights rather than a gaussian, so bright points open into flat hexagons like a painted
// bokeh. A tap only counts when its own blur reaches this pixel, which keeps a sharp object from smearing into the
// blurred background behind it.

#include "effect_depth_of_field.hlsli"

Texture2D scene : register(t0, space2);
SamplerState scene_sampler : register(s0, space2);

// the scene normal buffer. tilt shift binds the image here and never reads it.
Texture2D normals : register(t1, space2);
SamplerState normals_sampler : register(s1, space2);

// a hexagon: six taps halfway out, six corners and the six edge middles between them.
static const int TAPS = 18;

static const float2 HEXAGON[TAPS] = {
  float2(0.5, 0.0),    float2(0.25, 0.433), float2(-0.25, 0.433), float2(-0.5, 0.0),     float2(-0.25, -0.433), float2(0.25, -0.433),
  float2(1.0, 0.0),    float2(0.5, 0.866),  float2(-0.5, 0.866),  float2(-1.0, 0.0),     float2(-0.5, -0.866),  float2(0.5, -0.866),
  float2(0.75, 0.433), float2(0.0, 0.866),  float2(-0.75, 0.433), float2(-0.75, -0.433), float2(0.0, -0.866),   float2(0.75, -0.433),
};

// how much more a highlight weighs than the rest, so lamps and the sun spread into visible hexagons.
static const float HIGHLIGHT_WEIGHT = 6.0;

float highlight(float3 colour) {
  return 1.0 + HIGHLIGHT_WEIGHT * smoothstep(0.85, 1.0, dot(colour, float3(0.299, 0.587, 0.114)));
}

float4 main(FragInput input) : SV_Target {
  float2 uv = float2(input.uv.x, 1.0 - input.uv.y);

  float amount = depth_of_field_amount(normals, normals_sampler, uv);
  float reach = amount * radius;

  float3 centre = scene.SampleLevel(scene_sampler, uv, 0.0).rgb;

  float weight = highlight(centre);
  float3 sum = centre * weight;

  if (reach > 0.0) {
    for (int i = 0; i < TAPS; i++) {
      float2 tap_uv = uv + HEXAGON[i] * reach * texel;

      // this tap's own blur has to cover the distance to the centre, with a texel of slack for a soft rim.
      float covers = saturate(depth_of_field_amount(normals, normals_sampler, tap_uv) * radius - length(HEXAGON[i]) * reach + 1.0);

      float3 colour = scene.SampleLevel(scene_sampler, tap_uv, 0.0).rgb;
      float tap_weight = covers * highlight(colour);

      sum += colour * tap_weight;
      weight += tap_weight;
    }
  }

  return float4(sum / weight, 1.0);
}
