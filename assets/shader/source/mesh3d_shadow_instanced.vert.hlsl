// Vertex stage for the shadow pass over retained meshes. See nya_render3d_shadow_begin.
//
// mesh3d_shadow.vert.hlsl with a model matrix in front of it, and it exists for exactly the reason that
// file's own note gives: the game draws its scene twice with the same calls and only the matrix in the
// uniform differs. That stays true for instanced meshes — but the vertices arriving here are in *model*
// space, so the light's matrix cannot be applied to them directly.
//
// A mesh that had no shadow variant would be a mesh that lit correctly and cast nothing, which reads as
// the model floating rather than as a missing pass.

struct VertInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;

  // Per instance, matching NYA_Render3DInstance. The columns of the model matrix; see the long note in
  // mesh3d_instanced.vert.hlsl for why they are columns and why they are not reassembled into a matrix.
  float4 model_column_0 : TEXCOORD1;
  float4 model_column_1 : TEXCOORD2;
  float4 model_column_2 : TEXCOORD3;
  float4 model_column_3 : TEXCOORD4;

  // Declared but unused: the depth of a surface does not depend on its colour. It has to be declared
  // anyway, because the pipeline's vertex layout is shared with the scene pass and an attribute the
  // layout provides but the shader omits is a mismatch some backends reject.
  float4 tint : TEXCOORD5;
};

cbuffer Uniforms : register(b0, space1) {
  // The light's view-projection. Same slot the scene pass puts the camera's in.
  float4x4 view_projection;
};

struct VertOutput {
  float4 position : SV_POSITION;

  /** Clip-space depth, passed through for the fragment stage to write. See the non-instanced variant. */
  float depth : TEXCOORD0;
};

VertOutput main(VertInput input) {
  VertOutput output;

  float4 world = (input.model_column_0 * input.position.x) + (input.model_column_1 * input.position.y) +
                 (input.model_column_2 * input.position.z) + input.model_column_3;

  output.position = mul(view_projection, float4(world.xyz, 1.0));

  // Orthographic light, so w is one; divided anyway so a perspective light needs no edit here.
  output.depth = output.position.z / output.position.w;

  return output;
}
