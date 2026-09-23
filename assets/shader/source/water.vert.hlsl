// Vertex stage for a flowing water surface. See nya_render3d_water in src/nyangine/renderer/render3d.h.
//
// Like foliage.vert.hlsl, the vertices arrive in the surface's own *model* space and are placed by a model
// matrix here, not baked to world on the CPU: the waves have to lift a vertex about the still water plane,
// which is the model's y = 0, so the height above that plane has to survive to the shader. The still surface
// is a flat strip the caller lays along the riverbed; this stage lifts it into travelling waves.
//
// ## The waves, mirrored from render_water.c
//
// The height is the same summed sines nya_water_wave_height computes on the CPU (two octaves along the
// current plus one chop across it), so the renderer's cull-radius padding and the determinism test describe
// the surface this actually draws. The normal is the analytic gradient of that height, so lighting and the
// Fresnel term track the real slope rather than a flat plane. A little Gerstner-style horizontal pinch along
// the flow sharpens the crests. The wind field (render_wind.h) optionally drives the chop: its horizontal
// push speeds the travel and lifts the amplitude, so water, foliage and particles all lean on one wind.
//
// ## ESSL-300 safe
//
// No Gather, no compute, and the uniform block is a float4x4 and three float4s — std140-expressible — so the
// build cross-compiles this to GLSL ES 300 for the web backend unchanged. Keep it that way.

struct VertInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
};

cbuffer Uniforms : register(b0, space1) {
  float4x4 view_projection;
};

// b1/space1, beside the view-projection — a vertex shader's uniforms are space1 under SDL_GPU. Matches
// NYA_ShaderWaterVertexUniform in uniforms.h.
cbuffer Water : register(b1, space1) {
  float4x4 model;

  // flow_dir.x, flow_dir.z (the current's heading on the ground), flow_speed, time.
  float4 flow_time;

  // wave amplitude, frequency, wave travel speed, choppiness (the Gerstner pinch, 0..1).
  float4 wave;

  // wind.x, wind.z (the wind's horizontal push, from nya_wind_sample), wind_influence (0 ignores it), pad.
  float4 wind;
};

struct VertOutput {
  float4 position : SV_POSITION;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
  float3 world_position : TEXCOORD1;

  // x: the shore weight the mesh authored in vertex-colour alpha (0 mid-channel, 1 at the banks). y: the
  // signed crest height in [-1, 1], so the fragment stage can foam the wave tops. See water.frag.hlsl.
  float2 shore_crest : TEXCOORD2;
};

/** The two along-flow octave weights and the cross chop, matching _NYA_WATER_OCTAVE and _NYA_WATER_CHOP. */
static const float2 WATER_OCTAVE = float2(0.60, 0.40);
static const float WATER_CHOP = 0.35;

/** Their sum, NYA_WATER_WAVE_PEAK: the most the height reaches as a multiple of amplitude, for the crest normalise. */
static const float WATER_WAVE_PEAK = 1.35;

VertOutput main(VertInput input) {
  VertOutput output;

  float amplitude  = wave.x;
  float frequency  = wave.y;
  float wave_speed = wave.z;
  float choppiness = wave.w;

  float time = flow_time.w;

  // the still-water point in world space; the model origin's plane (y = 0) is what the waves lift about.
  float3 world = mul(model, float4(input.position, 1.0)).xyz;

  // the current's heading on the ground, and a horizontal perpendicular for the cross chop. A zero flow is
  // read as +x, the same fallback the CPU mirror uses.
  float2 flow = flow_time.xy;
  if (dot(flow, flow) < 1e-8) flow = float2(1.0, 0.0);
  flow = normalize(flow);
  float2 across = float2(-flow.y, flow.x);

  // the wind's push along and across the current. Its along-flow part hurries the travel and lifts the
  // amplitude; wind_influence at zero leaves the water on its own steady flow.
  float2 wind_xz = wind.xy;
  float wind_influence = wind.z;
  float along_wind = dot(wind_xz, flow) * wind_influence;

  float speed = wave_speed + (along_wind * 0.15);
  float amp   = amplitude * (1.0 + saturate(length(wind_xz) * wind_influence * 0.1));

  // how far along the current and across it this point sits: the phase the travelling sines run through.
  float along = dot(world.xz, flow);
  float side  = dot(world.xz, across);

  // two along-flow octaves and one cross chop, exactly the phases render_water.c sums.
  float a0 = (along * frequency) + (time * speed);
  float a1 = (along * frequency * 1.7) + (time * speed * 1.3) + 1.3;
  float a2 = (side * frequency * 0.8) + (time * speed * 1.9);

  float height = amp * ((WATER_OCTAVE.x * sin(a0)) + (WATER_OCTAVE.y * sin(a1)) + (WATER_CHOP * sin(a2)));

  // the analytic slope of that height, so the normal is the real surface slope, not the flat plane's.
  float dalong = amp * ((WATER_OCTAVE.x * frequency * cos(a0)) + (WATER_OCTAVE.y * frequency * 1.7 * cos(a1)));
  float dside  = amp * (WATER_CHOP * frequency * 0.8 * cos(a2));

  // chain the along/side derivatives back onto world x and z (along = world.xz . flow, side = world.xz . across).
  float dhdx = (dalong * flow.x) + (dside * across.x);
  float dhdz = (dalong * flow.y) + (dside * across.y);

  // a Gerstner-style pinch: pull the surface back toward the troughs along the flow so crests sharpen rather
  // than staying round. Scaled by choppiness and kept under the amplitude so the mesh never folds over itself.
  float pinch = choppiness * amp * cos(a0);
  float3 displaced = world + float3(-flow.x * pinch, height, -flow.y * pinch);

  output.position = mul(view_projection, float4(displaced, 1.0));
  output.world_position = displaced;
  output.uv = input.uv;

  // the surface normal from the height gradient, world y up. Renormalised, since a steep slope lengthens it.
  output.normal = normalize(float3(-dhdx, 1.0, -dhdz));

  // the shore weight rides in the authored vertex alpha; the crest is the height as a fraction of its peak.
  float crest = amp > 1e-5 ? (height / (amp * WATER_WAVE_PEAK)) : 0.0;
  output.shore_crest = float2(input.color.a, crest);

  return output;
}
