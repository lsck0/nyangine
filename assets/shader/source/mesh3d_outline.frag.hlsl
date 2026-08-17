// Fragment stage for the inverted-hull outline. See nya_render3d_outline_set.
//
// The whole shader. An outline is flat ink: it takes no light, samples no texture, and reads no material,
// because a line that shaded would stop reading as a line and start reading as a second object behind the
// first one.
//
// It exists as a file at all because a pipeline needs a fragment stage, and reusing the mesh one would
// pull in the entire lighting uniform block and the shadow map binding to output a constant.

struct FragInput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
};

float4 main(FragInput input) : SV_Target {
  return input.color;
}
