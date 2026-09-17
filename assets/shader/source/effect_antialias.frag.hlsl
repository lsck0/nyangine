// FXAA: finds edges by luma contrast, works out which way each runs by walking along it, and re-samples across it
// by how far the pixel sits from the edge's end. See NYA_PostAntialias.
//
// The quality variant's structure with a short walk. Ten steps reach about twenty pixels, which covers the
// staircases a 3D scene and its ink produce.

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

// Matches NYA_ShaderAntialiasUniform.
cbuffer AntialiasUniform : register(b0, space3) {
  float2 texel;
  float subpixel;
  float threshold;
};

struct FragInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

/** Below this much contrast nothing is an edge, however dark the area; noise in shadows is not worth smoothing. */
static const float ANTIALIAS_CONTRAST_FLOOR = 0.0312;

static const int ANTIALIAS_EDGE_STEPS = 10;

static const float ANTIALIAS_STEP_SIZES[ANTIALIAS_EDGE_STEPS] = { 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };

float luma_at(float2 uv) {
  return dot(source.SampleLevel(source_sampler, uv, 0.0).rgb, float3(0.299, 0.587, 0.114));
}

float4 main(FragInput input) : SV_Target {
  float2 uv = float2(input.uv.x, 1.0 - input.uv.y);

  float4 centre = source.SampleLevel(source_sampler, uv, 0.0);

  float middle = dot(centre.rgb, float3(0.299, 0.587, 0.114));
  float north = luma_at(uv + float2(0.0, -texel.y));
  float south = luma_at(uv + float2(0.0, texel.y));
  float west = luma_at(uv + float2(-texel.x, 0.0));
  float east = luma_at(uv + float2(texel.x, 0.0));

  float lowest = min(middle, min(north, min(south, min(west, east))));
  float highest = max(middle, max(north, max(south, max(west, east))));
  float contrast = highest - lowest;

  if (contrast < max(ANTIALIAS_CONTRAST_FLOOR, highest * threshold)) return centre;

  float north_west = luma_at(uv + float2(-texel.x, -texel.y));
  float north_east = luma_at(uv + float2(texel.x, -texel.y));
  float south_west = luma_at(uv + float2(-texel.x, texel.y));
  float south_east = luma_at(uv + float2(texel.x, texel.y));

  // how much this pixel differs from its neighbourhood, for detail smaller than a pixel.
  float neighbourhood = ((2.0 * (north + south + west + east)) + north_west + north_east + south_west + south_east) / 12.0;
  float subpixel_blend = smoothstep(0.0, 1.0, saturate(abs(neighbourhood - middle) / contrast));

  subpixel_blend = subpixel_blend * subpixel_blend * subpixel;

  float horizontal = (abs(north + south - (2.0 * middle)) * 2.0) + abs(north_east + south_east - (2.0 * east)) +
                     abs(north_west + south_west - (2.0 * west));
  float vertical = (abs(east + west - (2.0 * middle)) * 2.0) + abs(north_east + north_west - (2.0 * north)) +
                   abs(south_east + south_west - (2.0 * south));

  bool along_x = horizontal >= vertical;

  // across the edge, toward whichever side differs more.
  float2 across = along_x ? float2(0.0, texel.y) : float2(texel.x, 0.0);
  float positive = along_x ? south : east;
  float negative = along_x ? north : west;

  float opposite = positive;
  float gradient = abs(positive - middle);

  if (abs(negative - middle) > gradient) {
    across = -across;
    opposite = negative;
    gradient = abs(negative - middle);
  }

  float2 along = along_x ? float2(texel.x, 0.0) : float2(0.0, texel.y);
  float2 edge_uv = uv + (across * 0.5);
  float edge_luma = (middle + opposite) * 0.5;
  float end_contrast = gradient * 0.25;

  float2 forward_uv = edge_uv + along;
  float2 backward_uv = edge_uv - along;
  float forward_delta = luma_at(forward_uv) - edge_luma;
  float backward_delta = luma_at(backward_uv) - edge_luma;

  bool forward_end = abs(forward_delta) >= end_contrast;
  bool backward_end = abs(backward_delta) >= end_contrast;

  [loop]
  for (int i = 1; i < ANTIALIAS_EDGE_STEPS && !(forward_end && backward_end); i++) {
    if (!forward_end) {
      forward_uv += along * ANTIALIAS_STEP_SIZES[i];
      forward_delta = luma_at(forward_uv) - edge_luma;
      forward_end = abs(forward_delta) >= end_contrast;
    }

    if (!backward_end) {
      backward_uv -= along * ANTIALIAS_STEP_SIZES[i];
      backward_delta = luma_at(backward_uv) - edge_luma;
      backward_end = abs(backward_delta) >= end_contrast;
    }
  }

  float forward_distance = along_x ? forward_uv.x - uv.x : forward_uv.y - uv.y;
  float backward_distance = along_x ? uv.x - backward_uv.x : uv.y - backward_uv.y;

  float nearest = min(forward_distance, backward_distance);
  float nearest_delta = forward_distance <= backward_distance ? forward_delta : backward_delta;

  // only the end the pixel's own side of the edge runs into blends; the other side's end is someone else's.
  float edge_blend = (nearest_delta >= 0.0) == (middle - edge_luma >= 0.0) ? 0.0 : 0.5 - (nearest / (forward_distance + backward_distance));

  float blend = max(subpixel_blend, edge_blend);

  return float4(source.SampleLevel(source_sampler, uv + (across * blend), 0.0).rgb, centre.a);
}
