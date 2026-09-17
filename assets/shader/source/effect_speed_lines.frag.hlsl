// Cartoon speed lines: thin spikes radiating from a point, over the image. See NYA_PostSpeedLines.
//
// The circle around the centre is cut into wedges and a few of them hold a line, picked by a hash of the wedge and
// the drawing number. The drawing number only changes a few times a second, so the lines hold still between
// redraws the way inked frames do.

struct FragInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

// Matches NYA_ShaderSpeedLinesUniform.
cbuffer SpeedLinesUniform : register(b0, space3) {
  // where the lines converge, in uv.
  float2 center;
  float aspect;

  // which drawing this is. whole numbers, stepped at NYA_POST_SPEED_LINES_RATE.
  float frame;

  float amount;
  float density;
  float clear_radius;

  // one pixel in units of the screen's height.
  float pixel;

  float4 line_color;
};

static const float TAU = 6.28318530718;

float hash(float wedge, float salt) {
  return frac(sin(wedge * 12.9898 + salt * 78.233) * 43758.5453);
}

float4 main(FragInput input) : SV_Target {
  float2 uv = float2(input.uv.x, 1.0 - input.uv.y);

  float4 scene = source.SampleLevel(source_sampler, uv, 0.0);

  // in screen heights, so the lines are round on a wide screen.
  float2 offset = (uv - center) * float2(aspect, 1.0);
  float radius = max(length(offset), pixel);

  float around = (atan2(offset.y, offset.x) / TAU + 0.5) * density;
  float wedge = floor(around);
  float across = abs(frac(around) - 0.5);

  // more wedges hold a line, and lines reach further in, as the amount grows.
  float present = step(hash(wedge, frame), amount * 0.5);
  float start = clear_radius * (2.2 - 1.2 * amount) + hash(wedge, frame + 7.0) * 0.2;

  // a spike: a share of its wedge, so it narrows toward the centre, and pinched shut where it starts.
  float half_width = lerp(0.04, 0.32, hash(wedge, frame + 3.0)) * saturate((radius - start) / 0.4);

  // one pixel in wedge units at this radius, for an edge that is hard but not jagged.
  float wedge_pixel = density * pixel / (TAU * radius);
  float coverage = saturate((half_width - across) / wedge_pixel) * present * line_color.a;

  return float4(lerp(scene.rgb, line_color.rgb, coverage), max(scene.a, coverage));
}
