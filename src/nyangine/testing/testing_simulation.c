#include "nyangine/nyangine.h"

#ifdef NYA_TESTING

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Registers an action. The one place `weight_total` is kept in step with the table. */
NYA_INTERNAL void _nya_simulation_action_register(NYA_SimulationRun* run, NYA_ConstCString name, u32 weight, NYA_SimulationActionFn action, b8 is_fault);

/** Which action this step takes, by walking the weights. NYA_SIMULATION_MAX_ACTIONS when none can. */
NYA_INTERNAL u32 _nya_simulation_action_pick(NYA_SimulationRun* run);

/** Appends an action index to the history ring. */
NYA_INTERNAL void _nya_simulation_history_push(NYA_SimulationRun* run, u32 action);

/** One primitive field, filled from the run's entropy and whatever the hint says it means. */
NYA_INTERNAL void _nya_simulation_fill_primitive(NYA_SimulationRun* run, const NYA_TypeReflection* type, NYA_ReflectHint hint, void* instance);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────
 */

NYA_SimulationRun* nya_simulation_create_with_options(NYA_SimulationOptions options) {
    nya_assert(options.time_step_ns > 0, "a simulated tick of zero would never advance the clock");

    NYA_Arena* allocator = nya_arena_create(.name = "simulation");

    NYA_SimulationRun* run = nya_arena_alloc(allocator, sizeof(NYA_SimulationRun));

    *run = (NYA_SimulationRun){
        .allocator    = allocator,
        .seed         = options.seed,
        .step_count   = options.step_count,
        .time_step_ns = options.time_step_ns,
        .verbose      = options.verbose,
        .user_data    = options.user_data,
    };

    nya_assert(run->step == 0);
    nya_assert(run->clock_ns == 0);
    nya_assert(run->action_count == 0 && run->weight_total == 0);

    return run;
}

void nya_simulation_destroy(NYA_SimulationRun* run) {
    if (run == nullptr) return;

    // the run lives inside the arena, so this frees the struct too. Read before the free.
    NYA_Arena* allocator = run->allocator;
    nya_arena_destroy(allocator);
}

/*
 * ─────────────────────────────────────────────────────────
 * ACTIONS AND CHECKS
 * ─────────────────────────────────────────────────────────
 */

void nya_simulation_action_add(NYA_SimulationRun* run, NYA_ConstCString name, u32 weight, NYA_SimulationActionFn action) {
    _nya_simulation_action_register(run, name, weight, action, false);
}

void nya_simulation_fault_add(NYA_SimulationRun* run, NYA_ConstCString name, u32 weight, NYA_SimulationActionFn fault) {
    _nya_simulation_action_register(run, name, weight, fault, true);
}

void nya_simulation_check_add(NYA_SimulationRun* run, NYA_ConstCString name, NYA_SimulationCheckFn check) {
    nya_assert(run != nullptr);
    nya_assert(name != nullptr && name[0] != '\0');
    nya_assert(check != nullptr);

    // a broken invariant that is never checked because the table silently filled up is exactly the
    // failure this harness exists to catch, so the bound crashes rather than logs.
    nya_assert(run->check_count < NYA_SIMULATION_MAX_CHECKS, "more than %d invariants; raise NYA_SIMULATION_MAX_CHECKS", NYA_SIMULATION_MAX_CHECKS);

    run->checks[run->check_count++] = (NYA_SimulationCheck){ .name = name, .run = check };
}

/*
 * ─────────────────────────────────────────────────────────
 * THE RUN
 * ─────────────────────────────────────────────────────────
 */

u32 nya_simulation_run(NYA_SimulationRun* run) {
    nya_assert(run != nullptr);
    nya_assert(run->action_count > 0, "a simulation with no actions would take no steps");
    nya_assert(run->weight_total > 0, "every registered action has a weight of zero");

    printf("SIMULATION: seed 0x%016llX, %llu steps, %u actions, %u invariants\n", (unsigned long long)run->seed,
           (unsigned long long)run->step_count, run->action_count, run->check_count);

    u32 failures_before = run->failures;

    for (run->step = 0; run->step < run->step_count; run->step++) {
        // reset per step, so a draw's coordinate is (seed, step, index within the step) and adding a
        // draw inside one action cannot shift the decisions of every step after it.
        run->draw = 0;

        u32 index = _nya_simulation_action_pick(run);
        nya_assert(index < run->action_count);

        NYA_SimulationAction* action = &run->actions[index];

        if (run->verbose) printf("  step %llu: %s\n", (unsigned long long)run->step, action->name);

        action->taken++;
        _nya_simulation_history_push(run, index);

        action->run(run);

        // the oracle, after every action rather than at the end: a violated invariant names the action
        // that broke it instead of the last one before the run finished.
        for (u32 i = 0; i < run->check_count; i++) run->checks[i].run(run);
    }

    /*
     * The mix, so a run that never reached an action is visible rather than reported as a pass.
     */
    printf("  %llu steps, %llu simulated ms\n", (unsigned long long)run->step_count, (unsigned long long)(run->clock_ns / 1000000ULL));

    for (u32 i = 0; i < run->action_count; i++) {
        const NYA_SimulationAction* action = &run->actions[i];

        if (action->taken == 0) {
            printf("  NEVER TAKEN: %s%s\n", action->name, action->is_fault ? " (fault)" : "");
        }
    }

    u32 failures = run->failures - failures_before;
    if (failures > 0) nya_simulation_report(run);

    return failures;
}

void nya_simulation_report(const NYA_SimulationRun* run) {
    nya_assert(run != nullptr);

    printf("  SEED: 0x%016llX  STEP: %llu\n", (unsigned long long)run->seed, (unsigned long long)run->step);
    printf("  REPLAY: ./build run simulation --seed 0x%016llX --steps %llu\n", (unsigned long long)run->seed,
           (unsigned long long)run->step_count);

    u64 kept  = nya_min(run->history_count, (u64)NYA_SIMULATION_HISTORY_MAX);
    u64 first = run->history_count - kept;

    printf("  LAST %llu ACTIONS:\n", (unsigned long long)kept);

    for (u64 i = 0; i < kept; i++) {
        u16 index = run->history[(first + i) % NYA_SIMULATION_HISTORY_MAX];
        nya_assert(index < run->action_count);

        printf("    %llu %s%s\n", (unsigned long long)(first + i), run->actions[index].name, run->actions[index].is_fault ? " (fault)" : "");
    }
}

void nya_simulation_fail(NYA_SimulationRun* run, NYA_ConstCString format, ...) {
    nya_assert(run != nullptr);
    nya_assert(format != nullptr);

    if (run->failures < NYA_SIMULATION_REPORT_MAX) {
        printf("  FAIL (seed 0x%016llX step %llu): ", (unsigned long long)run->seed, (unsigned long long)run->step);

        va_list arguments;
        va_start(arguments, format);
        (void)vprintf(format, arguments);
        va_end(arguments);

        printf("\n");
    }

    run->failures++;
}

/*
 * ─────────────────────────────────────────────────────────
 * ENTROPY
 * ─────────────────────────────────────────────────────────
 */

u64 nya_simulation_roll(NYA_SimulationRun* run) {
    nya_assert(run != nullptr);

    u64 coordinate[3] = { run->seed, run->step, run->draw };
    run->draw++;

    return nya_siphash(coordinate, sizeof(coordinate), NYA_SIMULATION_HASH_KEY_LOW, NYA_SIMULATION_HASH_KEY_HIGH);
}

u64 nya_simulation_below(NYA_SimulationRun* run, u64 limit) {
    // nothing is below zero, and the modulo would divide by it.
    if (limit == 0) return 0;

    return nya_simulation_roll(run) % limit;
}

b8 nya_simulation_chance(NYA_SimulationRun* run, u32 percent) {
    nya_assert(percent <= 100, "a chance is a percentage, got %u", percent);

    return nya_simulation_below(run, 100) < percent;
}

f32 nya_simulation_range_f32(NYA_SimulationRun* run, f32 low, f32 high) {
    if (!(high > low)) return low;

    // 24 bits, which is exactly what an f32 mantissa holds, so the division is exact and the
    // distribution has no gaps a wider draw would leave.
    f32 unit = (f32)(nya_simulation_roll(run) & 0xFFFFFFULL) / (f32)0x1000000ULL;

    return low + ((high - low) * unit);
}

f32 nya_simulation_shaped_f32(NYA_SimulationRun* run, f32 low, f32 high) {
    /*
     * A quarter of the draws are edges. Uniform noise almost never lands on a boundary, and boundaries
     * are where the bugs are: a zero radius, a negative size, a denormal, an infinity a later multiply
     * turns into a NaN.
     */
    if (!nya_simulation_chance(run, 25)) return nya_simulation_range_f32(run, low, high);

    switch (nya_simulation_below(run, 8)) {
        case 0:  return 0.0F;
        case 1:  return low;
        case 2:  return high;
        case 3:  return -0.0F;
        case 4:  return low - 1.0F;
        case 5:  return high + 1.0F;
        case 6:  return NYA_EPSILON;
        default: return -1.0F;
    }
}

u64 nya_simulation_pick(NYA_SimulationRun* run, u64 count) {
    return nya_simulation_below(run, count);
}

/*
 * ─────────────────────────────────────────────────────────
 * TIME
 * ─────────────────────────────────────────────────────────
 */

u64 nya_simulation_now_ns(const NYA_SimulationRun* run) {
    nya_assert(run != nullptr);
    return run->clock_ns;
}

void nya_simulation_advance(NYA_SimulationRun* run, u64 nanoseconds) {
    nya_assert(run != nullptr);

    u64 before = run->clock_ns;
    run->clock_ns += nanoseconds;

    // a simulated clock that wrapped would make every duration computed from it negative, which reads
    // as a scheduling bug three layers away from the wrap.
    nya_assert(run->clock_ns >= before, "the simulated clock wrapped");
}

f32 nya_simulation_delta_s(const NYA_SimulationRun* run) {
    nya_assert(run != nullptr);

    return (f32)((f64)run->time_step_ns / 1e9);
}

/*
 * ─────────────────────────────────────────────────────────
 * WELL SHAPED DATA
 * ─────────────────────────────────────────────────────────
 */

void nya_simulation_fill(NYA_SimulationRun* run, const NYA_TypeReflection* type, void* instance) {
    nya_assert(run != nullptr);
    nya_assert(type != nullptr);
    nya_assert(instance != nullptr);

    switch (type->kind) {
        case NYA_REFLECT_PRIMITIVE: {
            _nya_simulation_fill_primitive(run, type, NYA_HINT_NONE, instance);
        } break;

        case NYA_REFLECT_STRUCT:
        case NYA_REFLECT_UNION: {
            /*
             * A union is filled through one member only: writing every member would leave the last one
             * written on top of the others, which is not a value the tag describes.
             */
            u32 first = 0;
            u32 count = type->field_count;

            if (type->kind == NYA_REFLECT_UNION && count > 0) {
                first = (u32)nya_simulation_below(run, count);
                count = first + 1;
            }

            for (u32 i = first; i < count; i++) {
                const NYA_ReflectField* field = &type->fields[i];

                void* address = nya_reflect_field_pointer(instance, field);

                if (field->type->kind == NYA_REFLECT_PRIMITIVE) {
                    _nya_simulation_fill_primitive(run, field->type, field->hint, address);
                } else {
                    nya_simulation_fill(run, field->type, address);
                }
            }
        } break;

        case NYA_REFLECT_ENUM: {
            if (type->variant_count == 0) break;

            NYA_Value value = { .type = type->primitive };

            if (type->is_bitflags) {
                /* A subset of the declared bits, rather than a number that happens to fit. */
                s64 bits = 0;
                for (u32 i = 0; i < type->variant_count; i++) {
                    if (nya_simulation_chance(run, 30)) bits |= type->variants[i].value;
                }

                value.as_s64 = bits;
            } else {
                value.as_s64 = type->variants[nya_simulation_below(run, type->variant_count)].value;
            }

            // written through nya_reflect_write, which narrows to the enum's own primitive, so a u8
            // backed enum gets a byte and not eight.
            value.type = NYA_TYPE_S64;
            (void)nya_reflect_write(type, instance, value);
        } break;

        case NYA_REFLECT_ARRAY:
        case NYA_REFLECT_VECTOR: {
            if (type->element == nullptr || type->element->size == 0) break;

            for (u32 i = 0; i < type->element_count; i++) {
                nya_simulation_fill(run, type->element, (u8*)instance + ((u64)i * type->element->size));
            }
        } break;

        case NYA_REFLECT_POINTER: {
            // left alone on purpose: a pointer filled with entropy is a segfault, not a test. The
            // pointers in a reflected struct are owned elsewhere and a scenario that wants one set
            // sets it itself.
        } break;

        case NYA_REFLECT_COUNT:
        default: nya_assert_always(false, "reflection kind %d is not a kind", (int)type->kind); break;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_simulation_action_register(NYA_SimulationRun* run, NYA_ConstCString name, u32 weight, NYA_SimulationActionFn action, b8 is_fault) {
    nya_assert(run != nullptr);
    nya_assert(name != nullptr && name[0] != '\0');
    nya_assert(action != nullptr);
    nya_assert(run->step == 0, "actions are registered before the run starts, so the mix is fixed for the whole seed");

    nya_assert(run->action_count < NYA_SIMULATION_MAX_ACTIONS, "more than %d actions; raise NYA_SIMULATION_MAX_ACTIONS",
               NYA_SIMULATION_MAX_ACTIONS);

    u64 weight_before = run->weight_total;

    run->actions[run->action_count++] = (NYA_SimulationAction){
        .name     = name,
        .weight   = weight,
        .run      = action,
        .is_fault = is_fault,
    };

    run->weight_total += weight;

    nya_assert(run->weight_total == weight_before + weight, "the weight total drifted from the table");
}

u32 _nya_simulation_action_pick(NYA_SimulationRun* run) {
    nya_assert(run->weight_total > 0);

    u64 roll = nya_simulation_below(run, run->weight_total);

    for (u32 i = 0; i < run->action_count; i++) {
        if (roll < run->actions[i].weight) return i;
        roll -= run->actions[i].weight;
    }

    // unreachable: the roll is below the sum of the weights, so the walk consumes it before the end.
    nya_assert_always(false, "the weight walk ran off the end of the action table");
    __builtin_unreachable();
}

void _nya_simulation_history_push(NYA_SimulationRun* run, u32 action) {
    nya_assert(action < run->action_count);
    nya_assert(action <= U16_MAX, "an action index has to fit the history ring's u16");

    run->history[run->history_count % NYA_SIMULATION_HISTORY_MAX] = (u16)action;
    run->history_count++;
}

void _nya_simulation_fill_primitive(NYA_SimulationRun* run, const NYA_TypeReflection* type, NYA_ReflectHint hint, void* instance) {
    nya_assert(type->kind == NYA_REFLECT_PRIMITIVE);

    NYA_Value value = { .type = type->primitive };

    switch (type->primitive) {
        case NYA_TYPE_B8:
        case NYA_TYPE_B16:
        case NYA_TYPE_B32:
        case NYA_TYPE_B64: {
            value.type   = NYA_TYPE_S64;
            value.as_s64 = nya_simulation_chance(run, 50) ? 1 : 0;
        } break;

        case NYA_TYPE_F16:
        case NYA_TYPE_F32:
        case NYA_TYPE_F64: {
            f32 filled = 0.0F;

            switch (hint) {
                // a colour channel outside [0, 1] is a different bug from a colour channel that is a
                // NaN, and the shaped draw finds the second while this keeps the first out of the way.
                case NYA_HINT_COLOR: filled = nya_simulation_range_f32(run, 0.0F, 1.0F); break;

                case NYA_HINT_POSITION: filled = nya_simulation_shaped_f32(run, -4096.0F, 4096.0F); break;
                case NYA_HINT_SCALE:    filled = nya_simulation_shaped_f32(run, 0.0F, 16.0F); break;
                case NYA_HINT_EULER:    filled = nya_simulation_shaped_f32(run, -(f32)M_PI, (f32)M_PI); break;

                case NYA_HINT_NONE:
                case NYA_HINT_ASSET:
                case NYA_HINT_BITFLAGS:
                case NYA_HINT_COUNT:
                default: filled = nya_simulation_shaped_f32(run, -1.0F, 1.0F); break;
            }

            value.type   = NYA_TYPE_F32;
            value.as_f32 = filled;
        } break;

        case NYA_TYPE_STRING: {
            // never written: the string a reflected struct holds is a borrowed literal or an arena
            // allocation, and a pointer made of entropy is a crash rather than a finding.
            return;
        }

        case NYA_TYPE_NULL:
        case NYA_TYPE_VOID:
        case NYA_TYPE_VOID_POINTER: return;

        default: {
            /* Every integer, including the ones a hint does not describe. */
            value.type   = NYA_TYPE_S64;
            value.as_s64 = (s64)(nya_simulation_roll(run) & 0xFFFFULL);

            // a quarter of the draws are the edges of whatever width this is, which the write clamps
            // into range rather than the simulator having to know each width's bounds.
            if (nya_simulation_chance(run, 25)) value.as_s64 = nya_simulation_chance(run, 50) ? 0 : -1;
        } break;
    }

    (void)nya_reflect_write(type, instance, value);
}

#endif // NYA_TESTING
