/**
 * RNG stream consistency, optimizer step arithmetic, and serde round tripping by type.
 */

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"


s32 main(void) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  NYA_Arena* arena = nya_arena_create(.name = "test_rng_optimizer_serde");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: gen_bytes produces the same stream however it is chunked
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: rng stream is chunk independent\n");
  {
    // Larger than the internal buffer, so the refill path is crossed either way.
    enum { STREAM = 4096 };

    u8 whole[STREAM];
    NYA_RNG a = nya_rng_create_with_options((NYA_RNGOptions){ .seed = "ABCDEF0123456789" });
    nya_rng_gen_bytes(&a, whole, STREAM);

    // Same seed, drawn in ragged pieces. The cursor and refill logic must land on the same bytes.
    u8      pieces[STREAM];
    NYA_RNG b      = nya_rng_create_with_options((NYA_RNGOptions){ .seed = "ABCDEF0123456789" });
    u64     offset = 0;
    u64     step   = 1;

    while (offset < STREAM) {
      u64 chunk = nya_min(step, (u64)STREAM - offset);
      nya_rng_gen_bytes(&b, &pieces[offset], chunk);
      offset += chunk;
      step    = (step * 3) + 1; // ragged, and eventually far past the buffer size
      if (step > 700) step = 1;
    }

    nya_check(nya_memcmp(whole, pieces, STREAM) == 0, "the chunked stream diverges from the contiguous one");

    // A zero length draw must not advance anything.
    NYA_RNG c = nya_rng_create_with_options((NYA_RNGOptions){ .seed = "ABCDEF0123456789" });
    u8      one_a[8];
    u8      one_b[8];
    nya_rng_gen_bytes(&c, one_a, 0);
    nya_rng_gen_bytes(&c, one_b, 8);
    nya_check(nya_memcmp(one_b, whole, 8) == 0, "a zero length draw disturbed the stream");
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: SGD and Adam steps against the arithmetic they document
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: optimizer steps\n");
  {
    // SGD, momentum 0: p -= lr * g
    {
      NYA_NNTensor* p = nya_nn_tensor_create(arena, NYA_NN_SHAPE(1), true);
      p->data[0]      = 1.0F;
      p->grad[0]      = 0.5F;

      NYA_NNOptimizer* sgd = nya_nn_optimizer_sgd(arena, (NYA_NNOptimizerConfig){ .learning_rate = 0.1F });
      nya_nn_optimizer_add(sgd, p);
      nya_nn_optimizer_step(sgd);

      nya_check(fabsf(p->data[0] - 0.95F) < 1e-6F, "sgd step gave %.7f, expected 0.95", (f64)p->data[0]);
    }

    // SGD with momentum: v = m*v + g, p -= lr*v, twice.
    {
      NYA_NNTensor* p = nya_nn_tensor_create(arena, NYA_NN_SHAPE(1), true);
      p->data[0]      = 0.0F;

      NYA_NNOptimizer* sgd = nya_nn_optimizer_sgd(arena, (NYA_NNOptimizerConfig){ .learning_rate = 0.1F, .momentum = 0.9F });
      nya_nn_optimizer_add(sgd, p);

      p->grad[0] = 1.0F;
      nya_nn_optimizer_step(sgd); // v = 1.0, p = -0.1
      p->grad[0] = 1.0F;
      nya_nn_optimizer_step(sgd); // v = 1.9, p = -0.29

      nya_check(fabsf(p->data[0] - (-0.29F)) < 1e-5F, "sgd momentum gave %.7f, expected -0.29", (f64)p->data[0]);
    }

    // Adam, first step: the moments cancel against the bias correction, so the update is very
    // nearly exactly the learning rate regardless of the gradient's size.
    {
      NYA_NNTensor* p = nya_nn_tensor_create(arena, NYA_NN_SHAPE(1), true);
      p->data[0]      = 0.0F;
      p->grad[0]      = 7.0F;

      NYA_NNOptimizer* adam = nya_nn_optimizer_adam(arena, (NYA_NNOptimizerConfig){ .learning_rate = 0.01F });
      nya_nn_optimizer_add(adam, p);
      nya_nn_optimizer_step(adam);

      nya_check(fabsf(p->data[0] - (-0.01F)) < 1e-5F, "adam first step gave %.7f, expected -0.01", (f64)p->data[0]);
    }

    // Gradient clipping is elementwise and applied before the step.
    {
      NYA_NNTensor* p = nya_nn_tensor_create(arena, NYA_NN_SHAPE(1), true);
      p->data[0]      = 0.0F;
      p->grad[0]      = 1000.0F;

      NYA_NNOptimizer* sgd = nya_nn_optimizer_sgd(arena, (NYA_NNOptimizerConfig){ .learning_rate = 1.0F, .gradient_clip = 0.5F });
      nya_nn_optimizer_add(sgd, p);
      nya_nn_optimizer_step(sgd);

      nya_check(fabsf(p->data[0] - (-0.5F)) < 1e-6F, "clipped sgd gave %.7f, expected -0.5", (f64)p->data[0]);
    }
  }
  printf("  PASSED\n");

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: .nya and JSON round trip every scalar type they claim to carry
  // ─────────────────────────────────────────────────────────────────────────────
  printf("TEST: serde round trip by type\n");
  {
    NYA_Object* object = nya_object_create(arena);

    nya_object_set(object, "b8", ((NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = true }));
    nya_object_set(object, "u8", ((NYA_Value){ .type = NYA_TYPE_U8, .as_u8 = 255 }));
    nya_object_set(object, "u16", ((NYA_Value){ .type = NYA_TYPE_U16, .as_u16 = 65535 }));
    nya_object_set(object, "u32", ((NYA_Value){ .type = NYA_TYPE_U32, .as_u32 = U32_MAX }));
    nya_object_set(object, "u64", ((NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = U64_MAX }));
    nya_object_set(object, "s8", ((NYA_Value){ .type = NYA_TYPE_S8, .as_s8 = S8_MIN }));
    nya_object_set(object, "s16", ((NYA_Value){ .type = NYA_TYPE_S16, .as_s16 = S16_MIN }));
    nya_object_set(object, "s32", ((NYA_Value){ .type = NYA_TYPE_S32, .as_s32 = S32_MIN }));
    nya_object_set(object, "s64", ((NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = S64_MIN }));
    nya_object_set(object, "f32", ((NYA_Value){ .type = NYA_TYPE_F32, .as_f32 = 0.1F }));
    nya_object_set(object, "f64", ((NYA_Value){ .type = NYA_TYPE_F64, .as_f64 = 0.1 }));
    nya_object_set(object, "char", ((NYA_Value){ .type = NYA_TYPE_CHAR, .as_char = 'q' }));
    nya_object_set(object, "string", ((NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "a \"quoted\"\n\tvalue" }));
    nya_object_set(object, "null", ((NYA_Value){ .type = NYA_TYPE_NULL }));

    // .nya writes hexadecimal floats precisely so this is exact rather than approximate.
    NYA_String* encoded = nya_serde_nya_serialize(arena, object, 0);

    NYA_Object* back  = nullptr;
    NYA_Error   error = nya_serde_nya_deserialize(arena, encoded->items, encoded->length, 0, &back);
    nya_check(error.ok, "nya format failed to reparse its own output");

    if (error.ok) {
      const NYA_Value* u64_back = nya_object_get(back, "u64");
      nya_check(u64_back != nullptr && u64_back->as_u64 == U64_MAX, "u64 did not survive the .nya round trip");

      const NYA_Value* s64_back = nya_object_get(back, "s64");
      nya_check(s64_back != nullptr && s64_back->as_s64 == S64_MIN, "s64 did not survive the .nya round trip");

      const NYA_Value* f64_back = nya_object_get(back, "f64");
      nya_check(f64_back != nullptr && f64_back->as_f64 == 0.1, "f64 did not survive the .nya round trip exactly");

      const NYA_Value* f32_back = nya_object_get(back, "f32");
      nya_check(f32_back != nullptr && f32_back->as_f32 == 0.1F, "f32 did not survive the .nya round trip exactly");

      const NYA_Value* string_back = nya_object_get(back, "string");
      nya_check(
        string_back != nullptr && string_back->as_string != nullptr && strcmp(string_back->as_string, "a \"quoted\"\n\tvalue") == 0,
        "the escaped string did not survive the .nya round trip"
      );

      NYA_String* again = nya_serde_nya_serialize(arena, back, 0);
      nya_check(nya_string_equals(again, encoded), "the .nya format is not idempotent");
    }

    // JSON is lossier by construction (no widths), but must still round trip its own output.
    NYA_String* json  = nya_serde_json_serialize(arena, object, 0);
    NYA_Object* json_back = nullptr;
    NYA_Error   json_error = nya_serde_json_deserialize(arena, json->items, json->length, 0, &json_back);
    nya_check(json_error.ok, "JSON failed to reparse its own output");

    if (json_error.ok) {
      NYA_String* json_again = nya_serde_json_serialize(arena, json_back, 0);
      if (!nya_string_equals(json_again, json)) {
        printf("  first:  " NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(json));
        printf("  second: " NYA_FMT_STRING "\n", NYA_FMT_STRING_ARG(json_again));
      }

      // The fixed point is reached after one pass, not zero: JSON has no widths, so a u64 past
      // S64_MAX comes back as f64 on the first read by design. Two and three must agree.
      NYA_Object* json_third = nullptr;
      NYA_Error   third_error = nya_serde_json_deserialize(arena, json_again->items, json_again->length, 0, &json_third);
      nya_check(third_error.ok, "JSON failed to reparse its second pass output");

      if (third_error.ok) {
        NYA_String* json_third_text = nya_serde_json_serialize(arena, json_third, 0);
        nya_check(nya_string_equals(json_third_text, json_again), "the JSON writer has no fixed point");
      }

      const NYA_Value* string_back = nya_object_get(json_back, "string");
      nya_check(
        string_back != nullptr && string_back->as_string != nullptr && strcmp(string_back->as_string, "a \"quoted\"\n\tvalue") == 0,
        "the escaped string did not survive the JSON round trip"
      );
    }
  }
  printf("  PASSED\n");

  nya_arena_destroy(arena);

  printf("%s: test_rng_optimizer_serde (" FMTu32 " failures)\n", nya_check_failures() == 0 ? "PASSED" : "FAILED", nya_check_failures());
  return nya_check_failures() == 0 ? 0 : 1;
}
