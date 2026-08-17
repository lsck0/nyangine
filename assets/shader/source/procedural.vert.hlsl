// A fullscreen triangle built from nothing but the vertex index.
//
// The vertex stage for nya_render2d_procedural: no vertex buffer is bound, so the only input is
// SV_VertexID and the positions are written here. That is the whole point of the pipeline — it is
// what a fullscreen pass, a debug primitive or a generated mesh looks like.
//
// A triangle rather than a quad, and deliberately oversized. Two triangles meeting across a
// screen's diagonal are rasterised as two, and the quads straddling that seam are shaded twice; one
// triangle large enough to cover the viewport has no seam. The parts hanging off the screen cost
// nothing, because clipping happens before rasterisation.
//
//   id 0 -> (-1, -1)    id 1 -> (3, -1)    id 2 -> (-1, 3)
//
// It declared POSITION, COLOR0, NORMAL0 and TEXCOORD0 alongside the index and used none of them.
// Nothing binds a buffer to feed those, and a vertex input a pipeline does not actually receive is
// the same trap NYA_VERTEX_LAYOUT_2D exists to avoid.

struct VertInput {
  uint id : SV_VertexID;
};

struct VertOutput {
  float4 pos : SV_POSITION;

  // Zero to one across the covered area. Interpolated for free, so the fragment stage needs no
  // resolution uniform to know where it is.
  float2 uv : TEXCOORD0;
};

VertOutput main(VertInput input) {
  VertOutput output;

  float2 corner = float2((input.id == 1) ? 3.0 : -1.0, (input.id == 2) ? 3.0 : -1.0);

  output.pos = float4(corner, 0.0, 1.0);
  output.uv  = (corner * 0.5) + 0.5;

  return output;
}
