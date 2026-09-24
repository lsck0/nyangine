/**
 * Reading an FBX into NYA_ASSET_TYPE_MESH.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "genyarated/assets.h"

#include "SDL3/SDL_init.h"

#include <math.h>

/** Drains the loading queue, the way the end of a real frame does. */
static void end_frame(void) {
  nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
}

/**
 * Asserts a mesh is unit-sized and centred, and reports the box it actually occupies.
 * */
static void assert_unit_sized(NYA_ConstCString name, const NYA_Asset* asset) {
  f32x3 lo = asset->as_mesh.positions[0];
  f32x3 hi = asset->as_mesh.positions[0];

  for (u32 i = 1; i < asset->as_mesh.vertex_count; i++) {
    f32x3 v = asset->as_mesh.positions[i];

    lo = (f32x3){ nya_min(lo.x, v.x), nya_min(lo.y, v.y), nya_min(lo.z, v.z) };
    hi = (f32x3){ nya_max(hi.x, v.x), nya_max(hi.y, v.y), nya_max(hi.z, v.z) };
  }

  nya_log_info("  %s: %u triangles, bounds %.2f %.2f %.2f .. %.2f %.2f %.2f", name, asset->as_mesh.vertex_count / 3, (f64)lo.x, (f64)lo.y,
           (f64)lo.z, (f64)hi.x, (f64)hi.y, (f64)hi.z);

  f32 extent = nya_max(hi.x - lo.x, nya_max(hi.y - lo.y, hi.z - lo.z));

  // Wide bounds deliberately: this is catching "a hundred times too big" and "a hundred times too
  // small", not a re-export that moved a vertex.
  nya_assert(extent > 0.5F && extent < 8.0F, "%s spans %f units, which the scene's scale of one does not suit", name, (f64)extent);

  // Centred on its own origin, which is what makes GNY_CUBE3D_MODEL_LIFT a single number rather than
  // something derived per model.
  nya_assert(fabsf(lo.y + hi.y) < extent * 0.5F, "%s is not centred on y: %f to %f", name, (f64)lo.y, (f64)hi.y);
}

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();

  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  // TEST: the model in the tree parses into triangles
  {
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_MESH, .handle = NYA_ASSET_MODELS_CUBIE_FBX }));

    // Queued, not loaded. The read happens at the end of the frame, which is why
    // nya_render3d_mesh draws nothing rather than asserting when it is asked too early.
    nya_assert(nya_asset_status(NYA_ASSET_MODELS_CUBIE_FBX) != NYA_ASSET_STATUS_LOADED, "the load is queued, not immediate");

    end_frame();

    NYA_Asset* asset = nya_asset_get(NYA_ASSET_MODELS_CUBIE_FBX);

    nya_assert(asset != nullptr, "the mesh asset exists");
    nya_assert(asset->status == NYA_ASSET_STATUS_LOADED, "and loaded, status %d", (int)asset->status);
    nya_assert(asset->type == NYA_ASSET_TYPE_MESH);

    u32 vertices = asset->as_mesh.vertex_count;

    nya_assert(vertices > 0, "it has geometry: %u vertices", vertices);
    nya_assert(vertices % 3 == 0, "triangulated and de-indexed, so the vertex count divides by three, got %u", vertices);

    nya_assert(asset->as_mesh.positions != nullptr && asset->as_mesh.normals != nullptr, "the geometry arrays are present");

    assert_unit_sized("Cubie.fbx", asset);

    /* UVs and the material, both of which were dropped on the floor until textures were wanted. */
    nya_assert(asset->as_mesh.uvs != nullptr, "the model's UV set was read");

    // whether the reservation and the write agreed. Not required, since teardown frees the reserved
    // extent, but worth knowing for these files.
    nya_log_info("  Cubie.fbx wrote %u of %u reserved vertices", vertices, asset->as_mesh.allocated);

    u32 in_unit_range = 0;

    for (u32 i = 0; i < vertices; i++) {
      f32x2 uv = asset->as_mesh.uvs[i];
      if (uv.x >= -0.01F && uv.x <= 1.01F && uv.y >= -0.01F && uv.y <= 1.01F) in_unit_range++;
    }

    // Not all: an atlas is often addressed with wrapped coordinates outside the unit square. Most.
    nya_assert(in_unit_range > vertices / 2, "only %u of %u UVs are in the unit square", in_unit_range, vertices);

    /* The embedded texture, decoded and on the GPU. */
    /* Parts, one per material, each a contiguous run of the index buffer. */
    nya_assert(asset->as_mesh.part_count > 0, "the model has at least one part");
    nya_assert(asset->as_mesh.part_count <= asset->as_mesh.part_capacity, "part_count is within what was reserved");

    u32 covered = 0;

    for (u32 i = 0; i < asset->as_mesh.part_count; i++) {
      const NYA_MeshPart* part = &asset->as_mesh.parts[i];

      nya_assert(part->vertex_count > 0, "part %u is not empty; an empty part is a draw that renders nothing", i);
      nya_assert(part->first_vertex == covered, "part %u starts at vertex %u rather than %u, so the parts do not tile", i, part->first_vertex,
                 covered);
      nya_assert((u64)part->first_vertex + part->vertex_count <= asset->as_mesh.vertex_count, "part %u runs past the geometry", i);

      // -1 is "untextured", which is legal; anything else has to name a texture that exists.
      nya_assert(part->texture >= -1 && part->texture < (s32)asset->as_mesh.texture_count, "part %u names texture %d of %u", i,
                 part->texture, asset->as_mesh.texture_count);

      // A zeroed base colour would multiply the whole part black, which reads as a lighting bug.
      nya_assert(part->base_color.a > 0.0F, "part %u has a non-zero material alpha", i);

      covered += part->vertex_count;
    }

    nya_assert(covered == asset->as_mesh.vertex_count, "the parts cover %u of %u vertices", covered, asset->as_mesh.vertex_count);

    if (nya_app_get()->render_system.gpu_device != nullptr) {
      nya_assert(asset->as_mesh.texture_count > 0, "the texture embedded in the FBX was decoded and uploaded");
    } else {
      /* Headless, so nothing could be uploaded and it must not be asserted. */
      nya_assert(asset->as_mesh.texture_count == 0, "no GPU, so no texture was uploaded");
    }

    nya_log_info("  Cubie.fbx: %u parts, %u textures, part 0 base colour %.2f %.2f %.2f", asset->as_mesh.part_count,
             asset->as_mesh.texture_count, (f64)asset->as_mesh.parts[0].base_color.r, (f64)asset->as_mesh.parts[0].base_color.g,
             (f64)asset->as_mesh.parts[0].base_color.b);


    /* Normals are unit length. */
    for (u32 i = 0; i < vertices; i++) {
      f32x3 normal = asset->as_mesh.normals[i];
      f32   length = sqrtf((normal.x * normal.x) + (normal.y * normal.y) + (normal.z * normal.z));

      nya_assert(length > 0.9F && length < 1.1F, "normal %u has length %f rather than one", i, (f64)length);
    }

    printf("  PASSED\n");
  }

  // TEST: the second model reads too, and the two are different meshes
  {
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_MESH, .handle = NYA_ASSET_MODELS_PILL_FBX }));

    end_frame();

    NYA_Asset* pill  = nya_asset_get(NYA_ASSET_MODELS_PILL_FBX);
    NYA_Asset* cubie = nya_asset_get(NYA_ASSET_MODELS_CUBIE_FBX);

    nya_assert(pill != nullptr && pill->status == NYA_ASSET_STATUS_LOADED, "the second model loads as well");
    nya_assert(pill->as_mesh.vertex_count % 3 == 0 && pill->as_mesh.vertex_count > 0);

    assert_unit_sized("pill.fbx", pill);

    // Two handles, two meshes. Sharing a buffer between assets would show up here as identical counts
    // and identical pointers, which is what a loader that wrote into a single static would produce.
    nya_assert(pill->as_mesh.positions != cubie->as_mesh.positions, "each model owns its own arrays");
    nya_assert(pill->as_mesh.parts != cubie->as_mesh.parts, "and its own parts");

    printf("  PASSED\n");
  }

  // TEST: a rigged model's skinned vertices carry their part's material colour
  {
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){
      .type   = NYA_ASSET_TYPE_MESH,
      .handle = NYA_ASSET_MODELS_BENDER_FBX,
    }));

    end_frame();

    NYA_Asset* asset = nya_asset_get(NYA_ASSET_MODELS_BENDER_FBX);
    nya_check(asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED, "the rigged model loads");

    if (asset != nullptr && asset->status == NYA_ASSET_STATUS_LOADED) {
      nya_check(asset->as_mesh.skeleton != nullptr, "and has a skeleton, or it is not the rigged one");
      nya_check(asset->as_mesh.skinned_vertices != nullptr, "and a skinned copy of its geometry");

      if (asset->as_mesh.skinned_vertices != nullptr) {
        /*
         * The skinned buffer is uploaded straight from the loader and never passes through the staging
         * that folds a part's base colour into the static vertices, so every skinned vertex used to be
         * white whatever material it belonged to. Checked per part rather than in bulk: a single-part
         * model would pass a bulk check by accident.
         */
        u32 matched = 0;

        for (u32 p = 0; p < asset->as_mesh.part_count; p++) {
          const NYA_MeshPart* part = &asset->as_mesh.parts[p];
          const u32           end  = part->first_vertex + part->vertex_count;

          for (u32 v = part->first_vertex; v < end && v < asset->as_mesh.vertex_count; v++) {
            const NYA_VertexSkinned3D* vertex = &asset->as_mesh.skinned_vertices[v];

            // f16 on the way in, so the comparison has to allow what that rounding costs.
            if (fabsf((f32)vertex->color[0] - part->base_color.r) > 0.01F) continue;
            if (fabsf((f32)vertex->color[1] - part->base_color.g) > 0.01F) continue;
            if (fabsf((f32)vertex->color[2] - part->base_color.b) > 0.01F) continue;

            matched++;
          }
        }

        nya_check(matched == asset->as_mesh.vertex_count, "every skinned vertex carries its part's colour, %u of %u", matched,
                  asset->as_mesh.vertex_count);

        /*
         * Worth knowing what this is worth: bender.fbx is one part with a white material, so the check
         * above passes whether or not the colour is folded in. It guards the rule rather than proving
         * it, and it will start proving it the day a rigged model with materials is in the tree. The
         * one thing it does catch today is a vertex left at zero, which would draw the model black.
         */
        nya_check(asset->as_mesh.parts[0].base_color.a > 0.0F, "and the part it came from is not transparent");
      }

      nya_log_info("  bender.fbx: %u parts, %u vertices, part 0 base colour %.2f %.2f %.2f", asset->as_mesh.part_count,
                   asset->as_mesh.vertex_count, (f64)asset->as_mesh.parts[0].base_color.r, (f64)asset->as_mesh.parts[0].base_color.g,
                   (f64)asset->as_mesh.parts[0].base_color.b);
    }

    printf("  PASSED\n");
  }

  // TEST: something that is not an FBX fails rather than being believed
  {
    // a real file of the wrong kind, so ufbx rejects the contents rather than the filesystem the name.
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_MESH, .handle = NYA_ASSET_I18N_EN_JSON }));

    end_frame();

    NYA_Asset* asset = nya_asset_get(NYA_ASSET_I18N_EN_JSON);

    nya_assert(asset != nullptr, "the entry exists");

    /* FAILED, and nothing left half built. */
    nya_assert(asset->status == NYA_ASSET_STATUS_FAILED, "a JSON file is not a model, status %d", (int)asset->status);
    nya_assert(asset->as_mesh.vertex_count == 0, "and it reports no geometry");

    printf("  PASSED\n");
  }

  nya_log_info("PASSED: test_asset_mesh (0 failures)");

  return EXIT_SUCCESS;
}
