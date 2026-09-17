// What the depth of field passes share: the uniform block and how blurred a point is. See NYA_PostDepthOfField.

// Matches NYA_ShaderDepthOfFieldUniform.
cbuffer DepthOfFieldUniform : register(b0, space3) {
  // one texel of the full image, and the widest blur in those texels.
  float2 texel;
  float radius;

  // 1 tilt shift, 2 distance. see NYA_PostFocus.
  float focus;

  float band_center;
  float band;
  float falloff;
  float layers;

  float focus_distance;
  float focus_range;
  float2 pad;
};

struct FragInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

// past any scene, for a texel no surface wrote.
static const float DEPTH_OF_FIELD_NOTHING = 100000.0;

/**
 * How blurred the image is at `uv`, stepped into `layers` so the scene separates into a few flat planes. Distance
 * focus reads the camera distance the scene normal buffer keeps in alpha; tilt shift never samples it.
 * */
float depth_of_field_amount(Texture2D normals, SamplerState normals_sampler, float2 uv) {
  float outside;

  if (focus < 1.5) {
    outside = abs(uv.y - band_center) - band;
  } else {
    float distance = normals.SampleLevel(normals_sampler, uv, 0.0).a;

    outside = abs((distance > 0.0 ? distance : DEPTH_OF_FIELD_NOTHING) - focus_distance) - focus_range;
  }

  return round(saturate(outside / falloff) * layers) / layers;
}
