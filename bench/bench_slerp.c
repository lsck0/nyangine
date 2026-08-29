/**
 * Slerp variants, because `nya_quaternion_slerp` measured at 125.8 ns/item — 22x the multiply that
 * prompt.md asked about — and it is what nya_skeleton_pose_blend calls per bone per blend.
 *
 * Five candidates, so the choice is measured rather than argued:
 *   1. current            two normalizes, acosf, three sinf
 *   2. no-normalize       same, assuming unit inputs (which baked clip frames are)
 *   3. sqrt-identity      sin(acos(x)) == sqrt(1-x^2), replacing one sinf with a sqrt
 *   4. both               no-normalize + sqrt identity
 *   5. nlerp              the floor: what it costs if the curve is allowed to change
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define COUNT 4096

static NYA_Quaternion a[COUNT];
static NYA_Quaternion b[COUNT];
static NYA_Quaternion out[COUNT];

static u32 lcg = 0x13579bdu;
static f32 next_f32(void) {
    lcg = (u32)((((u64)lcg * 1664525ull) + 1013904223ull) & 0xFFFFFFFFull);
    return ((f32)(lcg >> 8) / 16777216.0F) * 2.0F - 1.0F;
}

/* ── 2: assumes unit inputs ── */
static NYA_Quaternion slerp_no_normalize(NYA_Quaternion x, NYA_Quaternion y, f32 t) {
    f32 cosine = nya_quaternion_dot(x, y);
    if (cosine < 0.0F) { y = nya_quaternion_scale(y, -1.0F); cosine = -cosine; }
    if (cosine > 1.0F - NYA_EPSILON) return nya_quaternion_nlerp(x, y, t);

    f32 theta = acosf(nya_clamp(cosine, -1.0F, 1.0F));
    f32 sine  = sinf(theta);

    return nya_quaternion_add(nya_quaternion_scale(x, sinf((1.0F - t) * theta) / sine),
                              nya_quaternion_scale(y, sinf(t * theta) / sine));
}

/* ── 3: sin(acos(x)) == sqrt(1 - x^2) ── */
static NYA_Quaternion slerp_sqrt_identity(NYA_Quaternion x, NYA_Quaternion y, f32 t) {
    NYA_Quaternion start = nya_quaternion_normalize(x);
    NYA_Quaternion end   = nya_quaternion_normalize(y);

    f32 cosine = nya_quaternion_dot(start, end);
    if (cosine < 0.0F) { end = nya_quaternion_scale(end, -1.0F); cosine = -cosine; }
    if (cosine > 1.0F - NYA_EPSILON) return nya_quaternion_nlerp(start, end, t);

    f32 sine  = sqrtf(1.0F - (cosine * cosine));
    f32 theta = atan2f(sine, cosine);

    return nya_quaternion_add(nya_quaternion_scale(start, sinf((1.0F - t) * theta) / sine),
                              nya_quaternion_scale(end, sinf(t * theta) / sine));
}

/* ── 4: both ── */
static NYA_Quaternion slerp_both(NYA_Quaternion x, NYA_Quaternion y, f32 t) {
    f32 cosine = nya_quaternion_dot(x, y);
    if (cosine < 0.0F) { y = nya_quaternion_scale(y, -1.0F); cosine = -cosine; }
    if (cosine > 1.0F - NYA_EPSILON) return nya_quaternion_nlerp(x, y, t);

    f32 sine  = sqrtf(1.0F - (cosine * cosine));
    f32 theta = atan2f(sine, cosine);

    return nya_quaternion_add(nya_quaternion_scale(x, sinf((1.0F - t) * theta) / sine),
                              nya_quaternion_scale(y, sinf(t * theta) / sine));
}

s32 main(void) {
    for (u32 i = 0; i < COUNT; i++) {
        a[i] = nya_quaternion_from_axis_angle(nya_vector_normalize((f32x3){ next_f32(), next_f32(), next_f32() + 0.01F }), next_f32() * 3.0F);
        b[i] = nya_quaternion_from_axis_angle(nya_vector_normalize((f32x3){ next_f32(), next_f32() + 0.01F, next_f32() }), next_f32() * 3.0F);
    }

    // Accuracy first: a faster slerp that is not a slerp is not a candidate.
    f64 worst_no_norm = 0, worst_sqrt = 0, worst_both = 0, worst_nlerp = 0;
    for (u32 i = 0; i < COUNT; i++) {
        for (f32 t = 0.0F; t <= 1.0F; t += 0.125F) {
            NYA_Quaternion reference = nya_quaternion_slerp(a[i], b[i], t);

            #define WORST(fn, acc) do {                                                                        \
                NYA_Quaternion got = fn(a[i], b[i], t);                                                        \
                f64 d = fabs((f64)got.x - reference.x) + fabs((f64)got.y - reference.y)                        \
                      + fabs((f64)got.z - reference.z) + fabs((f64)got.w - reference.w);                       \
                if (d > (acc)) (acc) = d;                                                                      \
            } while (0)

            WORST(slerp_no_normalize, worst_no_norm);
            WORST(slerp_sqrt_identity, worst_sqrt);
            WORST(slerp_both, worst_both);
            WORST(nya_quaternion_nlerp, worst_nlerp);
            #undef WORST
        }
    }

    (void)printf("\n  max deviation from nya_quaternion_slerp, over 4096 pairs x 9 values of t:\n");
    (void)printf("    no-normalize   %.3e\n", worst_no_norm);
    (void)printf("    sqrt-identity  %.3e\n", worst_sqrt);
    (void)printf("    both           %.3e\n", worst_both);
    (void)printf("    nlerp          %.3e   <- a different curve, shown for scale\n", worst_nlerp);

    nya_bench_begin("slerp variants (4096 per iteration)");

    nya_bench("current", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out[i] = nya_quaternion_slerp(a[i], b[i], 0.35F);
        nya_bench_keep(out[0].x);
    });

    nya_bench("no-normalize", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out[i] = slerp_no_normalize(a[i], b[i], 0.35F);
        nya_bench_keep(out[0].x);
    });

    nya_bench("sqrt-identity", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out[i] = slerp_sqrt_identity(a[i], b[i], 0.35F);
        nya_bench_keep(out[0].x);
    });

    nya_bench("both", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out[i] = slerp_both(a[i], b[i], 0.35F);
        nya_bench_keep(out[0].x);
    });

    nya_bench("slerp_unit (new)", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out[i] = nya_quaternion_slerp_unit(a[i], b[i], 0.35F);
        nya_bench_keep(out[0].x);
    });

    // Adjacent baked frames are a few degrees apart, which is the case the widened threshold targets
    // and the one a pose sample actually hits.
    static NYA_Quaternion near_a[COUNT];
    static NYA_Quaternion near_b[COUNT];
    for (u32 i = 0; i < COUNT; i++) {
        f32x3 axis = nya_vector_normalize((f32x3){ next_f32(), next_f32(), next_f32() + 0.01F });
        near_a[i]  = nya_quaternion_from_axis_angle(axis, next_f32());
        near_b[i]  = nya_quaternion_multiply(near_a[i], nya_quaternion_from_axis_angle(axis, 0.05F));
        near_b[i]  = nya_quaternion_normalize(near_b[i]);
    }

    nya_bench("slerp_unit, adjacent frames", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out[i] = nya_quaternion_slerp_unit(near_a[i], near_b[i], 0.35F);
        nya_bench_keep(out[0].x);
    });

    nya_bench("nlerp (floor)", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out[i] = nya_quaternion_nlerp(a[i], b[i], 0.35F);
        nya_bench_keep(out[0].x);
    });

    return nya_bench_end();
}
