// Vertex stage for the retained mesh path. See src/nyangine/renderer/render3d.h.
//
// The counterpart to mesh3d.vert.hlsl, and the difference between them is the whole reason this file
// exists: that one takes vertices already in world space, because the immediate batch bakes every
// transform on the CPU and has nowhere to put a per-primitive one. This one takes vertices in *model*
// space and a per-instance model matrix, so one upload of a mesh can be drawn many times, in many
// places, from one draw call.
//
// Which of the two a given piece of geometry should go through is not a matter of taste. Geometry
// generated fresh every frame — a terrain rebuilt from a heightmap, a particle, a debug line — has no
// reuse to exploit and belongs in the immediate batch. Geometry read off disk and drawn repeatedly
// belongs here.

// Buffer 0, per vertex. Matches NYA_Vertex3D, exactly as the immediate path's does.
struct VertInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;

  // Buffer 1, per instance. Matches NYA_Render3DInstance.
  //
  // The four rows below are the model matrix's *columns*, split up because a vertex attribute holds at
  // most four components. TEXCOORD is used as the semantic for all of them because it is the only one
  // with enough indices to go round; the names are what carry the meaning.
  float4 model_column_0 : TEXCOORD1;
  float4 model_column_1 : TEXCOORD2;
  float4 model_column_2 : TEXCOORD3;
  float4 model_column_3 : TEXCOORD4;

  float4 tint : TEXCOORD5;
};

cbuffer Uniforms : register(b0, space1) {
  float4x4 view_projection;
};

struct VertOutput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;

  // World space, for the view vector, the point lights and the shadow lookup. Identical in meaning to
  // the immediate path's, which is what lets both share every fragment shader.
  float3 world_position : TEXCOORD1;
};

/**
 * Model space to world, written out rather than assembled into a float4x4 first.
 *
 * `c0*p.x + c1*p.y + c2*p.z + c3` *is* a matrix-vector product for a column-major matrix — it is the
 * definition of one. Building `float4x4(c0, c1, c2, c3)` and calling mul on it would go through HLSL's
 * constructor, which takes **rows**, so it would silently transpose the transform: rotations would come
 * out inverted and translation would land in the bottom row where nothing reads it.
 * */
float3 model_to_world(VertInput input, float3 point_in_model) {
  float4 world = (input.model_column_0 * point_in_model.x) + (input.model_column_1 * point_in_model.y) +
                 (input.model_column_2 * point_in_model.z) + input.model_column_3;

  return world.xyz;
}

VertOutput main(VertInput input) {
  VertOutput output;

  float3 world_position = model_to_world(input, input.position);

  output.position = mul(view_projection, float4(world_position, 1.0));
  output.world_position = world_position;

  /*
   * The normal is rotated by the same columns, with the translation left out, and then renormalised.
   *
   * Leaving out column 3 is what makes this a direction rather than a point — including it would move
   * every normal by the object's position, which lights a model by where it stands.
   *
   * Renormalised because the columns carry the object's scale. The immediate path can skip this: it only
   * ever rotates normals, so they arrive unit. Here a scaled instance shortens or lengthens them, and an
   * unnormalised normal shades as though the surface were darker or brighter than it is.
   *
   * This is correct for a uniform scale and an approximation for a non-uniform one, where the exact
   * answer is the inverse transpose. Carrying a second matrix per instance to be exact about squashed
   * models is not a trade worth making here; nothing in this renderer scales non-uniformly.
   */
  float3 world_normal = (input.model_column_0.xyz * input.normal.x) + (input.model_column_1.xyz * input.normal.y) +
                        (input.model_column_2.xyz * input.normal.z);

  output.normal = normalize(world_normal);

  // The mesh part's own material colour is already baked into the vertex colour at upload time, so this
  // multiply is the caller's tint alone and a part needs no uniform of its own.
  output.color = input.color * input.tint;
  output.uv = input.uv;

  return output;
}
