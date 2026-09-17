// Shared by both halves of eye adaptation. See NYA_PostEyeAdaptation.

struct FragInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

Texture2D source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

// one texel: the log brightness the eye is used to.
Texture2D adapted : register(t1, space2);
SamplerState adapted_sampler : register(s1, space2);

// Matches NYA_ShaderEyeAdaptationUniform.
cbuffer AdaptationUniform : register(b0, space3) {
  float key;
  float exposure_min;
  float exposure_max;
  float saturation;

  float rate_dark;
  float rate_bright;
  float2 jitter;
};

static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);
