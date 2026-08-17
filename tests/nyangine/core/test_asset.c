/**
 * The asset system: the handle registry, reference counting, and the two queues.
 *
 * Everything here runs without a GPU. Loading is queued from the caller's thread and drained on
 * NYA_EVENT_FRAME_ENDED, so a test can dispatch that event by hand and watch a load actually happen
 * — which is what separates these from tests that only check the queue got longer.
 *
 * NYA_ASSET_TYPE_TEXT is the type used throughout because it is the only one that touches neither
 * the GPU nor a decoder: _nya_asset_flush_uploads returns immediately when nothing was staged, so
 * the whole loading pass is reachable headless. The GPU backed types share every step of that path
 * up to the decode, so what is exercised here is the machinery, not just the text case.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Drains both queues, the way the end of a real frame does. */
static void end_frame(void) {
  nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
}

/** Writes `content` to `path`, so there is something on disk for an external load to find. */
static void write_file(NYA_ConstCString path, NYA_ConstCString content) {
  FILE* file = fopen(path, "wb");
  nya_assert(file != nullptr, "could not create the fixture at %s", path);
  (void)fwrite(content, 1, strlen(content), file);
  (void)fclose(file);
}

/** Where the synthesized clip lands. A real decoder needs a real file, not a buffer. */
#define SOUND_FIXTURE "./_test_asset_tone.wav"

/** A font that is actually in the repository, so the face has something valid to parse. */
#define FONT_FIXTURE "./assets/fonts/Aldrich.ttf"

/** Writes a quarter second 8 bit mono sine at 8kHz, the same shape test_audio uses. */
static void write_test_wav(void) {
  const u32 sample_rate = 8000;
  const u32 samples     = sample_rate / 4;

  u8 pcm[8000 / 4];
  for (u32 i = 0; i < samples; i++) {
    f64 t  = (f64)i / (f64)sample_rate;
    pcm[i] = (u8)(128.0 + (100.0 * sin(2.0 * M_PI * 440.0 * t)));
  }

  u8  header[44];
  u8* h = header;

#define PUT4(str) nya_memcpy(h, (str), 4), h += 4
#define PUT32(v)  { u32 _v = (v); nya_memcpy(h, &_v, 4); h += 4; }
#define PUT16(v)  { u16 _v = (u16)(v); nya_memcpy(h, &_v, 2); h += 2; }

  PUT4("RIFF");
  PUT32(36 + samples);
  PUT4("WAVE");
  PUT4("fmt ");
  PUT32(16);
  PUT16(1);
  PUT16(1);
  PUT32(sample_rate);
  PUT32(sample_rate);
  PUT16(1);
  PUT16(8);
  PUT4("data");
  PUT32(samples);

#undef PUT4
#undef PUT32
#undef PUT16

  FILE* file = fopen(SOUND_FIXTURE, "wb");
  nya_assert(file != nullptr, "could not create the fixture at %s", SOUND_FIXTURE);
  (void)fwrite(header, 1, sizeof(header), file);
  (void)fwrite(pcm, 1, samples, file);
  (void)fclose(file);
}

s32 main(void) {
  /*
   * The systems the asset system needs, rather than nya_app_init.
   *
   * A full init opens a window and brings up the renderer, neither of which a headless test can do.
   * The asset system itself needs only an arena and somewhere to register its two frame-ended
   * hooks, which is the event system, which in turn needs the callback system.
   */
  /*
   * No real audio device, ever.
   *
   * nya_system_asset_init brings up SDL_mixer, which opens the default playback device. On a
   * machine without a sound card — a CI container above all — ALSA leaks its configuration tree
   * while failing to open one, around 66 KB across 2000 allocations, and the leak sanitizer fails
   * the test over memory no nyangine code ever touched.
   *
   * The dummy driver is the honest fix rather than a suppression: this test asserts things about
   * the asset registry and the load queues, none of which involve playing a sound, so probing the
   * host's hardware was never something it needed to do.
   */
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
  b8 sdl_ok         = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();

  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  char fixture[] = "./_test_asset_fixture.txt";
  write_file(fixture, "nyangine");
  defer (void)remove(fixture);

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: an unknown handle is unloaded rather than an error
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Unloaded, not FAILED: nothing has been tried yet. A caller polling status before its load has
    // been queued should see "not here" rather than "gave up".
    nya_assert(nya_asset_status("nothing_by_this_name") == NYA_ASSET_STATUS_UNLOADED);
    nya_assert(nya_asset_get("nothing_by_this_name") == nullptr);
    nya_assert(nya_asset_reference_count("nothing_by_this_name") == 0);
    nya_assert(!nya_asset_unload("nothing_by_this_name"), "unloading something unknown does nothing");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: null handles are rejected rather than crashed on
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Error load = nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_TEXT, .handle = nullptr });
    nya_assert(load.kind == NYA_ERROR_INVALID_ARGUMENT, "a null handle is a caller mistake, not a panic");

    NYA_Error acquire = nya_asset_acquire(nullptr);
    nya_assert(acquire.kind == NYA_ERROR_INVALID_ARGUMENT);

    nya_asset_release(nullptr);              // must not crash
    nya_assert(!nya_asset_unload(nullptr));  //
    nya_assert(nya_asset_reference_count(nullptr) == 0);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: acquiring something that was never loaded fails without ending the process
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Deliberately an error rather than an assert: a typo'd handle in game code should be
    // recoverable, because assertions are live in shipping builds.
    NYA_Error result = nya_asset_acquire("never_loaded");
    nya_assert(result.kind == NYA_ERROR_NOT_FOUND);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: queueing registers the asset immediately, loading happens at frame end
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_TEXT,
      .handle   = fixture,
      .external = true,
    }));

    // Visible and LOADING before any frame has ended: the registry entry is created by the queueing
    // call, so a caller can already ask about the asset it just asked for.
    nya_assert(nya_asset_status(fixture) == NYA_ASSET_STATUS_LOADING, "queued but not yet loaded");
    nya_assert(nya_asset_get(fixture) != nullptr);

    end_frame();

    nya_assert(nya_asset_status(fixture) == NYA_ASSET_STATUS_LOADED, "the frame-ended hook is what performs the load");

    NYA_Asset* asset = nya_asset_get(fixture);
    nya_assert(asset != nullptr);
    nya_assert(asset->type == NYA_ASSET_TYPE_TEXT);
    nya_assert(asset->as_text.size == strlen("nyangine"), "got " FMTu64 " bytes", asset->as_text.size);
    nya_assert(memcmp(asset->as_text.data, "nyangine", asset->as_text.size) == 0, "the bytes on disk are the bytes in the asset");
    nya_assert(!asset->from_blob, "an external load never comes out of the blob");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: loading the same handle twice is a no-op rather than a second entry
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_Asset* before = nya_asset_get(fixture);
    u8*        data   = before->as_text.data;

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_TEXT,
      .handle   = fixture,
      .external = true,
    }));

    end_frame();

    // Same bytes at the same address: the second request returned early rather than reloading and
    // leaking the first copy.
    nya_assert(nya_asset_get(fixture)->as_text.data == data, "an already loaded asset is not loaded again");
    nya_assert(nya_asset_status(fixture) == NYA_ASSET_STATUS_LOADED);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: reference counting, and that a referenced asset cannot be unloaded
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(nya_asset_reference_count(fixture) == 0, "loading does not take a reference");

    NYA_EXPECT(nya_asset_acquire(fixture));
    NYA_EXPECT(nya_asset_acquire(fixture));
    nya_assert(nya_asset_reference_count(fixture) == 2);

    // The case that used to assert: two systems share an asset and the first one to finish must not
    // be able to pull it out from under the second.
    nya_assert(!nya_asset_unload(fixture), "still referenced, so the unload is refused");

    end_frame();
    nya_assert(nya_asset_status(fixture) == NYA_ASSET_STATUS_LOADED, "a refused unload leaves the asset alone");

    nya_asset_release(fixture);
    nya_assert(nya_asset_reference_count(fixture) == 1);
    nya_assert(nya_asset_status(fixture) == NYA_ASSET_STATUS_LOADED, "one holder left, nothing to do");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: releasing the last reference queues the unload, and frame end performs it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_assert(nya_asset_reference_count(fixture) == 1, "carried over from the test above");

    nya_asset_release(fixture);
    nya_assert(nya_asset_reference_count(fixture) == 0);
    nya_assert(nya_asset_status(fixture) == NYA_ASSET_STATUS_LOADED, "queued for unload, not yet unloaded");

    end_frame();
    nya_assert(nya_asset_status(fixture) == NYA_ASSET_STATUS_UNLOADED, "the last release is what unloads it");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: releasing more often than acquiring does not wrap the count
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // atomic_fetch_sub on zero would wrap to UINT64_MAX and leave an asset nothing could ever
    // unload. The release path compare-exchanges instead, so the count floors at zero.
    nya_assert(nya_asset_reference_count(fixture) == 0);

    nya_asset_release(fixture);
    nya_asset_release(fixture);

    nya_assert(nya_asset_reference_count(fixture) == 0, "the count floors rather than wrapping");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: acquiring between queueing and frame end cancels the unload
  // ─────────────────────────────────────────────────────────────────────────────
  {
    char revived[] = "./_test_asset_revived.txt";
    write_file(revived, "still wanted");
    defer (void)remove(revived);

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_TEXT,
      .handle   = revived,
      .external = true,
    }));
    end_frame();
    nya_assert(nya_asset_status(revived) == NYA_ASSET_STATUS_LOADED);

    nya_assert(nya_asset_unload(revived), "nothing holds it, so the unload is accepted");
    nya_assert(nya_asset_get(revived)->queued_for_unload);

    // The asset is still fully intact here: the queue is not processed until the frame ends, so
    // this is a plain revival rather than a resurrection of something already torn down.
    NYA_EXPECT(nya_asset_acquire(revived));
    nya_assert(!nya_asset_get(revived)->queued_for_unload, "acquiring cancels the pending unload");

    end_frame();
    nya_assert(nya_asset_status(revived) == NYA_ASSET_STATUS_LOADED, "the asset survived the frame it was queued in");

    nya_asset_release(revived);
    end_frame();
    nya_assert(nya_asset_status(revived) == NYA_ASSET_STATUS_UNLOADED);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a missing file fails the asset instead of the process, and stays failed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    char missing[] = "./_test_asset_definitely_absent.txt";

    // An external asset is a path from outside the game and may simply have moved, so the load
    // failing is ordinary. The engine keeps running.
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_TEXT,
      .handle   = missing,
      .external = true,
    }));

    end_frame();
    nya_assert(nya_asset_status(missing) == NYA_ASSET_STATUS_FAILED);

    // Terminal, and said out loud rather than silently doing nothing: a second request for
    // something already known to be broken is a caller bug worth surfacing.
    NYA_Error again = nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_TEXT,
      .handle   = missing,
      .external = true,
    });
    nya_assert(again.kind == NYA_ERROR_NOT_OK, "a previous failure is not retried");

    NYA_Error acquire = nya_asset_acquire(missing);
    nya_assert(acquire.kind == NYA_ERROR_NOT_OK, "a failed asset cannot be acquired");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: handles are interned, so an asset survives the caller's string going away
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Handles are string literals in the game DLL, and hot reloading unmaps the .rodata they live
    // in. The system copies every handle it keeps, so a dict keyed by content still hashes
    // something that exists after the DLL is gone. A stack buffer that is then overwritten stands
    // in for that unmapping.
    char handle[64];
    (void)snprintf(handle, sizeof(handle), "./_test_asset_interned.txt");
    write_file(handle, "interned");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_TEXT,
      .handle   = handle,
      .external = true,
    }));
    end_frame();

    NYA_Asset* asset = nya_asset_get("./_test_asset_interned.txt");
    nya_assert(asset != nullptr, "the asset is found by an equal string, not by the same pointer");
    nya_assert(asset->handle != handle, "the system kept a copy rather than the caller's pointer");
    nya_assert(nya_asset_status("./_test_asset_interned.txt") == NYA_ASSET_STATUS_LOADED);

    memset(handle, 'x', sizeof(handle));
    nya_assert(nya_asset_status("./_test_asset_interned.txt") == NYA_ASSET_STATUS_LOADED, "the copy is unaffected by the caller's buffer");

    (void)remove("./_test_asset_interned.txt");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: deinit unloads whatever is still registered
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Left loaded on purpose. The deinit defer at the top of main is what tears this down, and the
    // leak sanitizer is what checks it did: an asset still holding its bytes when the arena goes
    // away would be reported on exit.
    char leaked[] = "./_test_asset_left_loaded.txt";
    write_file(leaked, "cleaned up by deinit");

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_TEXT,
      .handle   = leaked,
      .external = true,
    }));
    end_frame();
    nya_assert(nya_asset_status(leaked) == NYA_ASSET_STATUS_LOADED);

    (void)remove(leaked);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a sound decodes, both streamed and predecoded
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The first type here other than text, and the reason that matters: the loading pass is a
     * dispatch over NYA_AssetType, and a suite that only ever loads text leaves every other arm of
     * it unexecuted. Sound is the one that reaches a real decoder without a GPU.
     *
     * The dummy audio driver still produces a mixer, so this is the true decode path rather than a
     * stub — MIX_LoadAudio actually parses the RIFF header written above.
     */
    write_test_wav();
    defer (void)remove(SOUND_FIXTURE);

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_SOUND,
      .handle   = SOUND_FIXTURE,
      .external = true,
      .as_sound = { .predecode = true },
    }));
    end_frame();

    NYA_Asset* predecoded = nya_asset_get(SOUND_FIXTURE);
    nya_assert(predecoded != nullptr, "the sound was never registered");

    /*
     * Guarded on the decode having worked, not asserted outright.
     *
     * A machine with no usable mixer fails the load, and that is the documented behaviour rather
     * than a bug — the same shape test_audio uses. What is unconditional is that the asset exists
     * and carries the type it was asked for.
     */
    nya_assert(predecoded->type == NYA_ASSET_TYPE_SOUND, "the asset came back as the wrong type");

    if (predecoded->status == NYA_ASSET_STATUS_LOADED) {
      nya_assert(predecoded->as_sound.audio != nullptr, "a loaded sound must carry decoded audio");
    } else {
      nya_assert(predecoded->status == NYA_ASSET_STATUS_FAILED, "a sound is either loaded or failed, never left pending");
      nya_info("no usable mixer, so the sound decode was not exercised (expected in some CI images)");
    }

    /*
     * The same file again, streamed rather than predecoded, under its own handle.
     *
     * `source` is what makes that possible: the handle is the identity and the source is the file,
     * so one file can back two assets with different load parameters. That is the case the field
     * was added for — a font at two point sizes — and nothing tested it.
     */
    char streamed_handle[] = "sound:streamed";

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_SOUND,
      .handle   = streamed_handle,
      .source   = SOUND_FIXTURE,
      .external = true,
      .as_sound = { .predecode = false },
    }));
    end_frame();

    NYA_Asset* streamed = nya_asset_get(streamed_handle);
    nya_assert(streamed != nullptr, "a source backed asset must register under its own handle");
    nya_assert(streamed != predecoded, "two handles over one file must be two assets");
    nya_assert(nya_asset_get(SOUND_FIXTURE) == predecoded, "the original handle must be untouched");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: one font file at two point sizes is two assets
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The case `source` exists for, spelled out in its own documentation: a face carries no size, so
     * a .ttf at two sizes cannot be keyed on the path alone. Font loading is CPU side — an atlas
     * needs a GPU, opening the face does not — so it is reachable here.
     */
    char small[] = "font:aldrich@12";
    char large[] = "font:aldrich@48";

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_FONT,
      .handle   = small,
      .source   = FONT_FIXTURE,
      .external = true,
      .as_font  = { .point_size = 12.0F },
    }));

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_FONT,
      .handle   = large,
      .source   = FONT_FIXTURE,
      .external = true,
      .as_font  = { .point_size = 48.0F },
    }));
    end_frame();

    NYA_Asset* small_asset = nya_asset_get(small);
    NYA_Asset* large_asset = nya_asset_get(large);

    nya_assert(small_asset != nullptr && large_asset != nullptr, "both sizes must register");
    nya_assert(small_asset != large_asset, "two sizes of one face must not collapse into one asset");

    // Guarded the same way as the sound: a build without SDL_ttf warns and fails the load rather
    // than refusing to start, so the type is what is asserted unconditionally.
    nya_assert(small_asset->type == NYA_ASSET_TYPE_FONT);
    nya_assert(large_asset->type == NYA_ASSET_TYPE_FONT);

    if (small_asset->status == NYA_ASSET_STATUS_LOADED && large_asset->status == NYA_ASSET_STATUS_LOADED) {
      nya_assert(small_asset->as_font.font != nullptr, "a loaded font must carry a face");
      nya_assert(large_asset->as_font.font != nullptr);
      nya_assert(small_asset->as_font.font != large_asset->as_font.font, "each size needs its own face");
    }
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a GPU backed type fails rather than crashing without a device
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * Headless is a supported configuration, not merely the state of this test — NYA_HEADLESS is a
     * documented build flag, and CI runs the suite that way. So asking for a texture with no device
     * has to be an ordinary failed asset rather than a fault, and the failure has to be reached
     * through the same queue as everything else.
     */
    char texture[] = "./_test_asset_texture.png";
    write_file(texture, "not really a png");
    defer (void)remove(texture);

    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type     = NYA_ASSET_TYPE_TEXTURE,
      .handle   = texture,
      .external = true,
    }));
    end_frame();

    NYA_Asset* asset = nya_asset_get(texture);
    nya_assert(asset != nullptr, "even a doomed load must register its handle");
    nya_assert(asset->status == NYA_ASSET_STATUS_FAILED, "a texture without a device must fail, got status %d", (s32)asset->status);

    // And a failed asset stays failed rather than being retried forever by the queue.
    end_frame();
    nya_assert(nya_asset_status(texture) == NYA_ASSET_STATUS_FAILED, "a failed asset must not be requeued");
  }

  printf("PASSED: test_asset\n");
  return 0;
}
