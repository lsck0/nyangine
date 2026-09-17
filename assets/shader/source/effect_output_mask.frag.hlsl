// Zeroes the frame's alpha under an alpha replacing blend, marking what is drawn so far as the scene. See
// nya_render_output_scene_end.

struct FragInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

float4 main(FragInput input) : SV_Target {
  return float4(0.0, 0.0, 0.0, 0.0);
}
