// Vertex stage for the 2D shape batch. See src/nyangine/renderer/render2d.h.
//
// Takes vertices already in whatever space the projection expects — pixels, for the screen camera —
// and does nothing but project them. No model transform: the batch bakes position into the vertices
// as it builds them, because every shape in a flush shares one draw call and so cannot have a
// per-shape uniform.

// Matches NYA_Vertex2D: twenty bytes, no normal, and a colour that arrives as four normalized bytes
// the input assembler has already expanded into this float4.
struct VertInput {
  float2 position : POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

cbuffer Uniforms : register(b0, space1) {
  float4x4 projection;
};

struct VertOutput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float2 uv : TEXCOORD0;
};

VertOutput main(VertInput input) {
  VertOutput output;

  // mul(matrix, vector), not mul(vector, matrix). The second form treats the vector as a *row*
  // vector and computes v·M, which is transpose(M)·v — and since nya_matrix_orthographic puts its
  // translation in the last column, that ordering feeds the translate terms into w instead of xy.
  // The result is a projective divide that collapses every shape into a sliver, which looks like
  // broken geometry rather than a transposed matrix.
  // z of zero, w of one. The projection maps z straight through into the 0..1 depth range, and
  // nothing drawn through this is depth tested, so a per vertex depth would be a constant paid for
  // once per vertex.
  output.position = mul(projection, float4(input.position, 0.0, 1.0));
  output.color = input.color;
  output.uv = input.uv;

  return output;
}
