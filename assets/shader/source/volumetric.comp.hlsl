// A raymarched volumetric: fog, cloud and smoke drawn by marching a procedural 3D density field. One thread
// per output texel casts a ray into a box of noise, steps along it accumulating extinction (Beer-Lambert) and
// in-scattered light from a single directional source, and writes the lit colour with the volume's coverage in
// alpha. render_compute_volumetric.c then composites that texture over the scene with the ordinary 2D path, so
// nothing here reads back to the CPU.
//
// The density is analytic — a fractal sum of value noise drifting with time — rather than a sampled 3D texture,
// so the effect owns no volume asset and the whole field lives in the shader. It is the compute sibling of the
// particle field: the same pipeline skin (render_compute.h), the same desktop-only gate. WebGL2/GLES3 has no
// compute stage, so the build skips this shader's web variant; see render_compute.h and src/nyangine-build/pp/asset.c.
//
// SDL_GPU's compute binding model, matching NYA_GPUComputePipelineDesc on the C side: a read-write storage
// texture at (u0, space1) and a uniform buffer at (b0, space2). See SDL_CreateGPUComputePipeline for the order.

// The image the volume is drawn into. Created with COMPUTE_STORAGE_WRITE and SAMPLER so the 2D path can draw it.
RWTexture2D<float4> image : register(u0, space1);

// space2 is the compute stage's uniform space; b0 is the slot nya_gpu_compute_dispatch pushes to. Laid out in
// 16-byte groups so the packing matches _NYA_GPUVolumetricUniform byte for byte, light_direction in a float4.
cbuffer Params : register(b0, space2) {
  float4 light_direction; // xyz: which way the sun's light travels, world space, normalised. w unused.
  float  density;         // overall opacity of the field, scales the sampled extinction.
  float  absorption;      // how fast light is swallowed along the light march, the Beer-Lambert coefficient.
  uint   steps;           // primary march steps along the view ray, clamped to VOLUMETRIC_STEPS_MAX.
  float  time;            // seconds since start, so the field drifts.
  uint   width;           // image size in texels, so the tail of a padded dispatch does nothing.
  uint   height;
  uint2  _pad;
};

// The most steps either march takes, matching NYA_VOLUMETRIC_STEPS_MAX on the C side.
#define VOLUMETRIC_STEPS_MAX 128u

// Steps of the shadow march toward the light per primary sample. Short: enough for a soft self-shadow, cheap
// enough to run inside the primary loop.
#define VOLUMETRIC_LIGHT_STEPS 6u

// A cheap integer hash (PCG-style) turned into a float in [0, 1), the same one the particle shaders use.
float hash(uint x) {
  x = x * 747796405u + 2891336453u;
  x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
  x = (x >> 22u) ^ x;
  return (float)x / 4294967296.0;
}

// Value noise at a 3D point: hash the eight lattice corners and trilinearly blend them on a smoothstep curve.
float value_noise(float3 p) {
  float3 i = floor(p);
  float3 f = frac(p);
  float3 u = f * f * (3.0 - 2.0 * f);

  // Fold the integer lattice coordinate into one hash input; the large odd multipliers keep neighbours apart.
  uint3  gi = (uint3)(int3)i;
  #define CORNER(dx, dy, dz) hash((gi.x + dx) * 1973u + (gi.y + dy) * 9277u + (gi.z + dz) * 26699u)

  float c000 = CORNER(0u, 0u, 0u), c100 = CORNER(1u, 0u, 0u);
  float c010 = CORNER(0u, 1u, 0u), c110 = CORNER(1u, 1u, 0u);
  float c001 = CORNER(0u, 0u, 1u), c101 = CORNER(1u, 0u, 1u);
  float c011 = CORNER(0u, 1u, 1u), c111 = CORNER(1u, 1u, 1u);
  #undef CORNER

  float x00 = lerp(c000, c100, u.x), x10 = lerp(c010, c110, u.x);
  float x01 = lerp(c001, c101, u.x), x11 = lerp(c011, c111, u.x);
  return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

// Fractal Brownian motion: a few octaves of value noise, each finer and fainter, drifting with time. Returns
// the local density in [0, 1] before the density scale.
float fbm(float3 p) {
  p += float3(0.0, time * 0.15, time * 0.05); // the field rolls upward and drifts, so it reads as moving smoke.

  float sum = 0.0;
  float amplitude = 0.5;
  for (uint octave = 0u; octave < 4u; octave++) {
    sum += amplitude * value_noise(p);
    p *= 2.02;
    amplitude *= 0.5;
  }
  return sum;
}

// The density of the volume at a world point: the fractal field, faded out toward the top and the edges of the
// box so the cloud has a shape rather than filling the frame, then scaled by the density parameter.
float sample_density(float3 p) {
  float base = fbm(p * 1.6);

  // A soft ceiling and floor so the body sits in a slab, and a radial fade so it does not touch the frame edge.
  float slab   = smoothstep(1.2, 0.2, abs(p.y));
  float radial = smoothstep(1.6, 0.4, length(p.xz));

  float d = (base - 0.35) * slab * radial; // subtract a threshold so the thin fringes clear to empty.
  return max(d, 0.0) * density;
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
  if (thread.x >= width || thread.y >= height) return;

  // This texel in [-1, 1] with a corrected aspect, the plane the view ray is cast through.
  float2 uv     = (float2(thread.xy) + 0.5) / float2((float)width, (float)height);
  float2 screen = (uv * 2.0 - 1.0) * float2(1.0, -1.0);
  float  aspect = (float)width / (float)height;
  screen.x *= aspect;

  // A fixed virtual camera looking down -z at the fog box centred on the origin.
  float3 origin    = float3(0.0, 0.0, 3.0);
  float3 direction = normalize(float3(screen * 0.6, -1.0));
  float3 light      = normalize(light_direction.xyz);

  uint march_steps = min(max(steps, 1u), VOLUMETRIC_STEPS_MAX);
  float step_size  = 4.0 / (float)march_steps; // the box is ~4 units deep along the ray.

  // A blue-dawn sky the volume is lit and composited against, and a warm sun colour for the scattered light.
  float3 sky_color = float3(0.42, 0.54, 0.72);
  float3 sun_color = float3(1.0, 0.86, 0.62);

  float  transmittance = 1.0;         // how much background still shows through; starts fully clear.
  float3 scattered     = float3(0.0, 0.0, 0.0);

  // Dither the ray's start by a per-texel hash so the fixed step count does not band.
  float jitter = hash(thread.x * 1973u + thread.y * 9277u) * step_size;

  for (uint i = 0u; i < march_steps; i++) {
    float3 position = origin + direction * (0.5 + jitter + (float)i * step_size);

    float sample = sample_density(position);
    if (sample <= 0.001) continue;

    // A short march toward the light estimates how much of the volume shadows this sample: Beer-Lambert over
    // the density between here and the light.
    float light_density = 0.0;
    for (uint j = 0u; j < VOLUMETRIC_LIGHT_STEPS; j++) {
      float3 light_position = position + light * (-((float)j + 1.0) * step_size);
      light_density += sample_density(light_position);
    }
    float light_transmittance = exp(-light_density * absorption * step_size);

    // The extinction this step removes, and the light it scatters back toward the camera, composited front to
    // back so a near sample occludes a far one.
    float extinction = sample * step_size;
    float3 luminance = (sun_color * light_transmittance) + (sky_color * 0.3);

    scattered     += transmittance * extinction * luminance;
    transmittance *= exp(-extinction);

    if (transmittance < 0.01) break; // fully opaque from here on; the rest is hidden.
  }

  // Coverage in alpha, and the scattered light un-premultiplied back to a straight colour, so the ordinary 2D
  // alpha blend composites the volume over the scene by its opacity rather than darkening it twice.
  float  coverage = 1.0 - transmittance;
  float3 color    = coverage > 0.001 ? scattered / coverage : float3(0.0, 0.0, 0.0);
  image[thread.xy] = float4(color, coverage);
}
