// Vertex stage for the 3D mesh batch. See src/nyangine/renderer/render3d.h.
//
// Takes vertices already in *world* space and does nothing but view-project them. No model matrix,
// for the same reason batch2d.vert.hlsl has none: every primitive in a flush shares one draw call
// and so cannot have a per-primitive uniform. The batch bakes the transform in as it builds the
// vertices, on the CPU, which is what makes a hundred cubes one draw call instead of a hundred.

// Matches NYA_Vertex3D: position, colour, normal, uv. The normal arrives already rotated into world
// space — same reasoning as the position.
struct VertInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
};

cbuffer Uniforms : register(b0, space1) {
  float4x4 view_projection;
};

struct VertOutput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;

  /**
   * The shaded point in world space, passed through untouched.
   *
   * Needed by anything positional in the fragment stage: the view vector for the highlight and rim, and
   * every point light's direction and distance. It cannot be recovered from SV_POSITION, which by the
   * time the fragment stage sees it holds screen coordinates and a reciprocal w.
   *
   * That distinction was a live bug rather than a hypothetical one. The fragment stage used to compute
   * its view vector as `camera_position - input.position.xyz`, reading screen space as though it were
   * world space — so the highlight and rim were positioned by where a fragment landed on the window
   * instead of where it sat in the scene.
   * */
  float3 world_position : TEXCOORD1;
};

VertOutput main(VertInput input) {
  VertOutput output;

  // mul(matrix, vector), not mul(vector, matrix) — see the note in batch2d.vert.hlsl. Here the cost
  // of getting it backwards is worse, not better: a perspective matrix's last *row* is what fills w,
  // and transposing it puts -z in the wrong place and inverts the projective divide.
  output.position = mul(view_projection, float4(input.position, 1.0));
  output.color = input.color;

  // Not renormalized here. The batch only ever rotates normals, never scales them non-uniformly, so
  // they arrive unit and the rasterizer's interpolation is the only thing that shortens them — which
  // the fragment stage corrects, once, where it matters.
  output.normal = input.normal;
  output.uv = input.uv;

  // Already world space on the way in — the batch bakes the transform in on the CPU — so this is a copy
  // rather than a second multiply.
  output.world_position = input.position;

  return output;
}
