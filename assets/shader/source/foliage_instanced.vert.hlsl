// Vertex stage for instanced wind-swayed foliage: a field of grass blades drawn in one instanced call.
// See nya_render3d_grass in src/nyangine/renderer/render3d.h.
//
// This is foliage.vert.hlsl and mesh3d_instanced.vert.hlsl fused. From foliage.vert it takes the whole
// bend: model-space vertices curved about their base (the model origin, y = 0), the flexibility weight in
// the vertex colour's alpha, the two-octave wind wave and the physics disturbers. From
// mesh3d_instanced.vert it takes the per-instance model matrix, read as four column attributes from
// buffer 1, so one upload of a blade mesh is drawn at every placement in the field from a single draw.
//
// The wind, the sway parameters and the disturbers are shared by the whole patch and arrive in the b1
// uniform (NYA_ShaderFoliageUniform), exactly the block the scalar path fills. Its `model` and `phase`
// fields go unread here: the placement is per instance now, and the phase is derived below from each
// blade's own world position — the same hash the scalar path uses on the CPU when a caller leaves phase
// unset, so a field of identical blades neither sways in lockstep nor is billed a per-instance attribute.
//
// ## ESSL-300 safe
//
// Same shape as foliage.vert: no Gather, no compute, an unrolled fixed loop, and the uniform block is a
// float4x4 and float4s (std140-expressible). Per-instance vertex attributes cross-compile to
// glVertexAttribDivisor under GLSL ES 300. Keep it that way so the web backend builds it unchanged.

struct VertInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;

  // Buffer 1, per instance. Matches NYA_Render3DInstance, the same layout mesh3d_instanced.vert reads:
  // the model matrix's four columns (the engine's matrices are column-major) and a tint.
  float4 model_column_0 : TEXCOORD1;
  float4 model_column_1 : TEXCOORD2;
  float4 model_column_2 : TEXCOORD3;
  float4 model_column_3 : TEXCOORD4;

  float4 tint : TEXCOORD5;
};

cbuffer Uniforms : register(b0, space1) {
  float4x4 view_projection;
};

#define FOLIAGE_DISTURBERS 4

// b1/space1, beside the view-projection. Matches NYA_ShaderFoliageUniform in uniforms.h, byte for byte,
// so the CPU fills one struct for both foliage paths; `model` and `detail.y` (phase) are unused here.
cbuffer Foliage : register(b1, space1) {
  float4x4 model_unused;

  // xyz: the wind's displacement/force at the patch, from nya_wind_sample. w: the field's time.
  float4 wind_time;

  // amplitude, frequency, stiffness, flutter. See NYA_Render3DFoliage.
  float4 sway;

  // detail_frequency, phase (unused: per-instance below), height_scale, (pad).
  float4 detail;

  // Multiplied into the vertex colour on top of the per-instance tint, so the whole patch can be shaded.
  float4 tint;

  // The nearest disturbers to the patch: xyz world position, w radius. Blades bend away from any they sit inside.
  float4 disturber_position_radius[FOLIAGE_DISTURBERS];

  // One strength per disturber, packed into the four channels.
  float4 disturber_strength;

  // x: how many disturbers are live.
  float4 disturber_count;
};

struct VertOutput {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
  float3 world_position : TEXCOORD1;
};

// Model space to world for a column-major matrix, written out rather than assembled into a float4x4:
// `c0*p.x + c1*p.y + c2*p.z + c3` is the matrix-vector product by definition, and dodges HLSL's
// row-taking float4x4 constructor that would silently transpose the transform. See mesh3d_instanced.vert.
float3 model_to_world(VertInput input, float3 point_in_model) {
  float4 world = (input.model_column_0 * point_in_model.x) + (input.model_column_1 * point_in_model.y) +
                 (input.model_column_2 * point_in_model.z) + input.model_column_3;

  return world.xyz;
}

VertOutput main(VertInput input) {
  VertOutput output;

  float amplitude    = sway.x;
  float frequency    = sway.y;
  float stiffness    = sway.z;
  float flutter      = sway.w;
  float detail_freq  = detail.x;
  float height_scale = detail.z;

  float time = wind_time.w;

  // The rest position in world space, and the blade's base (its model origin) the bend rotates about.
  float3 world = model_to_world(input, input.position);
  float3 base  = input.model_column_3.xyz;

  // The per-instance phase, from the blade's own world base — the same hash nya_render3d_foliage runs on
  // the CPU when a caller leaves phase unset. This is what makes each instance's sway its own.
  float phase = (base.x * 12.9898) + (base.z * 78.233);

  // How high this vertex sits above the base, normalised to [0, 1] so amplitude reads as a fraction of the
  // blade's height. Clamped so a vertex at or below the base cannot bend the wrong way.
  float height = saturate(max(input.position.y, 0.0) * height_scale);

  // A soft cantilever: the bend eases in from the base, so a blade curves rather than pivoting as a rod.
  float height_curve = height * height;

  // 0 at the anchored base, 1 at the free tip.
  float flex = input.color.a;

  // A stiff blade barely moves: full response at stiffness 0, none at 1.
  float response = saturate(1.0 - stiffness);

  // How far this vertex is free to travel: grows with the eased height and with flexibility.
  float reach = height_curve * flex * response;

  // Per-instance variation, so identical blades neither sway in lockstep nor by the same amount.
  float variation    = frac(sin(phase) * 43758.5453);
  float amp_instance = amplitude * (0.75 + (0.3 * variation));

  float  wind_strength = length(wind_time.xyz);
  float3 wind_dir      = wind_strength > 1e-5 ? (wind_time.xyz / wind_strength) : float3(0.0, 0.0, 0.0);

  // Two bend octaves rather than one sine, so a field does not pulse as a single wave. The spatial term
  // travels the gust across the ground; `phase` offsets whole blades.
  float wave_phase = (time * frequency) + dot(world.xz, float2(0.35, 0.27)) + (phase * 6.2831853);
  float bend       = (0.7 * sin(wave_phase)) + (0.3 * sin((wave_phase * 1.9) + 1.3));

  // The steady lean plus the swing, both along the wind, scaled by reach and how hard it blows.
  float3 offset = wind_dir * (reach * amp_instance * wind_strength * (0.55 + (0.45 * bend)));

  // High-frequency flutter, for leaves: a shimmer across two axes, out of step per instance. Zero skips it.
  if (flutter > 0.0) {
    float shimmer_a = sin((time * detail_freq) + dot(world.xz, float2(1.7, 1.3)) + (phase * 12.0));
    float shimmer_b = sin((time * detail_freq * 1.37) + dot(world.xz, float2(-1.1, 2.3)) + (phase * 7.0));

    float3 side = normalize(cross(float3(0.0, 1.0, 0.0), wind_dir + float3(1e-4, 0.0, 0.0)));

    offset += side * (reach * flutter * shimmer_a);
    offset.y += reach * flutter * 0.35 * shimmer_b;
  }

  // Physics disturbers: bend away from each nearby body on top of the wind. A plain unrolled loop over a
  // fixed count, so it stays ESSL-300 safe. The push is horizontal, scaled by reach and a smooth falloff.
  int disturbers = (int)disturber_count.x;

  [unroll]
  for (int i = 0; i < FOLIAGE_DISTURBERS; i++) {
    if (i >= disturbers) break;

    float radius = disturber_position_radius[i].w;
    if (radius <= 0.0) continue;

    float3 away = world - disturber_position_radius[i].xyz;
    float  dist = length(float3(away.x, 0.0, away.z));

    if (dist >= radius) continue;

    float3 push_dir = dist > 1e-4 ? normalize(float3(away.x, 0.0, away.z)) : wind_dir;

    float falloff = 1.0 - (dist / radius);
    falloff = falloff * falloff;

    offset += push_dir * (reach * disturber_strength[i] * falloff);
  }

  float3 displaced = world + offset;

  output.position = mul(view_projection, float4(displaced, 1.0));

  // The base colour, multiplied by the per-instance tint and then the shared patch tint. The alpha comes
  // from the tints, NOT the vertex: the vertex alpha is the flexibility weight, already consumed above.
  output.color = float4(input.color.rgb * input.tint.rgb * tint.rgb, input.tint.a * tint.a);

  output.uv             = input.uv;
  output.world_position = displaced;

  // The normal, rotated into world space by the model columns (translation left out so it stays a
  // direction), then leaned by the bend so lighting tracks the sway. Renormalised, since both shorten it.
  float3 n = (input.model_column_0.xyz * input.normal.x) + (input.model_column_1.xyz * input.normal.y) +
             (input.model_column_2.xyz * input.normal.z);
  n = normalize(n);

  float3 lean = float3(offset.x, 0.0, offset.z) * (height_scale * 1.5);

  output.normal = normalize(n - lean);

  return output;
}
