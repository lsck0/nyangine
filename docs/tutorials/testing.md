# Testing

A test is a program. Each file under `tests/` has its own `main`, includes the engine the way an
example does, prints what it tested and returns non-zero on failure. There is no framework to
register with: the engine's own assertions are the oracle, and they stay on in every build.

```bash
./build run test              # every test
./build run test property     # the ones whose path contains "property"
```

Beside plain tests there are four harnesses, compiled only under `NYA_TESTING`, each answering a
different question.

| Harness | Asks | Header |
| :--- | :--- | :--- |
| Property | Does this law hold for every input? | `testing_property.h` |
| Simulation | Does any order of these actions break an invariant? | `testing_simulation.h` |
| Session | Does the real application survive being played? | `testing_session.h` |
| Fuzzing | Does this parser survive hostile bytes? | `tests/fuzz/` |

## Properties

State the law, not the example. A law draws its own inputs and says whether it held:

```c
static b8 law_clamp_lands_in_range(NYA_Property* property) {
    f32 value   = nya_property_draw_f32_any(property);
    f32 clamped = nya_clamp(value, 0.0F, 1.0F);

    nya_property_note(property, "clamp(%f) = %f", (f64)value, (f64)clamped);
    return clamped >= 0.0F && clamped <= 1.0F;
}

failures += nya_property_check("clamp lands in range", 2000, SEED, law_clamp_lands_in_range);
```

When a case fails it is shrunk to the smallest input that still fails, and that is what is printed.
The shrinker works on the bytes the draws read rather than on the values, so every law gets one,
including laws over types nobody wrote a shrinker for. `tests/nyangine/testing/test_property.c`
holds the round trips of every encoder and serializer in the tree.

## Simulations

```c
NYA_SimulationRun* run = nya_simulation_create(.seed = seed, .step_count = 10000);
defer              nya_simulation_destroy(run);

nya_simulation_action_add(run, "spawn", 40, spawn_something);
nya_simulation_action_add(run, "tick", 30, tick_the_world);
nya_simulation_fault_add(run, "corrupt_save", 2, corrupt_the_save_file);
nya_simulation_check_add(run, "counts agree", counts_agree);

u32 failures = nya_simulation_run(run);
```

One seed composes atomic actions and faults into a random sequence, on a simulated clock. A hand
written sequence tests the order somebody thought of; the bugs are in the orders nobody did.

Every draw is `siphash(seed, step, draw_index)`, not a stateful generator, so adding a draw inside
one action does not change every decision after it, and yesterday's failing seed still replays.
When one fails it goes into `tests/gnyame/simulation_seeds.txt`, which `./build run test` replays
forever.

```bash
./build run simulation --seed 0xDEADBEEF --steps 20000 --verbose
```

## Sessions

A session drives the real application headless, pressing keys and moving the mouse through the
same event queue a player's hardware pushes into, as fast as the CPU goes:

```c
static void hold_left(NYA_Session* session) { nya_session_key(session, NYA_KEY_A, true); }

NYA_Session* session = nya_session_create(.seed = seed, .tick_count = 4000, .check = check);
defer        nya_session_destroy(session);

nya_session_action_add(session, "left", 20, hold_left);
nya_session_action_add(session, "idle", 40, nullptr);

u32 failures = nya_session_run(session);
```

The tick count, not a duration, is what a session takes: four thousand ticks is the same run on
every machine. `nya_session_digest` is one number for everything the run did, and the same seed
gives the same digest paced to the wall clock or not. Swap the random policy for a network and
`./build run agent --kind dqn` has a DQN or a NEAT population play gnyame instead.

## Next

- `tests/nyangine/testing/test_session.c` for a complete session, the headless setup included.
- `tests/gnyame/test_simulation.c` for the simulation over the whole engine.
