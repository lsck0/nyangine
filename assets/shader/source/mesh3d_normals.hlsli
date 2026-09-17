// The scene pass's second output, the normal buffer the scene post passes read. See NYA_RENDER3D_NORMAL_FORMAT.
// Included after mesh3d_shading.hlsli, whose cbuffer has the camera.
//
// Written by every opaque mesh shader whether or not the pass attaches the buffer: a pipeline without the second
// target drops it, and one that does not write depth masks it.

struct Mesh3DOutput {
  float4 colour : SV_Target0;
  float4 normal : SV_Target1;
};

Mesh3DOutput mesh3d_output(float4 colour, float3 normal, float3 world_position) {
  Mesh3DOutput output;

  output.colour = colour;
  output.normal = float4(normal, length(camera_position - world_position));

  return output;
}
