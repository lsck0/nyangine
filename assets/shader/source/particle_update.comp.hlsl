// A compute shader that integrates a buffer of particles, one thread per particle. The worked example
// of the GPU compute pipeline: nya_gpu_compute_pipeline_create builds it, nya_gpu_compute_dispatch runs
// it, and render_compute_particles.c drives it once a frame. Its sibling particle_render.comp.hlsl reads
// the buffer this leaves behind and draws it into a texture.
//
// Compute has no cross-compile to GLSL ES 300 (WebGL2 has no compute stage), so a .comp shader is a
// desktop-only path. The build skips its web variant, and the C side is gated behind !OS_WASM; see the
// note in src/build/pp/asset.c and render_compute_particles.c. This is authored for SDL_GPU's compute
// binding model: a read-write storage buffer at (u0, space1) and a uniform buffer at (b0, space2). See
// SDL_CreateGPUComputePipeline for the full order.

// One particle. Kept to sixteen bytes so the buffer stride matches NYA_GPUParticle on the C side exactly,
// with no padding to reason about.
struct Particle {
  float2 position;   // in [0, 1] over the field, the unit square particle_render.comp.hlsl draws.
  float2 velocity;   // field units per second.
};

// Read-write storage buffer. Read from and written to in place, so the integration carries frame to frame.
RWStructuredBuffer<Particle> particles : register(u0, space1);

// space2 is the compute stage's uniform space; b0 is the slot nya_gpu_compute_dispatch pushes to.
cbuffer Params : register(b0, space2) {
  uint  count;    // live particles, so the tail of a padded dispatch does nothing.
  float dt;       // seconds since the last step.
  float time;     // seconds since start, only used to vary the initial seeding.
  uint  seed;     // non-zero on the one seeding dispatch, zero every integrating one.
};

// A cheap integer hash (PCG-style) turned into a float in [0, 1). Enough randomness to scatter the initial
// particles without a random buffer to upload.
float hash(uint x) {
  x = x * 747796405u + 2891336453u;
  x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
  x = (x >> 22u) ^ x;
  return (float)x / 4294967296.0;
}

[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
  uint i = thread.x;
  if (i >= count) return;

  if (seed != 0u) {
    // Scatter across the field, heading in a random direction at a modest speed. The time only nudges the
    // hash so a re-seed after a resize does not land every particle where it was.
    float angle = hash(i * 3u + 0u) * 6.2831853 + time;
    float speed = 0.05 + hash(i * 3u + 1u) * 0.15;

    Particle p;
    p.position = float2(hash(i * 3u + 2u), hash(i * 7u + 5u));
    p.velocity = float2(cos(angle), sin(angle)) * speed;
    particles[i] = p;
    return;
  }

  Particle p = particles[i];
  p.position += p.velocity * dt;

  // Bounce off the walls of the unit square, so the field stays busy without ever emptying.
  if (p.position.x < 0.0) { p.position.x = -p.position.x;       p.velocity.x = -p.velocity.x; }
  if (p.position.x > 1.0) { p.position.x = 2.0 - p.position.x;  p.velocity.x = -p.velocity.x; }
  if (p.position.y < 0.0) { p.position.y = -p.position.y;       p.velocity.y = -p.velocity.y; }
  if (p.position.y > 1.0) { p.position.y = 2.0 - p.position.y;  p.velocity.y = -p.velocity.y; }

  particles[i] = p;
}
