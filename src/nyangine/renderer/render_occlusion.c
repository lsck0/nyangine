#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNALS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A world point after projection: pixel coordinates and normalised depth, or `behind` if unusable. */
typedef struct {
    f32 x;
    f32 y;
    f32 depth;
    b8  behind;
} _NYA_OcclusionPoint;

/**
 * World space to buffer pixels.
 *
 * `behind` covers everything at or past the eye plane, where the projective divide is meaningless and
 * the result would be a point mirrored through the camera. Every caller treats it as "give up", which
 * is the conservative direction for both halves of this file: an occluder that straddles the near
 * plane is dropped, and a query that does is answered visible.
 * */
NYA_INTERNAL _NYA_OcclusionPoint _nya_occlusion_project(const NYA_OcclusionBuffer* buffer, f32x3 point) {
    f32x4 clip = nya_matrix_times_vector(buffer->view_projection, (f32x4){ point.x, point.y, point.z, 1.0F });

    if (clip.w <= NYA_EPSILON) return (_NYA_OcclusionPoint){ .behind = true };

    f32 inverse_w = 1.0F / clip.w;

    // The y flip is the usual one: normalised device space has y up, a buffer has row zero at the top.
    return (_NYA_OcclusionPoint){
        .x     = ((clip.x * inverse_w) * 0.5F + 0.5F) * (f32)NYA_OCCLUSION_WIDTH,
        .y     = (0.5F - (clip.y * inverse_w) * 0.5F) * (f32)NYA_OCCLUSION_HEIGHT,
        .depth = clip.z * inverse_w,
    };
}

/** Twice the signed area of the triangle, which is also the edge function's sign convention. */
NYA_INTERNAL f32 _nya_occlusion_edge(f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py) {
    return ((bx - ax) * (py - ay)) - ((by - ay) * (px - ax));
}

/**
 * Rasterises one convex polygon, writing the depth of every pixel it *fully* covers.
 *
 * Full coverage rather than the usual pixel-centre rule, and that is the whole difference between a
 * conservative occlusion buffer and a rendering one. A half covered pixel is half a claim, and half a
 * claim used as a whole one hides geometry visible through the gap. So all four corners of the pixel
 * have to be inside every edge.
 *
 * A polygon and not a pair of triangles, and that is a consequence of the rule above rather than a
 * convenience. Split a quad along its diagonal and the pixels the diagonal passes through are fully
 * covered by neither half, so the buffer comes out with a one pixel slit down the middle of every
 * wall — and one unwritten pixel anywhere in a query's rectangle is enough to answer "visible". The
 * first version of this file did exactly that and occluded nothing at all.
 *
 * The depth written is the *largest* of the four pixel corners' — the farthest the occluder gets
 * anywhere in that pixel — for the same conservative reason. Depth is affine in screen space after
 * the divide (which is what a hardware depth buffer relies on too), so three of the polygon's
 * vertices determine it everywhere, including outside the triangle they form, and evaluating it at
 * the pixel's corners bounds it over the square.
 * */
NYA_INTERNAL b8 _nya_occlusion_convex(NYA_OcclusionBuffer* buffer, _NYA_OcclusionPoint* points, u32 count) {
    if (count < 3) return false;

    // Shoelace, to find out which way it was wound. Either is accepted — a caller submitting a wall
    // does not know which side of it the camera ended up on — by reversing rather than rejecting.
    f32 area = 0.0F;
    for (u32 i = 0; i < count; i++) {
        u32 next = (i + 1) % count;
        area += (points[i].x * points[next].y) - (points[next].x * points[i].y);
    }

    if (area < 0.0F) {
        for (u32 i = 0; i < count / 2; i++) {
            _NYA_OcclusionPoint swap  = points[i];
            points[i]                 = points[count - 1 - i];
            points[count - 1 - i]     = swap;
        }
    }

    /*
     * Three vertices that actually span a triangle, for the depth plane.
     *
     * Not always the first three: a quad may have a degenerate corner while still enclosing area, and
     * a zero area triple would divide the interpolation by nothing. Whichever triple is found spans
     * the same plane as any other, because the polygon is planar.
     */
    u32 a = 0;
    u32 b = 0;
    u32 c = 0;
    f32 plane_area = 0.0F;

    for (u32 i = 1; i + 1 < count && plane_area == 0.0F; i++) {
        f32 candidate = _nya_occlusion_edge(points[0].x, points[0].y, points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);

        if (fabsf(candidate) <= NYA_EPSILON) continue;

        a          = 0;
        b          = i;
        c          = i + 1;
        plane_area = candidate;
    }

    if (plane_area == 0.0F) return false;

    f32 minimum_x = points[0].x;
    f32 maximum_x = points[0].x;
    f32 minimum_y = points[0].y;
    f32 maximum_y = points[0].y;

    for (u32 i = 1; i < count; i++) {
        minimum_x = nya_min(minimum_x, points[i].x);
        maximum_x = nya_max(maximum_x, points[i].x);
        minimum_y = nya_min(minimum_y, points[i].y);
        maximum_y = nya_max(maximum_y, points[i].y);
    }

    s32 left   = (s32)floorf(minimum_x);
    s32 right  = (s32)ceilf(maximum_x);
    s32 top    = (s32)floorf(minimum_y);
    s32 bottom = (s32)ceilf(maximum_y);

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > NYA_OCCLUSION_WIDTH) right = NYA_OCCLUSION_WIDTH;
    if (bottom > NYA_OCCLUSION_HEIGHT) bottom = NYA_OCCLUSION_HEIGHT;

    if (left >= right || top >= bottom) return false;

    f32 inverse_plane_area = 1.0F / plane_area;
    b8  wrote              = false;

    for (s32 y = top; y < bottom; y++) {
        for (s32 x = left; x < right; x++) {
            f32 corner_x[4] = { (f32)x, (f32)(x + 1), (f32)x, (f32)(x + 1) };
            f32 corner_y[4] = { (f32)y, (f32)y, (f32)(y + 1), (f32)(y + 1) };

            f32 farthest = 0.0F;
            b8  covered  = true;

            for (u32 corner = 0; corner < 4 && covered; corner++) {
                for (u32 edge = 0; edge < count; edge++) {
                    u32 next = (edge + 1) % count;

                    if (_nya_occlusion_edge(points[edge].x, points[edge].y, points[next].x, points[next].y, corner_x[corner], corner_y[corner]) <
                        0.0F) {
                        covered = false;
                        break;
                    }
                }

                if (!covered) break;

                f32 weight_a = _nya_occlusion_edge(points[b].x, points[b].y, points[c].x, points[c].y, corner_x[corner], corner_y[corner]);
                f32 weight_b = _nya_occlusion_edge(points[c].x, points[c].y, points[a].x, points[a].y, corner_x[corner], corner_y[corner]);
                f32 weight_c = _nya_occlusion_edge(points[a].x, points[a].y, points[b].x, points[b].y, corner_x[corner], corner_y[corner]);

                f32 depth = ((weight_a * points[a].depth) + (weight_b * points[b].depth) + (weight_c * points[c].depth)) * inverse_plane_area;

                if (corner == 0 || depth > farthest) farthest = depth;
            }

            if (!covered) continue;

            f32* slot = &buffer->depth[((u64)y * NYA_OCCLUSION_WIDTH) + (u64)x];

            // The nearest claim wins. See NYA_OcclusionBuffer.depth.
            if (farthest < *slot) *slot = farthest;

            wrote = true;
        }
    }

    return wrote;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void nya_occlusion_begin(NYA_OcclusionBuffer* buffer, f32_4x4 view_projection) {
    if (buffer == nullptr) return;

    for (u64 i = 0; i < NYA_OCCLUSION_WIDTH * NYA_OCCLUSION_HEIGHT; i++) buffer->depth[i] = 1.0F;

    buffer->view_projection = view_projection;
    buffer->ready           = true;
    buffer->stats           = (NYA_OcclusionStats){ 0 };
}

b8 nya_occlusion_quad(NYA_OcclusionBuffer* buffer, f32x3 a, f32x3 b, f32x3 c, f32x3 d) {
    if (buffer == nullptr || !buffer->ready) return false;

    buffer->stats.occluders++;

    _NYA_OcclusionPoint points[4] = {
        _nya_occlusion_project(buffer, a),
        _nya_occlusion_project(buffer, b),
        _nya_occlusion_project(buffer, c),
        _nya_occlusion_project(buffer, d),
    };

    /*
     * One vertex behind the eye drops the whole quad, rather than clipping it.
     *
     * Clipping a polygon against the near plane is a page of code that exists to recover an occluder
     * the camera is standing inside — which is the case where it occludes the least anyway, because
     * whatever is behind it is off screen too. Dropping it costs nothing real and keeps the only
     * failure mode in this file "hides less than it could".
     */
    for (u32 i = 0; i < 4; i++) {
        if (points[i].behind) return false;
    }

    if (!_nya_occlusion_convex(buffer, points, 4)) return false;

    buffer->stats.occluders_rasterized++;
    return true;
}

u32 nya_occlusion_box(NYA_OcclusionBuffer* buffer, f32x3 center, f32x3 half_extents, f32x3 eye) {
    if (buffer == nullptr || !buffer->ready) return 0;

    f32x3 minimum = center - half_extents;
    f32x3 maximum = center + half_extents;

    // The eight corners, indexed so that bit 0 is x, bit 1 is y and bit 2 is z. The face tables below
    // are written against that numbering.
    f32x3 corner[8];
    for (u32 i = 0; i < 8; i++) {
        corner[i] = (f32x3){
            (i & 1) ? maximum.x : minimum.x,
            (i & 2) ? maximum.y : minimum.y,
            (i & 4) ? maximum.z : minimum.z,
        };
    }

    // Each face as four corner indices, wound consistently, beside the axis and sign of its outward
    // normal. Only the three the eye is on the outside of are submitted; the other three are hidden
    // behind them and would write the same silhouette at a farther depth.
    static const u8 faces[6][4] = {
        { 0, 2, 6, 4 }, // -x
        { 1, 5, 7, 3 }, // +x
        { 0, 4, 5, 1 }, // -y
        { 2, 3, 7, 6 }, // +y
        { 0, 1, 3, 2 }, // -z
        { 4, 6, 7, 5 }, // +z
    };

    f32 offset[6] = {
        minimum.x - eye.x, eye.x - maximum.x,
        minimum.y - eye.y, eye.y - maximum.y,
        minimum.z - eye.z, eye.z - maximum.z,
    };

    u32 rasterized = 0;

    for (u32 face = 0; face < 6; face++) {
        // Positive means the eye is outside that slab, which is exactly when the face points at it.
        if (offset[face] <= 0.0F) continue;

        if (nya_occlusion_quad(buffer, corner[faces[face][0]], corner[faces[face][1]], corner[faces[face][2]], corner[faces[face][3]])) {
            rasterized++;
        }
    }

    return rasterized;
}

b8 nya_occlusion_test(const NYA_OcclusionBuffer* buffer, f32x3 center, f32 radius) {
    if (buffer == nullptr || !buffer->ready) return false;

    // Cast away only the counters, which are what a test is allowed to move. The buffer itself is
    // read only here, and saying so in the signature is worth more than the tidiness of a non-const
    // parameter nobody would then be able to pass a const buffer to.
    NYA_OcclusionStats* stats = (NYA_OcclusionStats*)&buffer->stats;
    stats->tests++;

    f32 minimum_x = 0.0F;
    f32 maximum_x = 0.0F;
    f32 minimum_y = 0.0F;
    f32 maximum_y = 0.0F;
    f32 nearest   = 1.0F;

    /*
     * The sphere is tested as the box that contains it, by projecting the box's eight corners.
     *
     * Bigger than the sphere in every direction, so its screen rectangle covers the sphere's and its
     * nearest corner is nearer than the sphere's nearest point. Both make the answer "hidden" harder
     * to reach, which is the direction this file errs in everywhere. The alternative — projecting the
     * centre and scaling the radius by the perspective divide — is only correct for a sphere on the
     * view axis and quietly wrong at the edges of a wide field of view.
     */
    for (u32 i = 0; i < 8; i++) {
        f32x3 point = {
            center.x + ((i & 1) ? radius : -radius),
            center.y + ((i & 2) ? radius : -radius),
            center.z + ((i & 4) ? radius : -radius),
        };

        _NYA_OcclusionPoint projected = _nya_occlusion_project(buffer, point);

        if (projected.behind) return false;

        if (i == 0 || projected.x < minimum_x) minimum_x = projected.x;
        if (i == 0 || projected.x > maximum_x) maximum_x = projected.x;
        if (i == 0 || projected.y < minimum_y) minimum_y = projected.y;
        if (i == 0 || projected.y > maximum_y) maximum_y = projected.y;
        if (i == 0 || projected.depth < nearest) nearest = projected.depth;
    }

    s32 left   = (s32)floorf(minimum_x);
    s32 right  = (s32)ceilf(maximum_x);
    s32 top    = (s32)floorf(minimum_y);
    s32 bottom = (s32)ceilf(maximum_y);

    // Any part of it outside the buffer is a part nothing has claimed to hide. Frustum culling has
    // already decided it is on screen, so this is the edge of the screen, not the edge of the world.
    if (left < 0 || top < 0 || right > NYA_OCCLUSION_WIDTH || bottom > NYA_OCCLUSION_HEIGHT) return false;
    if (left >= right || top >= bottom) return false;

    if ((right - left) * (bottom - top) > NYA_OCCLUSION_MAX_QUERY_PIXELS) {
        stats->abandoned++;
        return false;
    }

    for (s32 y = top; y < bottom; y++) {
        for (s32 x = left; x < right; x++) {
            // One pixel where the nearest part of the box is in front of, or level with, whatever was
            // claimed there is enough to make the whole thing visible.
            if (nearest <= buffer->depth[((u64)y * NYA_OCCLUSION_WIDTH) + (u64)x]) return false;
        }
    }

    stats->occluded++;
    return true;
}

NYA_OcclusionStats nya_occlusion_stats(const NYA_OcclusionBuffer* buffer) {
    if (buffer == nullptr) return (NYA_OcclusionStats){ 0 };

    return buffer->stats;
}
