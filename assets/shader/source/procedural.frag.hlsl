// The demo's backdrop: a slow gradient with a drifting glow and a vignette.
//
// Pairs with procedural.vert.hlsl, and exists to be the thing everything else is drawn on top of.
// It is deliberately dark and low contrast — a background that competes with the panels in front of
// it is a worse background than none, and the previous version of this shader was a screen-filling
// animated triangle that did exactly that.
//
// Cheap on purpose: no texture, no loop, a handful of arithmetic ops per pixel. A backdrop should
// not be the most expensive thing in the frame.

struct VertOutput {
  float4 pos : SV_POSITION;
  float2 uv : TEXCOORD0;
};

cbuffer Uniforms : register(b0, space3) {
  // Seconds since startup, pushed by nya_render2d_procedural into slot 0 of both stages.
  float now;
};

float4 main(VertOutput input) : SV_TARGET {
  float2 uv = input.uv;

  // Top to bottom, near black to a faint blue. The base the rest sits on.
  float3 top    = float3(0.045, 0.050, 0.070);
  float3 bottom = float3(0.015, 0.018, 0.030);
  float3 color  = lerp(top, bottom, uv.y);

  /*
   * Two slow glows on independent orbits.
   *
   * Two rather than one, at different speeds and different colours, because a single moving
   * highlight reads as an object crossing the screen — where two overlapping ones read as light,
   * which is what this is meant to be.
   */
  float2 first  = float2(0.30 + (0.10 * sin(now * 0.21)), 0.35 + (0.08 * cos(now * 0.17)));
  float2 second = float2(0.72 + (0.09 * cos(now * 0.13)), 0.62 + (0.07 * sin(now * 0.23)));

  // Inverse square rather than a smoothstep: it never quite reaches zero, so the falloff has no
  // visible edge where the glow ends.
  float first_glow  = 0.030 / (0.045 + dot(uv - first, uv - first));
  float second_glow = 0.024 / (0.055 + dot(uv - second, uv - second));

  color += float3(0.10, 0.22, 0.42) * first_glow;
  color += float3(0.26, 0.10, 0.34) * second_glow;

  // A faint grid, to give the flat areas some texture without drawing anything.
  float2 grid  = abs(frac(uv * float2(28.0, 16.0)) - 0.5);
  float  lines = smoothstep(0.48, 0.5, max(grid.x, grid.y));
  color += lines * 0.012;

  // Corners darkened, so the panels in front of it have something to sit against.
  float2 vignette_uv = uv * (1.0 - uv.yx);
  float  vignette    = pow(saturate(vignette_uv.x * vignette_uv.y * 18.0), 0.30);

  return float4(color * vignette, 1.0);
}
