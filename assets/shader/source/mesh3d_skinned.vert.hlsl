// Vertex stage for a skinned mesh. See nya_render3d_skinned_mesh.
//
// mesh3d.vert.hlsl with one difference, and it is the difference that matters everywhere else: the
// vertices arriving here are in the mesh's own *geometry* space rather than in world space. Every
// other 3D pipeline takes world-space vertices because the batch bakes the transform in on the CPU,
// which is what makes a hundred cubes one draw call. A skinned mesh cannot do that — its vertices
// move every frame and by a different matrix each — so the transform arrives as a bone palette and
// is applied here.
//
// ## Why the palette is three rows
//
// A bone transform is affine, so its fourth row is always (0, 0, 0, 1). Sending it would spend a
// quarter of a three-kibibyte uniform block restating that. See NYA_ShaderSkinUniform.

struct VertInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;

  // Matching NYA_VertexSkinned3D. Indices into the palette, and how much each one pulls.
  uint4 bones : TEXCOORD1;
  float4 weights : TEXCOORD2;
};

cbuffer Uniforms : register(b0, space1) {
  float4x4 view_projection;
};

// b1/space1, beside the view-projection rather than at space3. SDL_GPU assigns uniform slots by
// *stage*: a vertex shader's are space1. Putting this at space3 asks for a fragment binding from a
// vertex shader, which Vulkan tolerates and D3D12 refuses — the same mistake the outline pipeline
// made once, recorded in mesh3d_outline.vert.hlsl.
// 64 bones, three rows each. Matches NYA_ShaderSkinUniform and NYA_SHADER_SKIN_MAX_BONES, which
// uniforms.h keeps on the C side — the shaders here do not include it, so the number is written out
// in both places and checked by a static assertion beside nya_render3d_skinned_mesh.
#define SKIN_MAX_BONES 64

cbuffer SkinUniform : register(b1, space1) {
  float4 bone_rows[SKIN_MAX_BONES * 3];
  float4 skin_tint;
};

struct VertOutput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
  float3 world_position : TEXCOORD1;
};

/** One bone's matrix, rebuilt from its three stored rows. The fourth is the affine constant. */
float4x4 bone_matrix(uint index) {
  uint row = index * 3;

  return float4x4(bone_rows[row + 0], bone_rows[row + 1], bone_rows[row + 2], float4(0.0, 0.0, 0.0, 1.0));
}

VertOutput main(VertInput input) {
  VertOutput output;

  /*
   * Linear blend skinning: the vertex through each bone, weighted, summed.
   *
   * The weights are normalised at load — see the note in core_skeleton.h about the exporter's own
   * weights summing to as little as 0.982 — so this is a weighted average and not merely a sum, and
   * a vertex with a single full-weight influence comes through exactly as that bone moved it.
   *
   * Unrolled over four because that is the vertex layout's cap, and a loop over a constant four with
   * a dynamic index into a uniform array is the shape compilers handle worst.
   */
  float4 local = float4(input.position, 1.0);

  float3 skinned = 0.0;
  float3 skinned_normal = 0.0;

  [unroll]
  for (int i = 0; i < 4; i++) {
    float weight = input.weights[i];

    // Skipped rather than added as zero. A weight of zero still indexes the palette, and a vertex
    // whose unused slots all point at bone zero would otherwise pay three redundant matrix builds.
    if (weight <= 0.0) continue;

    float4x4 bone = bone_matrix(input.bones[i]);

    skinned += mul(bone, local).xyz * weight;

    // The normal takes the rotation and not the translation, which is what the 3x3 does. Non-uniform
    // scale on a bone would want the inverse transpose instead; bones are not scaled that way here,
    // and paying for a per-vertex inverse would be the wrong trade if they were.
    skinned_normal += mul((float3x3)bone, input.normal) * weight;
  }

  output.position = mul(view_projection, float4(skinned, 1.0));
  // The mesh's own vertex colour times the draw's tint, which is how one rig is drawn in several
  // colours without a second copy of its geometry.
  output.color = input.color * skin_tint;

  // Renormalised, unlike the unskinned path: blending two rotations by weight shortens the result
  // wherever they disagree, which is exactly the middle of every joint.
  output.normal = normalize(skinned_normal);
  output.uv = input.uv;

  // The skinned position, not the input one. This is what the fragment stage lights against, and
  // handing it the bind pose would put every highlight where the limb used to be.
  output.world_position = skinned;

  return output;
}
