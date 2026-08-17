// The 2D light map. See nya_render2d_lights_apply.
//
// Runs over a fullscreen triangle and computes, per pixel, how much light reaches it. The result is
// drawn with NYA_BLEND_MULTIPLY over the finished scene, so this shader outputs a *multiplier*: 1 is
// fully lit and leaves the pixel alone, 0 is unlit and blacks it out.
//
// Multiply rather than a deferred pass, because a 2D scene has no normals and no depth to shade
// with. Every light here is a radial falloff around a point, which is what a torch, a lamp or an
// explosion looks like from directly above — and the whole of what "emits light" can mean without
// geometry to bounce off.

struct FragInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

// Sixteen, which is what fits comfortably in one uniform push alongside the header. A scene wanting
// more wants a tiled or clustered pass, which is a different design rather than a bigger array.
#define MAX_LIGHTS 16

cbuffer Uniforms : register(b0, space3) {
  // xy is the light's position in *target pixels*, z is its radius in the same units, w is its
  // intensity. Packed into one float4 per light because a cbuffer pads a float3 out to four anyway,
  // so the intensity rides along for free.
  float4 lights[MAX_LIGHTS];

  // rgb is the light's colour, a is unused padding. Split from the line above rather than
  // interleaved so both arrays are naturally sixteen-byte aligned.
  float4 light_colors[MAX_LIGHTS];

  // How lit an unlit pixel is. Zero is pitch black, which almost nothing wants — a night scene still
  // has a moon. This is the floor the falloff never goes below.
  float3 ambient;

  // How many entries of the arrays above are real.
  float count;

  // Pixels across the target, so the fullscreen uv can be turned back into the same coordinates the
  // lights are in. Passing the lights in uv instead would make a light's radius change shape with
  // the window's aspect.
  float2 target_size;

  float2 _padding;
};

float4 main(FragInput input) : SV_Target {
  /*
   * The uv arrives with y pointing *up*, because procedural.vert.hlsl builds it straight out of clip
   * space and clip space y grows upward. Everything else in this engine measures y downward from the
   * top of the target — mouse coordinates, entity positions, the 2D camera.
   *
   * So the flip happens here, once, rather than at every place that hands a light position in.
   * Without it the whole light map is mirrored vertically, which reads as the lights being in the
   * wrong place rather than as a coordinate convention.
   */
  float2 pixel = float2(input.uv.x, 1.0 - input.uv.y) * target_size;

  float3 accumulated = ambient;

  int light_count = (int)count;

  for (int i = 0; i < light_count; i++) {
    float2 to_light = lights[i].xy - pixel;
    float radius = lights[i].z;

    if (radius <= 0.0) continue;

    // Normalised distance, so the falloff below is in units of the light's own radius and one shape
    // serves every size.
    float distance = length(to_light) / radius;

    if (distance >= 1.0) continue;

    /*
     * Smooth inverse-square-ish falloff, windowed to reach exactly zero at the radius.
     *
     * A true inverse square never reaches zero, so every light would touch every pixel and the
     * radius would be a lie — the `distance >= 1` cull above would then produce a visible ring where
     * the light is cut off. The (1 - d^2)^2 window is the standard fix: it looks like an inverse
     * square near the middle and lands on zero with zero slope at the edge, so the cull is invisible.
     */
    float window = 1.0 - (distance * distance);
    float falloff = window * window;

    accumulated += light_colors[i].rgb * (falloff * lights[i].w);
  }

  // Not clamped to one. Values above it are what makes an over-bright area read as blown out rather
  // than merely white, and the multiply blend carries that through to the scene underneath.
  return float4(max(accumulated, 0.0), 1.0);
}
