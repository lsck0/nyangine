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

/** How much two normals turn past `crease_cosine`, full a little beyond it so a fold near the angle does not flicker. */
float scene_crease(float3 a, float3 b, float crease_cosine) {
  return 1.0 - smoothstep(crease_cosine - 0.25, crease_cosine, dot(a, b));
}

/**
 * How much ink covers `pixel`, in [0, 1].
 *
 * A silhouette is a neighbour much further away than the surface predicts, so the line lands on the nearer surface
 * only. The prediction carries on how the distance changes from the opposite neighbour to this pixel, straight in
 * inverse distance as a plane is, so a floor at a grazing angle climbs as fast as it likes without any edge, and one
 * turned past the horizon within the reach draws none. A pixel with nothing nearer behind it falls back to how
 * steeply its own normal is seen.
 *
 * A silhouette also has to hold seen back from beyond the far side: a surface bending further away climbs fast too,
 * but carried back toward this pixel it passes in front of it. Both predictions allow for facets in proportion to
 * how fast their side recedes, so a faceted slope seen at a grazing angle draws nothing.
 *
 * A crease is a turn past `crease_cosine` between the neighbours either side at half the reach, where both lie on
 * the surface through this pixel, and still there against the faces beyond them.
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

  float pixel_size = scene_pixel_size(view, distance);
  float margin = (silhouette_reach * pixel_size) + (distance * 0.002);

  // the depth a plane seen at this angle gains over the reach, doubled for slack.
  float steep_allowed = ((silhouette_reach * pixel_size) / facing) * 2.0 + (distance * 0.002);

  // opposite directions sit next to each other, so the one across from i is i ^ 1.
  const float2 directions[8] = {
    float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0),
    float2(0.7071, 0.7071), float2(-0.7071, -0.7071), float2(0.7071, -0.7071), float2(-0.7071, 0.7071),
  };

  float sides[8];

  [unroll]
  for (int i = 0; i < 8; i++) sides[i] = scene_texel(normals, normals_sampler, view, pixel, directions[i] * silhouette_reach).a;

  float ink = 0.0;

  [unroll]
  for (int i = 0; i < 8; i++) {
    float far_side = sides[i];
    float near_side = sides[i ^ 1];

    // nothing behind: always an edge.
    if (far_side <= 0.0) {
      ink = max(ink, smoothstep(steep_allowed, steep_allowed * 2.0, distance));
      continue;
    }

    float expected = distance;
    float allowed = steep_allowed;

    if (near_side > 0.0 && near_side < distance) {
      float inverse = (2.0 / distance) - (1.0 / near_side);

      if (inverse <= 0.0) continue;

      expected = 1.0 / inverse;

      // facets bend a receding surface a little, which counts for more the faster it recedes.
      allowed = ((expected - distance) * 3.0) + margin;
    }

    float edge = smoothstep(allowed, allowed * 2.0, far_side - expected);

    if (edge <= ink) continue;

    // from two samples wholly past the edge, since a resolved buffer blends the pixel on it into a ramp.
    float past = scene_texel(normals, normals_sampler, view, pixel, directions[i] * silhouette_reach * 2.0).a;
    float beyond = scene_texel(normals, normals_sampler, view, pixel, directions[i] * silhouette_reach * 3.0).a;

    if (past > 0.0 && beyond > past) {
      float inverse = (3.0 / past) - (2.0 / beyond);
      float slack = ((beyond - past) * 5.0) + margin;

      if (inverse > 0.0) edge *= smoothstep(slack, slack * 2.0, (1.0 / inverse) - distance);
    }

    ink = max(ink, edge);
  }

  // creases on the two axes only; the diagonals add cost and no line the axes miss.
  [unroll]
  for (int axis = 0; axis < 4; axis += 2) {
    float2 step = directions[axis] * crease_reach;

    float4 plus = scene_texel(normals, normals_sampler, view, pixel, step);
    float4 minus = scene_texel(normals, normals_sampler, view, pixel, -step);

    if (plus.a <= 0.0 || minus.a <= 0.0) continue;

    // both on this pixel's surface: halfway between them is about as far as this pixel.
    if (abs(((plus.a + minus.a) * 0.5) - distance) > abs(plus.a - minus.a) * 0.5 + margin) continue;

    float crease = scene_crease(plus.rgb, minus.rgb, crease_cosine);

    if (crease <= ink) continue;

    // the turn has to hold out to the face beyond on each side, so a sliver of ledge seen edge on, which turns back
    // within the reach, draws nothing rather than a dash.
    float3 plus_face = scene_texel(normals, normals_sampler, view, pixel, step * 2.0).rgb;
    float3 minus_face = scene_texel(normals, normals_sampler, view, pixel, -step * 2.0).rgb;

    crease = min(crease, min(scene_crease(plus_face, minus.rgb, crease_cosine), scene_crease(plus.rgb, minus_face, crease_cosine)));

    ink = max(ink, crease);
  }

  // under a pixel wide, the line fades rather than thins.
  return ink * saturate(reach) * (1.0 - scene_fog(view, position, distance));
}
