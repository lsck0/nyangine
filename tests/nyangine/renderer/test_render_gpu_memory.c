/**
 * GPU memory counting: texture sizes from their create info, and the handle table behind the byte counts,
 * including releases that shift colliding entries back.
 **/

#include "nyangine-core/nyangine.c"
#include "nyangine-core/nyangine.h"

/** Fake handles, never dereferenced. Aligned like real allocations so they spread over the table. */
static const void* fake_handle(u64 index) {
    return (const void*)(uintptr_t)(0x10000ULL + (index * 16ULL));
}

s32 main(void) {
    // Texture sizes follow the format, mips, layers and samples.
    {
        SDL_GPUTextureCreateInfo atlas = {
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = SDL_GPU_TEXTUREFORMAT_R8_UNORM,
            .width                = 464,
            .height               = 216,
            .layer_count_or_depth = 1,
            .num_levels           = 1,
        };
        nya_check(nya_gpu_texture_bytes(&atlas) == 464ULL * 216ULL, "an R8 texture is one byte a texel");

        SDL_GPUTextureCreateInfo msaa = atlas;
        msaa.format                   = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        msaa.sample_count             = SDL_GPU_SAMPLECOUNT_4;
        nya_check(nya_gpu_texture_bytes(&msaa) == 464ULL * 216ULL * 4ULL * 4ULL, "four samples of four bytes");

        SDL_GPUTextureCreateInfo mipped = {
            .type                 = SDL_GPU_TEXTURETYPE_2D,
            .format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .width                = 4,
            .height               = 2,
            .layer_count_or_depth = 1,
            .num_levels           = 3,
        };
        nya_check(nya_gpu_texture_bytes(&mipped) == (8ULL + 2ULL + 1ULL) * 4ULL, "4x2, 2x1 and 1x1, never below one texel");

        SDL_GPUTextureCreateInfo cube = mipped;
        cube.type                     = SDL_GPU_TEXTURETYPE_CUBE;
        cube.width                    = 2;
        cube.height                   = 2;
        cube.layer_count_or_depth     = 6;
        cube.num_levels               = 2;
        nya_check(nya_gpu_texture_bytes(&cube) == (4ULL + 1ULL) * 4ULL * 6ULL, "every face keeps its mips");
    }

    // Counts follow tracks and untracks, by kind.
    {
        nya_check(nya_gpu_memory_bytes(NYA_GPU_MEMORY_TEXTURE) == 0, "nothing counted yet");

        _nya_gpu_memory_track(NYA_GPU_MEMORY_TEXTURE, fake_handle(1), 1000);
        _nya_gpu_memory_track(NYA_GPU_MEMORY_BUFFER, fake_handle(2), 200);
        _nya_gpu_memory_track(NYA_GPU_MEMORY_TRANSFER, fake_handle(3), 30);

        nya_check(nya_gpu_memory_bytes(NYA_GPU_MEMORY_TEXTURE) == 1000, "the texture is counted as a texture");
        nya_check(nya_gpu_memory_bytes(NYA_GPU_MEMORY_BUFFER) == 200, "the buffer as a buffer");
        nya_check(nya_gpu_memory_bytes(NYA_GPU_MEMORY_TRANSFER) == 30, "the transfer buffer as a transfer buffer");
        nya_check(_nya_gpu_memory.count == 3, "three objects counted");

        // a second track of a live handle would double count it.
        _nya_gpu_memory_track(NYA_GPU_MEMORY_TEXTURE, fake_handle(1), 1000);
        nya_check(nya_gpu_memory_bytes(NYA_GPU_MEMORY_TEXTURE) == 1000, "a handle is counted once");

        _nya_gpu_memory_untrack(fake_handle(2));
        nya_check(nya_gpu_memory_bytes(NYA_GPU_MEMORY_BUFFER) == 0, "a release uncounts what the create counted");

        // created while the table was full, so never counted.
        _nya_gpu_memory_untrack(fake_handle(99));
        nya_check(_nya_gpu_memory.count == 2, "an unknown handle is ignored");

        _nya_gpu_memory_untrack(fake_handle(1));
        _nya_gpu_memory_untrack(fake_handle(3));
        nya_check(_nya_gpu_memory.count == 0 && nya_gpu_memory_bytes(NYA_GPU_MEMORY_TEXTURE) == 0 && nya_gpu_memory_bytes(NYA_GPU_MEMORY_TRANSFER) == 0,
                  "releasing everything returns every count to zero");
    }

    // Released in a scrambled order, every entry stays findable: the backward shift keeps probe runs intact.
    {
        const u64 count = NYA_GPU_MEMORY_TRACKED_MAX;

        for (u64 i = 0; i < count; i++) _nya_gpu_memory_track(NYA_GPU_MEMORY_BUFFER, fake_handle(i), i + 1);

        nya_check(_nya_gpu_memory.count == NYA_GPU_MEMORY_TRACKED_MAX, "filled to the ceiling");
        nya_check(nya_gpu_memory_bytes(NYA_GPU_MEMORY_BUFFER) == (count * (count + 1)) / 2, "every size added up");

        // past the ceiling the object goes uncounted rather than failing.
        _nya_gpu_memory_track(NYA_GPU_MEMORY_BUFFER, fake_handle(count), 1);
        nya_check(_nya_gpu_memory.count == NYA_GPU_MEMORY_TRACKED_MAX && _nya_gpu_memory.full_warned, "a full table refuses and warns");

        // odd indices first, then even: each release leaves holes the next lookup has to probe past.
        u64 expected = nya_gpu_memory_bytes(NYA_GPU_MEMORY_BUFFER);
        for (u64 parity = 1; parity <= 2; parity++) {
            for (u64 i = parity % 2; i < count; i += 2) {
                _nya_gpu_memory_untrack(fake_handle(i));
                expected -= i + 1;
                nya_check(nya_gpu_memory_bytes(NYA_GPU_MEMORY_BUFFER) == expected, "entry %llu was lost by an earlier release", (unsigned long long)i);
            }
        }

        nya_check(_nya_gpu_memory.count == 0 && expected == 0, "everything released");

        for (u32 i = 0; i < NYA_GPU_MEMORY_TRACKED_MAX * 2; i++) nya_check(_nya_gpu_memory.slots[i].handle == nullptr, "no slot left behind");
    }

    // The counts are published as gauges once anything is counted.
    {
        u32 found = 0;
        for (u32 i = 0; i < nya_gauge_count(); i++) {
            if (nya_string_equals(nya_gauge_name_at(i), "gpu_textures")) found++;
            if (nya_string_equals(nya_gauge_name_at(i), "gpu_buffers")) found++;
            if (nya_string_equals(nya_gauge_name_at(i), "gpu_transfer")) found++;
        }
        nya_check(found == 3, "the three kinds should be registered as gauges");
    }

    return nya_check_failures() == 0 ? 0 : 1;
}
