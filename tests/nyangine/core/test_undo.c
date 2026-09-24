/**
 * A bounded undo/redo history over a reflected value: the ring semantics and round-trip fidelity.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────
 * THE TYPE UNDER TEST
 * ─────────────────────────────────────────────────────────
 *
 * Ints of two signednesses, two float widths, a nested struct, a fixed string and a vector: enough
 * that "undo restored the exact value" is a claim about every kind of field a snapshot has to carry.
 */

typedef struct {
  f32 r, g, b, a;
} UndoColor;

typedef struct {
  s32       health;
  u32       score;
  f64       weight;
  UndoColor tint;
  char      name[32];
  f32x3     position;
  b8        alive;
} UndoState;

/*
 * The tables as the generator would emit them, hand written here the way test_reflection.c does. The
 * primitives and f32x3 are the engine's single copies, from genyarated/reflection_engine.c.
 */

static const NYA_ReflectField _NYA_REFLECT_UndoColor_FIELDS[] = {
  { .name = "r", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(UndoColor, r) },
  { .name = "g", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(UndoColor, g) },
  { .name = "b", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(UndoColor, b) },
  { .name = "a", .type = &_NYA_REFLECT_f32, .offset = nya_offsetof(UndoColor, a) },
};

static const NYA_TypeReflection _NYA_REFLECT_UndoColor = {
  .name        = "UndoColor",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(UndoColor),
  .alignment   = alignof(UndoColor),
  .fields      = _NYA_REFLECT_UndoColor_FIELDS,
  .field_count = 4,
};

static const NYA_TypeReflection _NYA_REFLECT_char_32 = {
  .name          = "char[32]",
  .kind          = NYA_REFLECT_ARRAY,
  .size          = sizeof(char[32]),
  .alignment     = alignof(char[32]),
  .element       = &_NYA_REFLECT_char,
  .element_count = 32,
};

static const NYA_ReflectField _NYA_REFLECT_UndoState_FIELDS[] = {
  { .name = "health", .type = &_NYA_REFLECT_s32, .offset = nya_offsetof(UndoState, health) },
  { .name = "score", .type = &_NYA_REFLECT_u32, .offset = nya_offsetof(UndoState, score) },
  { .name = "weight", .type = &_NYA_REFLECT_f64, .offset = nya_offsetof(UndoState, weight) },
  { .name = "tint", .type = &_NYA_REFLECT_UndoColor, .offset = nya_offsetof(UndoState, tint), .hint = NYA_HINT_COLOR },
  { .name = "name", .type = &_NYA_REFLECT_char_32, .offset = nya_offsetof(UndoState, name) },
  { .name = "position", .type = &_NYA_REFLECT_f32x3, .offset = nya_offsetof(UndoState, position), .hint = NYA_HINT_POSITION },
  { .name = "alive", .type = &_NYA_REFLECT_b8, .offset = nya_offsetof(UndoState, alive) },
};

static const NYA_TypeReflection _NYA_REFLECT_UndoState = {
  .name        = "UndoState",
  .kind        = NYA_REFLECT_STRUCT,
  .size        = sizeof(UndoState),
  .alignment   = alignof(UndoState),
  .fields      = _NYA_REFLECT_UndoState_FIELDS,
  .field_count = 7,
};

/* A distinct value per id, so an undo landing on the wrong one is caught by any field. */
static UndoState make_state(u32 id) {
  UndoState state = {
    .health   = 100 - (s32)id * 10,
    .score    = id * 1000u,
    .weight   = 1.5 * (f64)id,
    .tint     = { .r = 0.1F * (f32)id, .g = 0.2F, .b = 0.3F, .a = 1.0F },
    .position = { (f32)id, (f32)id * 2.0F, (f32)id * 3.0F },
    .alive    = (id % 2) == 0,
  };
  (void)snprintf(state.name, sizeof(state.name), "state-%u", id);

  return state;
}

/* Every field, by value, so this is what "restored the exact value" means and nothing hides in padding. */
static b8 states_equal(const UndoState* a, const UndoState* b) {
  return a->health == b->health && a->score == b->score && a->weight == b->weight && a->tint.r == b->tint.r && a->tint.g == b->tint.g
      && a->tint.b == b->tint.b && a->tint.a == b->tint.a && a->position[0] == b->position[0] && a->position[1] == b->position[1]
      && a->position[2] == b->position[2] && a->alive == b->alive && nya_string_equals(a->name, b->name);
}

s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_undo");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: three states, undo twice reaches state 1, redo reaches state 2
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: record, undo, redo\n");
  {
    NYA_History* history = nya_history_create(arena, &_NYA_REFLECT_UndoState, 8);

    UndoState s1 = make_state(1);
    UndoState s2 = make_state(2);
    UndoState s3 = make_state(3);

    nya_history_record(history, &s1);
    nya_history_record(history, &s2);
    nya_history_record(history, &s3);

    nya_assert(nya_history_count(history) == 3);
    nya_assert(nya_history_can_undo(history));
    nya_assert(!nya_history_can_redo(history), "the newest state has nothing to redo to");

    UndoState out = { 0 };
    nya_assert(nya_history_undo(history, &out));
    nya_assert(states_equal(&out, &s2), "the first undo did not return state 2");

    nya_assert(nya_history_undo(history, &out));
    nya_assert(states_equal(&out, &s1), "the second undo did not return state 1");

    // Round-trip fidelity, field by field, on the restored oldest state.
    nya_assert(out.health == s1.health && out.score == s1.score);
    nya_assert(out.weight == s1.weight, "an f64 field did not survive the round trip");
    nya_assert(out.tint.b == s1.tint.b, "a nested struct field did not survive the round trip");
    nya_assert(out.position[2] == s1.position[2], "a vector element did not survive the round trip");
    nya_assert(nya_string_equals(out.name, "state-1"), "a string field did not survive the round trip");

    nya_assert(!nya_history_can_undo(history));
    nya_assert(!nya_history_undo(history, &out), "an undo past the oldest state must fail");

    nya_assert(nya_history_redo(history, &out));
    nya_assert(states_equal(&out, &s2), "the redo did not return state 2");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a record after an undo drops the redo tail
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: record after undo truncates the tail\n");
  {
    NYA_History* history = nya_history_create(arena, &_NYA_REFLECT_UndoState, 8);

    UndoState s1 = make_state(1);
    UndoState s2 = make_state(2);
    UndoState s3 = make_state(3);
    UndoState s4 = make_state(4);

    nya_history_record(history, &s1);
    nya_history_record(history, &s2);
    nya_history_record(history, &s3);

    UndoState out = { 0 };
    nya_assert(nya_history_undo(history, &out) && states_equal(&out, &s2));
    nya_assert(nya_history_can_redo(history), "state 3 is still ahead until we record over it");

    // Recording here makes state 3 unreachable: the timeline forks at the current state.
    nya_history_record(history, &s4);
    nya_assert(!nya_history_can_redo(history), "recording after an undo must drop the redo tail");
    nya_assert(nya_history_count(history) == 3, "the ring holds s1, s2, s4 — s3 is gone");

    nya_assert(nya_history_undo(history, &out) && states_equal(&out, &s2));
    nya_assert(nya_history_undo(history, &out) && states_equal(&out, &s1));
    nya_assert(nya_history_redo(history, &out) && states_equal(&out, &s2));
    nya_assert(nya_history_redo(history, &out) && states_equal(&out, &s4), "redo must reach the newly recorded state, not the dropped one");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: overflowing the ring evicts the oldest entry
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: overflow evicts the oldest\n");
  {
    const u32    capacity = 3;
    NYA_History* history   = nya_history_create(arena, &_NYA_REFLECT_UndoState, capacity);

    UndoState states[6];
    for (u32 i = 0; i < 6; i++) {
      states[i] = make_state(i + 1);
      nya_history_record(history, &states[i]);
    }

    nya_assert(nya_history_count(history) == capacity, "the ring must not grow past its capacity");

    // The three survivors are the three newest: states 4, 5 and 6. The current one is state 6.
    UndoState out = { 0 };
    nya_assert(nya_history_undo(history, &out) && states_equal(&out, &states[4]), "undo should reach state 5");
    nya_assert(nya_history_undo(history, &out) && states_equal(&out, &states[3]), "undo should reach state 4");
    nya_assert(!nya_history_can_undo(history), "states 1..3 were evicted; there is nothing older");
    nya_assert(!nya_history_undo(history, &out), "an evicted state cannot be undone to");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a capacity of one keeps only the newest, and clamps a huge request
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: degenerate capacities\n");
  {
    NYA_History* single = nya_history_create(arena, &_NYA_REFLECT_UndoState, 1);
    UndoState    s1     = make_state(1);
    UndoState    s2     = make_state(2);

    nya_history_record(single, &s1);
    nya_history_record(single, &s2);
    nya_assert(nya_history_count(single) == 1);
    nya_assert(!nya_history_can_undo(single), "with room for one state there is nothing to undo to");

    UndoState out = { 0 };
    nya_assert(!nya_history_undo(single, &out));

    // A capacity past the cap is clamped rather than refused, so the bound always holds.
    NYA_History* huge = nya_history_create(arena, &_NYA_REFLECT_UndoState, NYA_HISTORY_CAPACITY_MAX + 100);
    nya_history_record(huge, &s1);
    nya_assert(nya_history_count(huge) == 1);

    // Reclaiming a history's memory without tearing down the arena around it.
    nya_history_destroy(huge);
    nya_history_destroy(single);

    printf("  PASSED\n");
  }

  printf("PASSED: test_undo\n");

  return 0;
}
