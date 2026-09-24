/**
 * Audio: what each bus effect costs the mixer's callback, and what propagation costs a frame against real physics.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "SDL3/SDL_init.h"

/** Ten milliseconds of stereo at 48 kHz, about what SDL_mixer hands a callback. */
#define FRAMES 480

static const SDL_AudioSpec STEREO = { .format = SDL_AUDIO_F32, .channels = 2, .freq = 48000 };

static f32 source[FRAMES * 2];
static f32 buffer[FRAMES * 2];

static NYA_AudioChain* chain_with(NYA_Arena* arena, NYA_AudioEffects effects, const NYA_AudioReflections* reflections) {
    NYA_AudioChain*        chain    = _nya_audio_chain_create(arena);
    NYA_AudioChainSettings settings = { .effects = _nya_audio_effects_validate(effects) };

    if (reflections != nullptr) settings.reflections = *reflections;

    _nya_audio_chain_publish(chain, &settings, arena);

    // settled, so the numbers are a running unit rather than one easing in.
    for (u32 i = 0; i < 200; i++) {
        nya_memcpy(buffer, source, sizeof(buffer));
        _nya_audio_chain_process(chain, &STEREO, buffer, FRAMES * 2);
    }

    return chain;
}

s32 main(void) {
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };
    b8 sdl_ok         = SDL_Init(0);
    nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

    nya_system_callback_init();
    defer nya_system_callback_deinit();

    NYA_Arena* arena = nya_arena_create(.name = "bench_audio");
    defer      nya_arena_destroy(arena);

    for (u32 i = 0; i < FRAMES * 2; i++) source[i] = sinf((f32)i * 0.05F) * 0.5F + (nya_ihash2((s32)i, 7, 11) * 0.2F);

    NYA_AudioReflections echoes = { 0 };
    for (u32 tap = 0; tap < NYA_AUDIO_REFLECTION_TAPS; tap++) {
        echoes.taps[tap] = (NYA_AudioReflectionTap){ .delay_s = 0.01F + (0.03F * (f32)tap), .gain = 0.2F, .pan = (f32)tap / 3.0F - 1.0F, .lowpass_hz = 4000.0F };
    }

    struct {
        NYA_ConstCString            name;
        NYA_AudioEffects            effects;
        const NYA_AudioReflections* reflections;
    } units[] = {
        { "no chain settings (bypass)", { 0 }, nullptr },
        { "low + high pass", { .pass = { .lowpass_hz = 4000.0F, .highpass_hz = 80.0F } }, nullptr },
        { "equaliser, three bands", { .equalizer = { .low_db = 3.0F, .mid_db = -2.0F, .high_db = 2.0F } }, nullptr },
        { "compressor", { .compressor = { .enabled = true } }, nullptr },
        { "echo", { .echo = { .enabled = true } }, nullptr },
        { "reflections, six taps", { 0 }, &echoes },
        { "reverb", { .reverb = { .room_size = 0.7F, .damping = 0.4F } }, nullptr },
        { "limiter", { .limiter = { .enabled = true } }, nullptr },
        {
            "everything",
            {
                .pass       = { .lowpass_hz = 4000.0F, .highpass_hz = 80.0F },
                .equalizer  = { .low_db = 3.0F, .mid_db = -2.0F, .high_db = 2.0F },
                .compressor = { .enabled = true },
                .echo       = { .enabled = true },
                .reverb     = { .room_size = 0.7F, .damping = 0.4F },
                .limiter    = { .enabled = true },
            },
            &echoes,
        },
    };

    nya_bench_begin("audio effects, one 480 frame stereo buffer at 48 kHz (10 ms of audio)");

    for (u32 u = 0; u < nya_carray_length(units); u++) {
        NYA_AudioChain* chain = chain_with(arena, units[u].effects, units[u].reflections);

        nya_bench(units[u].name, FRAMES, {
            nya_memcpy(buffer, source, sizeof(buffer));
            _nya_audio_chain_process(chain, &STEREO, buffer, FRAMES * 2);
            nya_bench_keep(buffer[0]);
        });
    }

    if (nya_bench_end() != 0) return 1;

    /* Propagation against Box3D: a walled yard with a wall across it, sixteen voices behind the wall, through the same adapter the app installs. */
    NYA_World* world = nya_world_create();
    (void)nya_world_set(world);
    defer nya_world_destroy(world);

    const f32x3 walls[][2] = {
        { { 0.0F, -0.5F, 0.0F }, { 60.0F, 1.0F, 60.0F } },  { { 0.0F, 3.0F, -30.0F }, { 60.0F, 6.0F, 1.0F } },
        { { 0.0F, 3.0F, 30.0F }, { 60.0F, 6.0F, 1.0F } },   { { -30.0F, 3.0F, 0.0F }, { 1.0F, 6.0F, 60.0F } },
        { { 30.0F, 3.0F, 0.0F }, { 1.0F, 6.0F, 60.0F } },   { { -4.0F, 2.0F, -8.0F }, { 20.0F, 4.0F, 0.5F } },
    };

    for (u64 i = 0; i < nya_carray_length(walls); i++) {
        NYA_EntityHandle wall = nya_entity_spawn(.name = "wall", .position = walls[i][0]);
        nya_assert(nya_physics3d_body_attach(wall, .type = NYA_PHYSICS_BODY_STATIC, .shape = NYA_PHYSICS3D_SHAPE_BOX, .size = walls[i][1]));
    }

    nya_system_physics3d_update(1.0F / 60.0F);

    NYA_AudioEmitter crowd[NYA_AUDIO_VOICES];
    for (u32 i = 0; i < NYA_AUDIO_VOICES; i++) crowd[i] = (NYA_AudioEmitter){ .position = { (f32)i - 8.0F, 1.0F, -14.0F }, .active = true };

    static NYA_AudioTracer tracer;

    const f32x3 ear   = { 0.0F, 1.7F, 0.0F };
    const f32x3 right = { 1.0F, 0.0F, 0.0F };

    struct {
        NYA_ConstCString     name;
        NYA_AudioPropagation propagation;
    } setups[] = {
        { "occlusion only, 64 rays", { .enabled = true } },
        { "everything, 64 rays", { .enabled = true, .diffraction = true, .environment = true, .reflections = 0.3F } },
        { "everything, 256 rays", { .enabled = true, .diffraction = true, .environment = true, .reflections = 0.3F, .ray_budget = 256 } },
    };

    nya_bench_begin("audio propagation, one frame, 16 voices behind a wall, Box3D rays");

    for (u32 s = 0; s < nya_carray_length(setups); s++) {
        NYA_AudioPropagation propagation = _nya_audio_propagation_validate(setups[s].propagation);

        _nya_audio_tracer_reset(&tracer);

        nya_bench(setups[s].name, 0, {
            _nya_audio_tracer_step(&tracer, &propagation, _nya_app_audio_rays_3d, nullptr, ear, right, crowd, 1.0F / 60.0F);
            nya_bench_keep(tracer.rays_cast);
        });

        nya_log_info("%s: %u rays a frame.", setups[s].name, tracer.rays_cast);
    }

    return nya_bench_end();
}
