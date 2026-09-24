// The second half of the GPU compute demo: one thread per output texel, each summing a soft splat from
// every particle the integration pass (particle_update.comp.hlsl) left in the buffer, and writing the
// result straight into a storage texture. render_compute_particles.c then draws that texture with the
// ordinary 2D path, so nothing here reads back to the CPU.
//
// This is deliberately a per-texel gather rather than a per-particle scatter: a scatter needs atomics or
// a cleared target and ordered writes, while a gather clears and accumulates in one write per texel with
// no synchronisation to reason about. It is O(texels * particles), which is why the field and the count
// are kept small; it is a demonstration, not a production particle renderer.
//
// SDL_GPU's compute binding model: a read-only storage buffer at (t0, space0), a read-write storage
// texture at (u0, space1), a uniform buffer at (b0, space2). Desktop-only, like its sibling.

struct Particle {
  float2 position;
  float2 velocity;
};

// Read-only here: this pass never writes the particles, only the image.
StructuredBuffer<Particle> particles : register(t0, space0);

// The image the field is drawn into. Created with COMPUTE_STORAGE_WRITE and SAMPLER so the 2D path can
// draw it afterwards.
RWTexture2D<float4> image : register(u0, space1);

cbuffer Params : register(b0, space2) {
  uint  count;    // particles to gather.
  uint  width;    // image size in texels, so the tail of a padded dispatch does nothing.
  uint  height;
  float radius;   // splat radius as a fraction of the field, controls how fat each dot is.
};

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
  if (thread.x >= width || thread.y >= height) return;

  // This texel's centre in the same [0, 1] field space the particles live in.
  float2 uv = (float2(thread.xy) + 0.5) / float2((float)width, (float)height);

  // A dark blue ground the dots glow over.
  float3 color = float3(0.02, 0.03, 0.06);

  float inv_two_sigma_sq = 1.0 / (2.0 * radius * radius);

  for (uint i = 0u; i < count; i++) {
    float2 delta = uv - particles[i].position;
    float  d2    = dot(delta, delta);

    // A gaussian falloff, warm at the core. Additive, so overlapping particles brighten.
    float intensity = exp(-d2 * inv_two_sigma_sq);
    color += float3(1.0, 0.7, 0.3) * intensity;
  }

  image[thread.xy] = float4(color, 1.0);
}
