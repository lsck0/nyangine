// Vertex stage for a skinned mesh in the shadow pass. See nya_render3d_skinned_mesh.
//
// mesh3d_shadow.vert.hlsl with the skinning from mesh3d_skinned.vert.hlsl in front of it, and it
// exists for the reason the instanced shadow variant does: the scene is drawn twice with the same
// call and only the matrix in the uniform differs. That stays true for a skinned mesh — but its
// vertices are in geometry space and move with the pose, so the light's matrix cannot be applied to
// them directly.
//
// A skinned mesh with no shadow variant is a character that lights correctly and casts nothing, which
// reads as the model hovering rather than as a missing pass.

struct VertInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;

  uint4 bones : TEXCOORD1;
  float4 weights : TEXCOORD2;
};

cbuffer Uniforms : register(b0, space1) {
  // The light's view-projection. The same slot the scene pass puts the camera's in.
  float4x4 view_projection;
};

// Matches mesh3d_skinned.vert.hlsl exactly, because the same palette is pushed to both. See
// NYA_ShaderSkinUniform.
#define SKIN_MAX_BONES 64

cbuffer SkinUniform : register(b1, space1) {
  float4 bone_rows[SKIN_MAX_BONES * 3];
  float4 skin_tint;
};

struct VertOutput {
  float4 position : SV_POSITION;

  /** Clip-space depth, passed through for the fragment stage to write. See the non-skinned variant. */
  float depth : TEXCOORD0;
};

float4x4 bone_matrix(uint index) {
  uint row = index * 3;

  return float4x4(bone_rows[row + 0], bone_rows[row + 1], bone_rows[row + 2], float4(0.0, 0.0, 0.0, 1.0));
}

VertOutput main(VertInput input) {
  VertOutput output;

  /*
   * The same skin as the scene pass, and it has to be exactly the same.
   *
   * A shadow map records where a surface *is*. Posing the shadow differently from the surface — by a
   * frame, by a different palette, by skipping the blend — puts the recorded depth somewhere the lit
   * geometry is not, and the model shadows itself in stripes.
   *
   * The normal is not skinned here: depth does not depend on which way a surface faces.
   */
  float4 local = float4(input.position, 1.0);

  float3 skinned = 0.0;

  [unroll]
  for (int i = 0; i < 4; i++) {
    float weight = input.weights[i];

    if (weight <= 0.0) continue;

    skinned += mul(bone_matrix(input.bones[i]), local).xyz * weight;
  }

  output.position = mul(view_projection, float4(skinned, 1.0));

  // Orthographic light, so w is one; divided anyway so a perspective light needs no edit here.
  output.depth = output.position.z / output.position.w;

  return output;
}
