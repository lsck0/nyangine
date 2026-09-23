// Vertex stage for wind-swayed foliage. See nya_render3d_foliage in src/nyangine/renderer/render3d.h.
//
// Like mesh3d_skinned.vert.hlsl, and for the same reason, the vertices here are in the plant's own
// *model* space rather than world space: every other 3D pipeline takes world-baked vertices because
// the batch folds the transform in on the CPU, but a swaying plant has to bend about its base, and
// the base is the model origin (y = 0). Baking the vertices to world space would throw away the one
// thing the sway needs — how high each vertex sits above the pivot. So the placement arrives as a
// model matrix (b1) and is applied here, and the bend is added before the view-projection.
//
// ## The pivot and the flexibility weight, under the fixed NYA_Vertex3D
//
// NYA_Vertex3D is frozen at 36 bytes (a static_assert guards it), so there is no room for a per-vertex
// stiffness attribute. The sway takes what it needs from what the vertex already carries: the pivot is
// the model origin (the base of the plant), the height above it is the model-space y, and the
// flexibility — 0 at the anchored base, 1 at the free tip — rides in the vertex colour's alpha, which
// nothing else in this pipeline reads. So a plant is authored once, with dark-to-light alpha up its
// height, and grass, a leaf and a branch differ only by the sway parameters in the uniform.
//
// ## ESSL-300 safe
//
// No Gather, no compute, and the uniform block is a float4x4 and four float4s — std140-expressible — so
// the build cross-compiles this to GLSL ES 300 for the web backend with no change. Keep it that way.

struct VertInput {
  float3 position : POSITION;
  float4 color : COLOR0;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
};

cbuffer Uniforms : register(b0, space1) {
  float4x4 view_projection;
};

// b1/space1, beside the view-projection — a vertex shader's uniforms are space1 under SDL_GPU; see the
// note in mesh3d_skinned.vert.hlsl. Matches NYA_ShaderFoliageUniform in uniforms.h.
#define FOLIAGE_DISTURBERS 4

cbuffer Foliage : register(b1, space1) {
  float4x4 model;

  // xyz: the wind's displacement/force at the plant, from nya_wind_sample. w: the field's time.
  float4 wind_time;

  // amplitude, frequency, stiffness, flutter. See NYA_Render3DFoliage.
  float4 sway;

  // detail_frequency, phase, height_scale, (pad).
  float4 detail;

  // Multiplied into the vertex colour, so one authored plant draws in many tints.
  float4 tint;

  // The nearest disturbers: xyz world position, w radius. The plant bends away from any it is inside.
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

VertOutput main(VertInput input) {
  VertOutput output;

  float amplitude    = sway.x;
  float frequency    = sway.y;
  float stiffness    = sway.z;
  float flutter      = sway.w;
  float detail_freq  = detail.x;
  float phase        = detail.y;
  float height_scale = detail.z;

  float time = wind_time.w;

  // The rest position in world space. The base (model origin) is the pivot the bend rotates about.
  float3 world = mul(model, float4(input.position, 1.0)).xyz;

  // How high this vertex sits above the pivot, normalised to [0, 1] so amplitude reads as a fraction of
  // the plant's height. Clamped so a vertex at or below the base cannot bend the wrong way.
  float height = saturate(max(input.position.y, 0.0) * height_scale);

  // A soft cantilever: the bend eases in from the base rather than growing straight from it, so a blade
  // curves instead of pivoting as a stiff rod. height^2 is the cheap smooth falloff.
  float height_curve = height * height;

  // 0 at the anchored base, 1 at the free tip.
  float flex = input.color.a;

  // A stiff plant barely moves: full response at stiffness 0, none at 1.
  float response = saturate(1.0 - stiffness);

  // How far this vertex is free to travel: grows with the eased height and with flexibility, which is
  // what anchors the bend at the base (height 0 or flex 0 -> no motion).
  float reach = height_curve * flex * response;

  // Per-instance variation from the object's phase, so identical plants neither sway in lockstep nor by
  // the same amount. A cheap hash into [0.75, 1.05] for amplitude and a whole extra phase turn.
  float variation   = frac(sin(phase) * 43758.5453);
  float amp_instance = amplitude * (0.75 + (0.3 * variation));

  float  wind_strength = length(wind_time.xyz);
  float3 wind_dir      = wind_strength > 1e-5 ? (wind_time.xyz / wind_strength) : float3(0.0, 0.0, 0.0);

  // Two bend octaves rather than one sine, so a field does not pulse as a single wave. The spatial term
  // travels the gust across the ground; `phase` offsets whole objects.
  float wave_phase = (time * frequency) + dot(world.xz, float2(0.35, 0.27)) + (phase * 6.2831853);
  float bend       = (0.7 * sin(wave_phase)) + (0.3 * sin((wave_phase * 1.9) + 1.3));

  // The steady lean plus the swing, both along the wind, scaled by reach and how hard it blows. The
  // 0.55/0.45 split keeps the plant leaning downwind at the swing's low point rather than snapping up.
  float3 offset = wind_dir * (reach * amp_instance * wind_strength * (0.55 + (0.45 * bend)));

  // High-frequency flutter, for leaves: a shimmer across two axes so a leaf twists rather than only
  // sliding, out of step per instance. Zero flutter (grass, branches) skips it.
  if (flutter > 0.0) {
    float shimmer_a = sin((time * detail_freq) + dot(world.xz, float2(1.7, 1.3)) + (phase * 12.0));
    float shimmer_b = sin((time * detail_freq * 1.37) + dot(world.xz, float2(-1.1, 2.3)) + (phase * 7.0));

    float3 side = normalize(cross(float3(0.0, 1.0, 0.0), wind_dir + float3(1e-4, 0.0, 0.0)));

    offset += side * (reach * flutter * shimmer_a);
    offset.y += reach * flutter * 0.35 * shimmer_b;
  }

  // Physics disturbers: bend away from each nearby body on top of the wind. A plain unrolled loop over a
  // fixed count, so it stays ESSL-300 safe. The push is horizontal (a parting on the ground), scaled by
  // the same reach as the wind and by a smooth distance falloff.
  int disturbers = (int)disturber_count.x;

  [unroll]
  for (int i = 0; i < FOLIAGE_DISTURBERS; i++) {
    if (i >= disturbers) break;

    float radius = disturber_position_radius[i].w;
    if (radius <= 0.0) continue;

    // horizontal separation from the disturber to this vertex, so a plant parts sideways, not upward.
    float3 away = world - disturber_position_radius[i].xyz;
    float  dist = length(float3(away.x, 0.0, away.z));

    if (dist >= radius) continue;

    float3 push_dir = dist > 1e-4 ? normalize(float3(away.x, 0.0, away.z)) : wind_dir;

    // smooth falloff (1 at the centre, 0 at the rim), squared for a soft edge.
    float falloff = 1.0 - (dist / radius);
    falloff = falloff * falloff;

    offset += push_dir * (reach * disturber_strength[i] * falloff);
  }

  float3 displaced = world + offset;

  output.position = mul(view_projection, float4(displaced, 1.0));

  // The base colour, tinted. The alpha comes from the tint, NOT the vertex: the vertex alpha is the
  // flexibility weight, already consumed above, and letting it through would fade every plant's anchored
  // base to nothing under the blended pipeline.
  output.color = float4(input.color.rgb * tint.rgb, tint.a);

  output.uv             = input.uv;
  output.world_position = displaced;

  // The normal, rotated into world space then leaned by the actual bend so lighting tracks the sway.
  // The surface tilts by roughly how fast the horizontal offset grows with height, so the lean is taken
  // straight from the offset we just applied rather than guessed. Renormalised, since leaning shortens it.
  float3 n    = normalize(mul((float3x3)model, input.normal));
  float3 lean = float3(offset.x, 0.0, offset.z) * (height_scale * 1.5);

  output.normal = normalize(n - lean);

  return output;
}
