// Vertex stage for the shadow pass. See nya_render3d_shadow_begin in src/nyangine/renderer/render3d.h.
//
// The same vertices the scene pass gets, projected through the *light's* view instead of the camera's.
// Nothing else about the batch changes, which is the point: the game draws its scene twice with the same
// calls, and only the matrix in this uniform differs between the two.

struct VertInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
};

cbuffer Uniforms : register(b0, space1) {
  // The light's view-projection during a shadow pass, where the scene pass puts the camera's. Reusing
  // the slot rather than adding one is what lets the batch upload identical vertices for both passes.
  float4x4 view_projection;
};

struct VertOutput {
  float4 position : SV_POSITION;

  /**
   * Clip-space depth, passed through so the fragment stage can write it.
   *
   * Not read back off SV_POSITION: by the fragment stage that holds window coordinates with a
   * reciprocal w, so its z is no longer the value the shadow comparison needs. The same distinction
   * that made the scene pass carry world position separately.
   * */
  float depth : TEXCOORD0;
};

VertOutput main(VertInput input) {
  VertOutput output;

  output.position = mul(view_projection, float4(input.position, 1.0));

  /*
   * Divided here rather than in the fragment stage.
   *
   * The light's projection is orthographic, so w is one and this division is a no-op that costs nothing
   * and keeps the shader correct if a perspective light is ever added. Interpolating z/w linearly is only
   * right *because* w is constant; a spot light would have to interpolate z and w separately and divide
   * per fragment.
   */
  output.depth = output.position.z / output.position.w;

  return output;
}
