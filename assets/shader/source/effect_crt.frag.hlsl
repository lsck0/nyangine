// A CRT look: barrel distortion, scanlines, chromatic aberration and a vignette.
//
// Four effects rather than one because no single one of them reads as a CRT — the curve alone looks
// like a lens, the scanlines alone like a filter. Each is separately controllable so a subtle
// setting is possible; all four at full strength is a caricature.

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer CrtUniform : register(b0, space3) {
  // How far the screen bulges. 0 is flat, 0.1 is a gentle tube, past 0.3 is a fishbowl.
  float curvature;

  // Scanline count down the image, usually the target's height in pixels for one line per row.
  float scanline_count;

  // How dark the gaps between scanlines go. 0 disables them.
  float scanline_strength;

  // Colour fringing at the edges, in texels. Zero disables it.
  float aberration;
};

float2 curve_uv(float2 uv, float amount) {
  // Work from the centre so the bulge is symmetric.
  float2 centered = (uv - 0.5) * 2.0;

  // Push each axis out by the square of the other: the classic cheap barrel, and enough at these
  // strengths that a real lens model would not be visible.
  centered.x *= 1.0 + pow(abs(centered.y) * amount, 2.0);
  centered.y *= 1.0 + pow(abs(centered.x) * amount, 2.0);

  return (centered * 0.5) + 0.5;
}

float4 main(FragInput input) : SV_TARGET {
  float2 uv = curve_uv(input.uv, curvature);

  // Outside the tube after curving. Black rather than clamping, which would smear the edge pixel
  // outwards into the bezel and look like a stretch rather than an edge.
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return float4(0.0, 0.0, 0.0, 1.0);

  float4 result;

  if (aberration > 0.0) {
    // Channels sampled at slightly different offsets, growing towards the edges — which is where a
    // real tube's convergence error actually shows.
    float2 offset = (uv - 0.5) * aberration * 0.01;

    result.r = source.Sample(source_sampler, uv + offset).r;
    result.g = source.Sample(source_sampler, uv).g;
    result.b = source.Sample(source_sampler, uv - offset).b;
    result.a = source.Sample(source_sampler, uv).a;
  } else {
    result = source.Sample(source_sampler, uv);
  }

  if (scanline_strength > 0.0) {
    // A sine across the vertical axis, so lines are soft rather than a hard on/off comb — which
    // aliases badly the moment the image is not displayed at exactly one texel per pixel.
    float lines = sin(uv.y * scanline_count * 3.14159265);
    result.rgb *= 1.0 - (scanline_strength * (0.5 - (lines * 0.5)));
  }

  // A vignette, always on: a tube is dimmer at the corners, and without it the curve reads as a
  // distortion of a flat panel rather than as the shape of the glass.
  float2 vignette_uv = uv * (1.0 - uv.yx);
  float  vignette    = pow(vignette_uv.x * vignette_uv.y * 15.0, 0.25);

  result.rgb *= saturate(vignette);

  return result * input.color;
}
