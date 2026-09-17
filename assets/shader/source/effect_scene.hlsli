// Shared by the post passes that read the scene normal buffer: rebuilding a world position from a texel, the fog a
// surface had, and where ink lands. See NYA_RENDER3D_NORMAL_FORMAT for what the buffer holds.

// Matches NYA_ShaderSceneView. A struct in a cbuffer starts on a row and this one fills five exactly.
struct SceneView {
  float3 right;
  float tangent;

  float3 up;
  float aspect;

  float3 forward;
  float half_height;

  float3 eye;
  float fog_density;

  float fog_height_falloff;
  float fog_height_base;
  float2 texel;
};

struct FragInput {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

/** The procedural vertex stage's uv has v growing up; textures grow down. */
float2 scene_uv(FragInput input) {
  return float2(input.uv.x, 1.0 - input.uv.y);
}

/** The world point a texel at `uv` shows, `distance` from the eye. */
float3 scene_position(SceneView view, float2 uv, float distance) {
  float2 ndc = float2((uv.x * 2.0) - 1.0, 1.0 - (uv.y * 2.0));

  if (view.half_height > 0.0) {
    float3 offset = (view.right * (ndc.x * view.half_height * view.aspect)) + (view.up * (ndc.y * view.half_height));

    // the buffer holds distance from the eye, and orthographic rays start beside it.
    float along = sqrt(max((distance * distance) - dot(offset, offset), 0.0));

    return view.eye + offset + (view.forward * along);
  }

  float3 ray = view.forward + (view.right * (ndc.x * view.tangent * view.aspect)) + (view.up * (ndc.y * view.tangent));

  return view.eye + (normalize(ray) * distance);
}

/** How wide one pixel of the normal buffer is in world units at `distance`. */
float scene_pixel_size(SceneView view, float distance) {
  float half_extent = view.half_height > 0.0 ? view.half_height : view.tangent * distance;

  return 2.0 * half_extent * view.texel.y;
}

/** How much fog covers a surface, in [0, 1]. The amount mesh3d_fog blends by, without its colour. */
float scene_fog(SceneView view, float3 position, float distance) {
  if (view.fog_density <= 0.0) return 0.0;

  float above = max(position.y - view.fog_height_base, 0.0);

  return saturate(1.0 - exp(-view.fog_density * distance * exp(-view.fog_height_falloff * above)));
}

/** The normal buffer texel `offset` pixels from `pixel`, read at its centre so the linear sampler blends nothing. */
float4 scene_texel(Texture2D buffer, SamplerState buffer_sampler, SceneView view, float2 pixel, float2 offset) {
  return buffer.SampleLevel(buffer_sampler, (pixel + offset) * view.texel, 0.0);
}

/**
 * How much ink covers `pixel`, in [0, 1].
 *
 * A silhouette is a neighbour much further away, so the line lands on the nearer surface only. How much further
 * counts depends on how steeply the surface is seen: a floor at a grazing angle climbs fast from pixel to pixel
 * without any edge. A crease is a neighbour at about the same depth whose normal has turned past `crease_cosine`,
 * sampled at half the reach since both faces draw it.
 *
 * Width and strength fall off between the fade distances, so far lines thin to a pixel and then disappear, and fog
 * takes what is left.
 * */
float scene_ink(
  Texture2D normals,
  SamplerState normals_sampler,
  SceneView view,
  float2 pixel,
  float width,
  float crease_cosine,
  float fade_start,
  float fade_end
) {
  float4 centre = scene_texel(normals, normals_sampler, view, pixel, float2(0.0, 0.0));

  float distance = centre.a;

  if (distance <= 0.0) return 0.0;

  float reach = width * (1.0 - smoothstep(fade_start, fade_end, distance));

  if (reach <= 0.05) return 0.0;

  float3 normal = centre.rgb;
  float3 position = scene_position(view, pixel * view.texel, distance);
  float facing = max(abs(dot(normal, normalize(view.eye - position))), 0.2);

  float silhouette_reach = max(reach, 1.0);
  float crease_reach = max(reach * 0.5, 1.0);

  // the depth a plane seen at this angle gains over the reach, doubled for slack.
  float allowed = ((silhouette_reach * scene_pixel_size(view, distance)) / facing) * 2.0 + (distance * 0.002);

  const float2 directions[8] = {
    float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0),
    float2(0.7071, 0.7071), float2(-0.7071, 0.7071), float2(0.7071, -0.7071), float2(-0.7071, -0.7071),
  };

  float ink = 0.0;

  [unroll]
  for (int i = 0; i < 8; i++) {
    float4 far_side = scene_texel(normals, normals_sampler, view, pixel, directions[i] * silhouette_reach);

    float jump = far_side.a <= 0.0 ? distance : far_side.a - distance;

    ink = max(ink, smoothstep(allowed, allowed * 2.0, jump));

    // creases on the four axes only; the diagonals add cost and no line the axes miss.
    if (i >= 4) continue;

    float4 beside = scene_texel(normals, normals_sampler, view, pixel, directions[i] * crease_reach);

    if (beside.a <= 0.0 || abs(beside.a - distance) > allowed) continue;

    ink = max(ink, 1.0 - smoothstep(crease_cosine - 0.05, crease_cosine + 0.05, dot(normal, beside.rgb)));
  }

  // under a pixel wide, the line fades rather than thins.
  return ink * saturate(reach) * (1.0 - scene_fog(view, position, distance));
}
