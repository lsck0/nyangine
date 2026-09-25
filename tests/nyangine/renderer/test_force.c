/**
 * The general force field: each kind's force at known sample points (uniform is constant, point attracts and
 * repels along the spoke, vortex is tangential, drag opposes velocity, turbulence is bounded and divergence-free),
 * a composed set that sums, the self-advanced clocks, and the two consumers — a particle system pulled into a well
 * and a fluid volume wound up by a vortex. All arithmetic and deterministic, so a failure reproduces exactly.
 **/

#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

/** A pure function must give bit-identical results; the field carries no hidden state to drift. */
static b8 same_vector(f32x3 a, f32x3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

static b8 near_f32(f32 a, f32 b, f32 slack) {
    return fabsf(a - b) <= slack;
}

int main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    NYA_Arena* arena = nya_arena_create(.name = "test_force");
    defer      nya_arena_destroy(arena);

    // TEST: the defaults, and uniform is a constant push everywhere and always
    {
        NYA_ForceField plain = nya_force_field((NYA_ForceOptions){ 0 });
        nya_check(plain.kind == NYA_FORCE_UNIFORM, "the default kind is uniform");
        nya_check(plain.strength == 1.0F, "an unset strength is one");
        nya_check(plain.radius == 1.0F, "an unset radius is one");
        nya_check(same_vector(plain.direction, (f32x3){ 1.0F, 0.0F, 0.0F }), "an unset uniform direction is +x");

        NYA_ForceField uniform = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_UNIFORM, .direction = { 0, 0, 2 }, .strength = 3.0F });
        f32x3          base    = { 0.0F, 0.0F, 3.0F };

        // constant in position, velocity and time: it is direction (normalized) times strength, always.
        f32x3 a = nya_force_at(&uniform, (f32x3){ 5, 1, -2 }, (f32x3){ 9, 9, 9 }, 4.0F);
        f32x3 b = nya_force_at(&uniform, (f32x3){ -3, 7, 8 }, f32x3_zero, 0.0F);
        nya_check(same_vector(a, base) && same_vector(b, base), "uniform is direction*strength everywhere, got (%f,%f,%f)", (f64)a.x, (f64)a.y, (f64)a.z);

        // a vortex with no axis defaults to the vertical, not to +x.
        NYA_ForceField vortex = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_VORTEX });
        nya_check(same_vector(vortex.direction, (f32x3){ 0.0F, 1.0F, 0.0F }), "an unset vortex axis is +y");
    }

    // TEST: a point force attracts toward its centre and repels away from it
    {
        f32x3 point = { 3.0F, 0.0F, 0.0F };

        // negative strength pulls in. linear falloff at distance 3 over radius 10 is 1 - 0.3 = 0.7.
        NYA_ForceField attract = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_POINT, .center = { 0, 0, 0 }, .strength = -2.0F, .radius = 10.0F, .falloff = NYA_FORCE_FALLOFF_LINEAR });
        f32x3          in      = nya_force_at(&attract, point, f32x3_zero, 0.0F);
        nya_check(in.x < 0.0F && in.y == 0.0F && in.z == 0.0F, "an attractor pulls toward the centre along -x, got (%f,%f,%f)", (f64)in.x, (f64)in.y, (f64)in.z);
        nya_check(near_f32(in.x, -1.4F, 1e-4F), "and by strength*falloff = 2*0.7, got %f", (f64)in.x);

        // positive strength pushes out.
        NYA_ForceField repel = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_POINT, .center = { 0, 0, 0 }, .strength = 2.0F, .radius = 10.0F, .falloff = NYA_FORCE_FALLOFF_LINEAR });
        f32x3          out   = nya_force_at(&repel, point, f32x3_zero, 0.0F);
        nya_check(out.x > 0.0F, "a repeller pushes away from the centre along +x, got %f", (f64)out.x);
        nya_check(same_vector(out, in * -1.0F), "the sign of strength is the only difference between attract and repel");

        // linear falloff is zero past the radius; inverse-square is a half at the rim and falls beyond.
        f32x3          far      = { 20.0F, 0.0F, 0.0F };
        nya_check(same_vector(nya_force_at(&repel, far, f32x3_zero, 0.0F), f32x3_zero), "linear falloff reaches zero past the radius");

        NYA_ForceField gravity = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_POINT, .center = { 0, 0, 0 }, .strength = -1.0F, .radius = 10.0F, .falloff = NYA_FORCE_FALLOFF_INVERSE_SQUARE });
        f32            near    = nya_vector_length(nya_force_at(&gravity, (f32x3){ 10.0F, 0, 0 }, f32x3_zero, 0.0F));
        f32            further = nya_vector_length(nya_force_at(&gravity, (f32x3){ 20.0F, 0, 0 }, f32x3_zero, 0.0F));
        nya_check(near_f32(near, 0.5F, 1e-4F), "inverse-square is a half at the rim, got %f", (f64)near);
        nya_check(further < near, "inverse-square keeps falling past the rim, %f < %f", (f64)further, (f64)near);
    }

    // TEST: a vortex force is tangential — perpendicular to the axis and to the spoke
    {
        NYA_ForceField vortex = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_VORTEX, .center = { 0, 0, 0 }, .direction = { 0, 1, 0 }, .strength = 2.0F, .radius = 10.0F, .falloff = NYA_FORCE_FALLOFF_NONE });

        f32x3 spoke = { 3.0F, 0.0F, 0.0F };
        f32x3 axis  = { 0.0F, 1.0F, 0.0F };
        f32x3 swirl = nya_force_at(&vortex, spoke, f32x3_zero, 0.0F);

        nya_check(fabsf(nya_vector_dot(swirl, spoke)) < 1e-4F, "the swirl is perpendicular to the spoke, dot=%f", (f64)nya_vector_dot(swirl, spoke));
        nya_check(fabsf(nya_vector_dot(swirl, axis)) < 1e-4F, "and perpendicular to the axis, dot=%f", (f64)nya_vector_dot(swirl, axis));
        nya_check(near_f32(nya_vector_length(swirl), 2.0F, 1e-4F), "its magnitude is strength (no falloff), got %f", (f64)nya_vector_length(swirl));
        // cross((0,1,0),(1,0,0)) = (0,0,-1): the swirl about +y at +x spoke points to -z.
        nya_check(swirl.z < 0.0F, "the swirl winds the right way about +y, got z=%f", (f64)swirl.z);
    }

    // TEST: drag opposes the sampled velocity, exactly -k * velocity
    {
        NYA_ForceField drag = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_DRAG, .strength = 0.5F });

        f32x3 velocity = { 2.0F, -3.0F, 1.0F };
        f32x3 force    = nya_force_at(&drag, (f32x3){ 100, 100, 100 }, velocity, 0.0F);

        nya_check(same_vector(force, velocity * -0.5F), "drag is -k*velocity, got (%f,%f,%f)", (f64)force.x, (f64)force.y, (f64)force.z);
        nya_check(nya_vector_dot(force, velocity) < 0.0F, "and always opposes the motion, dot=%f", (f64)nya_vector_dot(force, velocity));

        // a still point feels no drag, whatever its position.
        nya_check(same_vector(nya_force_at(&drag, (f32x3){ 5, 6, 7 }, f32x3_zero, 3.0F), f32x3_zero), "no velocity, no drag");
    }

    // TEST: turbulence is deterministic, a copy samples identically, bounded, and ~divergence-free
    {
        NYA_ForceField turb = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_TURBULENCE, .strength = 1.0F, .scale = 0.3F, .seed = 7.0F });

        f32x3 probe = { 2.5F, -1.0F, 4.0F };
        f32x3 first  = nya_force_at(&turb, probe, f32x3_zero, 2.0F);
        f32x3 second = nya_force_at(&turb, probe, f32x3_zero, 2.0F);
        nya_check(same_vector(first, second), "the same field, point and time give the same vector");

        // a copy carries the baked noise table, so it is the same pure function.
        NYA_ForceField copy   = turb;
        nya_check(same_vector(first, nya_force_at(&copy, probe, f32x3_zero, 2.0F)), "a copy samples identically");

        // the field drifts with time, so a later sample differs.
        nya_check(!same_vector(first, nya_force_at(&turb, probe, f32x3_zero, 9.0F)), "the stir animates over time");

        // over a spread of points and times: bounded magnitude, and a divergence that stays near zero because the force is the curl of a potential. divergence is a central difference of the force itself.
        f32 max_magnitude = 0.0F;
        f32 max_divergence = 0.0F;
        f32 h = 0.05F;

        for (u32 i = 0; i < 512; i++) {
            f32   t = (f32)i * 0.031F;
            f32x3 p = { (f32)i * 1.7F - 200.0F, (f32)(i % 11) * 0.9F, (f32)i * -0.6F + 40.0F };

            f32 magnitude = nya_vector_length(nya_force_at(&turb, p, f32x3_zero, t));
            if (magnitude > max_magnitude) max_magnitude = magnitude;

            f32x3 xp = nya_force_at(&turb, p + (f32x3){ h, 0, 0 }, f32x3_zero, t);
            f32x3 xm = nya_force_at(&turb, p - (f32x3){ h, 0, 0 }, f32x3_zero, t);
            f32x3 yp = nya_force_at(&turb, p + (f32x3){ 0, h, 0 }, f32x3_zero, t);
            f32x3 ym = nya_force_at(&turb, p - (f32x3){ 0, h, 0 }, f32x3_zero, t);
            f32x3 zp = nya_force_at(&turb, p + (f32x3){ 0, 0, h }, f32x3_zero, t);
            f32x3 zm = nya_force_at(&turb, p - (f32x3){ 0, 0, h }, f32x3_zero, t);

            f32 divergence = ((xp.x - xm.x) + (yp.y - ym.y) + (zp.z - zm.z)) / (2.0F * h);
            if (fabsf(divergence) > max_divergence) max_divergence = fabsf(divergence);
        }

        printf("  turbulence: max |force| = %f, max |divergence| = %f\n", (f64)max_magnitude, (f64)max_divergence);
        nya_check(max_magnitude < 10.0F, "the stir is bounded, got max |force| = %f", (f64)max_magnitude);
        nya_check(max_divergence < 0.05F, "the stir is ~divergence-free, got max |divergence| = %f", (f64)max_divergence);
    }

    // TEST: a set sums its fields, and its clock feeds nya_forces_sample
    {
        NYA_ForceField push = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_UNIFORM, .direction = { 0, 0, 1 }, .strength = 3.0F });
        NYA_ForceField well = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_POINT, .center = { 0, 0, 0 }, .strength = -1.0F, .radius = 100.0F, .falloff = NYA_FORCE_FALLOFF_NONE });

        NYA_ForceSet set = { 0 };
        set.fields[set.count++] = push;
        set.fields[set.count++] = well;

        f32x3 p = { 4.0F, 0.0F, 0.0F };
        f32x3 v = f32x3_zero;

        f32x3 total    = nya_forces_at(&set, p, v, 0.0F);
        f32x3 expected = nya_force_at(&push, p, v, 0.0F) + nya_force_at(&well, p, v, 0.0F);
        nya_check(same_vector(total, expected), "a set is the sum of its fields, got (%f,%f,%f)", (f64)total.x, (f64)total.y, (f64)total.z);
        // the well pulls toward the origin (−x) while the push adds +z: both survive the sum.
        nya_check(total.x < 0.0F && near_f32(total.z, 3.0F, 1e-4F), "both fields land in the sum, got (%f,%f,%f)", (f64)total.x, (f64)total.y, (f64)total.z);

        // an empty set is no force at all.
        NYA_ForceSet empty = { 0 };
        nya_check(same_vector(nya_forces_at(&empty, p, v, 0.0F), f32x3_zero), "an empty set is zero force");
    }

    // TEST: the self-advanced clocks accumulate and are read by _sample
    {
        NYA_ForceField turb = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_TURBULENCE, .strength = 1.0F, .seed = 2.0F });
        nya_check(turb.time == 0.0F, "a fresh field starts at time zero");

        nya_force_advance(&turb, 0.25F);
        nya_force_advance(&turb, 0.25F);
        nya_force_advance(&turb, -1.0F);   // ignored, the rule the wind and the particle update share
        nya_check(turb.time == 0.5F, "advance accumulates and drops a non-positive step, got %f", (f64)turb.time);

        f32x3 p = { 1.0F, 2.0F, 3.0F };
        nya_check(same_vector(nya_force_sample(&turb, p, f32x3_zero), nya_force_at(&turb, p, f32x3_zero, 0.5F)), "sample reads the field's own clock");

        NYA_ForceSet set = { 0 };
        set.fields[set.count++] = turb;
        nya_forces_advance(&set, 0.75F);
        nya_check(set.time == 0.75F, "the set has its own clock, got %f", (f64)set.time);
        nya_check(same_vector(nya_forces_sample(&set, p, f32x3_zero), nya_forces_at(&set, p, f32x3_zero, 0.75F)), "the set sample reads the set clock");
    }

    // TEST: a particle system under a point attractor is pulled inward over a few steps
    {
        NYA_ParticleSystem* system = nya_particles_create(arena, 16);

        NYA_ForceSet well = { 0 };
        well.fields[well.count++] = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_POINT, .center = { 0, 0, 0 }, .strength = -50.0F, .radius = 100.0F, .falloff = NYA_FORCE_FALLOFF_NONE });

        nya_particles_clear(system);
        (void)nya_particles_emit(system, (NYA_ParticleBurst){ .count = 1, .lifetime_s = { 1000.0F, 1000.0F } });
        system->particles[0].position     = (f32x3){ 10.0F, 0.0F, 0.0F };
        system->particles[0].velocity     = f32x3_zero;
        system->particles[0].acceleration = f32x3_zero;
        system->particles[0].damping      = 0.0F;

        // no force set: the still particle stays put (old behaviour).
        for (u32 i = 0; i < 5; i++) nya_particles_update(system, 0.02F);
        nya_check(near_f32(system->particles[0].position.x, 10.0F, 1e-4F), "with no force the particle stays put, got x=%f", (f64)system->particles[0].position.x);

        // set the well: the particle accelerates toward the origin and its distance falls each step.
        nya_particles_force_set(system, &well, 1.0F);
        f32 previous = system->particles[0].position.x;
        for (u32 i = 0; i < 5; i++) {
            nya_particles_update(system, 0.02F);
            f32 now = system->particles[0].position.x;
            nya_check(now < previous, "the attractor pulls the particle inward each step, %f then %f", (f64)previous, (f64)now);
            previous = now;
        }
        nya_check(system->particles[0].velocity.x < 0.0F, "and it is moving inward, got vx=%f", (f64)system->particles[0].velocity.x);
    }

    // TEST: a fluid volume under a vortex gains the expected swirl
    {
        NYA_Fluid* fluid = nya_fluid_create(arena, (NYA_FluidOptions){
                                                       .space     = NYA_FLUID_SPACE_3D,
                                                       .width     = 16,
                                                       .height    = 16,
                                                       .depth     = 16,
                                                       .cell_size = 1.0F,
                                                       .origin    = { 0.0F, 0.0F, 0.0F },
                                                       .gravity   = { 0.0F, 0.0F, 0.0F },
                                                   });

        // a vortex about the vertical through the volume's centre.
        NYA_ForceSet swirl = { 0 };
        swirl.fields[swirl.count++] = nya_force_field((NYA_ForceOptions){ .kind = NYA_FORCE_VORTEX, .center = { 8, 8, 8 }, .direction = { 0, 1, 0 }, .strength = 5.0F, .radius = 20.0F, .falloff = NYA_FORCE_FALLOFF_NONE });

        // no force: the still volume stays still.
        for (u32 i = 0; i < 10; i++) nya_fluid_step(fluid, 0.05F);
        f32x3 rest = nya_fluid_velocity_at(fluid, (f32x3){ 12.0F, 8.0F, 8.0F });
        nya_check(nya_vector_length(rest) < 1e-2F, "with no force the volume is still, got |v|=%f", (f64)nya_vector_length(rest));

        nya_fluid_force_set(fluid, &swirl, 2.0F);
        for (u32 i = 0; i < 20; i++) nya_fluid_step(fluid, 0.05F);

        // at a +x spoke the swirl about +y points to -z; at a +z spoke it points to +x. the projection keeps the rotation (it is already divergence-free), so the sign survives.
        f32x3 east  = nya_fluid_velocity_at(fluid, (f32x3){ 12.0F, 8.0F, 8.0F });
        f32x3 north = nya_fluid_velocity_at(fluid, (f32x3){ 8.0F, 8.0F, 12.0F });
        printf("  fluid vortex: east=(%f,%f,%f) north=(%f,%f,%f)\n", (f64)east.x, (f64)east.y, (f64)east.z, (f64)north.x, (f64)north.y, (f64)north.z);

        nya_check(east.z < -0.1F, "the +x spoke is pushed toward -z, got z=%f", (f64)east.z);
        nya_check(north.x > 0.1F, "the +z spoke is pushed toward +x, got x=%f", (f64)north.x);
    }

    printf("test_force: all passed\n");
    return 0;
}
