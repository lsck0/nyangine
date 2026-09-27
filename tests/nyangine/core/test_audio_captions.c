/**
 * The caption ring and the quarter a caption's marker shows. Pure, so none of it needs an audio device.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

s32 main(void) {
  // TEST: a caption stays up for its hold and comes down after it
  {
    NYA_AudioCaptions captions = { 0 };

    nya_audio_captions_add(&captions, "[thud]", (NYA_AudioCaption){ .hold_until_s = 2.0 });
    nya_assert_eq(captions.live, 1U);

    nya_audio_captions_remove(&captions, 1.9);
    nya_assert_eq(captions.live, 1U);
    nya_assert(captions.items[0].shown, "a caption must stay up until its hold is over");

    // the voice is NYA_SOUND_VOICE_NONE, which never plays, so the hold alone decides.
    nya_audio_captions_remove(&captions, 2.0);
    nya_assert_eq(captions.live, 0U);
    nya_assert(!captions.items[0].shown, "a caption must come down once its hold is over and its voice is silent");
  }

  // TEST: the ring is bounded, drops the oldest, and folds a repeat into the line already up
  {
    NYA_AudioCaptions captions = { 0 };
    char              text[8];

    for (u32 i = 0; i < NYA_AUDIO_CAPTIONS_MAX + 2; i++) {
      (void)snprintf(text, sizeof(text), "c%u", i);
      nya_audio_captions_add(&captions, text, (NYA_AudioCaption){ .hold_until_s = 10.0 });
      nya_assert(captions.live <= NYA_AUDIO_CAPTIONS_MAX, "the ring must never hold more than its capacity");
    }

    nya_assert_eq(captions.live, (u32)NYA_AUDIO_CAPTIONS_MAX);

    // the oldest first from `next`: the two first captions gave way, and the order is kept.
    for (u32 i = 0; i < NYA_AUDIO_CAPTIONS_MAX; i++) {
      (void)snprintf(text, sizeof(text), "c%u", i + 2);
      nya_assert(strcmp(captions.items[(captions.next + i) % NYA_AUDIO_CAPTIONS_MAX].text, text) == 0, "slot %u should read %s", i, text);
    }

    u32 next = captions.next;
    nya_audio_captions_add(&captions, "c3", (NYA_AudioCaption){ .hold_until_s = 20.0 });

    nya_assert_eq(captions.live, (u32)NYA_AUDIO_CAPTIONS_MAX);
    nya_assert_eq(captions.next, next);

    u32 repeats = 0;
    for (u32 i = 0; i < NYA_AUDIO_CAPTIONS_MAX; i++) repeats += strcmp(captions.items[i].text, "c3") == 0 ? 1 : 0;
    nya_assert_eq(repeats, 1U);

    // only the refreshed line outlives the others' hold.
    nya_audio_captions_remove(&captions, 15.0);
    nya_assert_eq(captions.live, 1U);
  }

  // TEST: a long caption is cut on a character boundary
  {
    NYA_AudioCaptions captions = { 0 };
    char              text[NYA_AUDIO_CAPTION_TEXT_MAX + 8];

    // two byte characters only, so a cut at the buffer's odd last byte lands in the middle of one.
    for (u32 i = 0; i + 1 < sizeof(text); i += 2) {
      text[i]     = (char)0xC3;
      text[i + 1] = (char)0xA4;
    }
    text[sizeof(text) - 1] = '\0';

    nya_audio_captions_add(&captions, text, (NYA_AudioCaption){ 0 });

    nya_assert_eq(strlen(captions.items[0].text), (u64)NYA_AUDIO_CAPTION_TEXT_MAX - 2);
  }

  // TEST: the quarter a direction falls in, in the mixer's frame of +x right and -z ahead
  {
    nya_assert_eq((s32)nya_audio_side((f32x3){ 0.0F, 0.0F, -1.0F }), NYA_AUDIO_SIDE_AHEAD);
    nya_assert_eq((s32)nya_audio_side((f32x3){ 1.0F, 0.0F, 0.0F }), NYA_AUDIO_SIDE_RIGHT);
    nya_assert_eq((s32)nya_audio_side((f32x3){ 0.0F, 0.0F, 1.0F }), NYA_AUDIO_SIDE_BEHIND);
    nya_assert_eq((s32)nya_audio_side((f32x3){ -1.0F, 0.0F, 0.0F }), NYA_AUDIO_SIDE_LEFT);

    // the boundaries lean to the nearer quarter, and height plays no part.
    nya_assert_eq((s32)nya_audio_side((f32x3){ 0.9F, 5.0F, -1.0F }), NYA_AUDIO_SIDE_AHEAD);
    nya_assert_eq((s32)nya_audio_side((f32x3){ 1.0F, 0.0F, -0.9F }), NYA_AUDIO_SIDE_RIGHT);
    nya_assert_eq((s32)nya_audio_side((f32x3){ -0.1F, 0.0F, 1.0F }), NYA_AUDIO_SIDE_BEHIND);
    nya_assert_eq((s32)nya_audio_side((f32x3){ -1.0F, 0.0F, 0.9F }), NYA_AUDIO_SIDE_LEFT);

    // on the listener it has no direction, and is called ahead.
    nya_assert_eq((s32)nya_audio_side((f32x3){ 0 }), NYA_AUDIO_SIDE_AHEAD);
  }

  printf("PASSED: test_audio_captions\n");
  return 0;
}
