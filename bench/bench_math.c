/**
 * The math primitives, and the quaternion question in particular.
 *
 * `prompt.md` asked whether quaternion multiplication could be made faster. Static analysis said it was
 * called from two places and never from skinning; `perf.data` measured it at 0.07% of cycles. This is
 * the third answer: what it actually costs, next to the operations around it, so the comparison is a
 * number rather than an argument.
 *
 * Every case works over an array rather than one value, because a single scalar operation is shorter
 * than the clock call that would time it — the harness batches, but a realistic access pattern also
 * matters, and the engine uses these over arrays of entities and bones.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#define COUNT 4096

static NYA_Quaternion quats[COUNT];
static NYA_Quaternion quats_b[COUNT];
static f32x3          vecs[COUNT];
static f32x3          vecs_b[COUNT];
static NYA_Quaternion out_q[COUNT];
static f32x3          out_v[COUNT];
static f32_4x4        out_m[COUNT];
static f32            out_f[COUNT];

static u32 lcg = 0x2468aceu;

static f32 next_f32(void) {
    lcg = (u32)((((u64)lcg * 1664525ull) + 1013904223ull) & 0xFFFFFFFFull);
    return ((f32)(lcg >> 8) / 16777216.0F) * 2.0F - 1.0F;
}

s32 main(void) {
    for (u32 i = 0; i < COUNT; i++) {
        f32x3 axis = nya_vector_normalize((f32x3){ next_f32(), next_f32(), next_f32() + 0.01F });

        quats[i]   = nya_quaternion_from_axis_angle(axis, next_f32() * 3.0F);
        quats_b[i] = nya_quaternion_from_axis_angle(nya_vector_normalize((f32x3){ next_f32(), next_f32() + 0.01F, next_f32() }),
                                                    next_f32() * 3.0F);
        vecs[i]    = (f32x3){ next_f32() * 10.0F, next_f32() * 10.0F, next_f32() * 10.0F };
        vecs_b[i]  = (f32x3){ next_f32() * 10.0F, next_f32() * 10.0F, next_f32() * 10.0F };
    }

    nya_bench_begin("quaternions (4096 per iteration)");

    // The one prompt.md asked about. Kept first so everything else reads relative to it.
    nya_bench("multiply", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out_q[i] = nya_quaternion_multiply(quats[i], quats_b[i]);
        nya_bench_keep(out_q[0].x);
    });

    // What the renderer actually calls, from billboard and sphere corner generation.
    nya_bench("rotate a vector", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out_v[i] = nya_quaternion_rotate(quats[i], vecs[i]);
        nya_bench_keep(out_v[0].x);
    });

    // What skinning calls, once per joint.
    nya_bench("to_matrix4", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out_m[i] = nya_quaternion_to_matrix4(quats[i]);
        nya_bench_keep(out_m[0][0][0]);
    });

    // What pose blending calls, once per bone per blend — the operation that would become hot if the
    // skeletal work in 3.2 lands.
    nya_bench("slerp", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out_q[i] = nya_quaternion_slerp(quats[i], quats_b[i], 0.35F);
        nya_bench_keep(out_q[0].x);
    });

    nya_bench("normalize", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out_q[i] = nya_quaternion_normalize(quats[i]);
        nya_bench_keep(out_q[0].x);
    });

    if (nya_bench_end() != 0) return 1;

    nya_bench_begin("vectors (4096 per iteration)");

    nya_bench("dot", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out_f[i] = nya_vector_dot(vecs[i], vecs_b[i]);
        nya_bench_keep(out_f[0]);
    });

    nya_bench("cross", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out_v[i] = nya_vector_cross(vecs[i], vecs_b[i]);
        nya_bench_keep(out_v[0].x);
    });

    nya_bench("length", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out_f[i] = nya_vector_length(vecs[i]);
        nya_bench_keep(out_f[0]);
    });

    nya_bench("normalize", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out_v[i] = nya_vector_normalize(vecs[i]);
        nya_bench_keep(out_v[0].x);
    });

    if (nya_bench_end() != 0) return 1;

    nya_bench_begin("easing and springs (4096 per iteration)");

    nya_bench("nya_ease cubic_in_out", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out_f[i] = nya_ease(NYA_EASE_CUBIC_IN_OUT, (f32)i / (f32)COUNT);
        nya_bench_keep(out_f[0]);
    });

    nya_bench("nya_ease elastic_out", COUNT, {
        for (u32 i = 0; i < COUNT; i++) out_f[i] = nya_ease(NYA_EASE_ELASTIC_OUT, (f32)i / (f32)COUNT);
        nya_bench_keep(out_f[0]);
    });

    // The stateful integrator, for comparison with the stateless curves above.
    nya_bench("spring step", COUNT, {
        NYA_SpringF32 spring = { .frequency = 4.0F };
        for (u32 i = 0; i < COUNT; i++) out_f[i] = nya_spring_f32(&spring, 1.0F, 1.0F / 60.0F);
        nya_bench_keep(out_f[0]);
    });

    return nya_bench_end();
}
