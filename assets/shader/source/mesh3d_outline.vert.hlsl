// Vertex stage for the inverted-hull outline. See nya_render3d_outline_set.
//
// mesh3d_instanced.vert.hlsl with one line added: the position is pushed out along its own normal before
// it is projected. Everything else — the instance columns, the world-space reconstruction, the reason the
// matrix is not reassembled into a float4x4 — is identical, and the note there covers all of it.
//
// The expansion happens in *world* space rather than model space, after the instance transform. Doing it
// before would scale the outline with the object: a model at twice the size would get a line twice as
// thick, and a set of props at different scales would have visibly different inks.

struct VertInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;

  float4 model_column_0 : TEXCOORD1;
  float4 model_column_1 : TEXCOORD2;
  float4 model_column_2 : TEXCOORD3;
  float4 model_column_3 : TEXCOORD4;

  // Declared because the vertex layout provides it, and unused: an outline is one flat colour by
  // definition, and taking the instance's tint would make each object's ink its own colour.
  float4 tint : TEXCOORD5;
};

cbuffer Uniforms : register(b0, space1) {
  float4x4 view_projection;
};

// The ink itself, in its own block so the outline colour and width are not tangled up with the scene's
// lighting uniform — this pipeline runs a fragment shader that reads no light at all.
//
// b1/space1, not b0/space3. SDL_GPU assigns uniform buffers by *stage*: a vertex shader's are space1 and a
// fragment shader's are space3, and the register index counts within the stage. Putting a vertex uniform
// in space3 is asking for a fragment binding from a vertex shader — which Vulkan tolerated and D3D12
// refused outright, since the root signature is built from the declared spaces. The same mistake, in the
// same shape, as the bloom pipeline's missing uniform-buffer count.
cbuffer OutlineUniform : register(b1, space1) {
  float4 outline_color;
  float outline_thickness;
  float3 outline_pad;
};

struct VertOutput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
};

VertOutput main(VertInput input) {
  VertOutput output;

  float4 world = (input.model_column_0 * input.position.x) + (input.model_column_1 * input.position.y) +
                 (input.model_column_2 * input.position.z) + input.model_column_3;

  float3 world_normal = (input.model_column_0.xyz * input.normal.x) + (input.model_column_1.xyz * input.normal.y) +
                        (input.model_column_2.xyz * input.normal.z);

  /*
   * Renormalised before it is used as a distance.
   *
   * The instance columns carry the object's scale, so an unnormalised normal here would make the ink
   * thickness follow the scale — which is the exact thing expanding in world space was meant to avoid.
   *
   * A zero-length normal, from a degenerate triangle, normalizes to zero and moves the vertex nowhere.
   * That leaves the shell coincident with the model there, which z-fights rather than crashing, and is
   * the safe failure for geometry that was already broken.
   */
  float3 expanded = world.xyz + (normalize(world_normal) * outline_thickness);

  output.position = mul(view_projection, float4(expanded, 1.0));
  output.color = outline_color;

  return output;
}
