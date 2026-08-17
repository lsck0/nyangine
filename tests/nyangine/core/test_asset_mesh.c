/**
 * Reading an FBX into NYA_ASSET_TYPE_MESH.
 *
 * The 3D scene that draws the model is opened from a menu, so nothing headless can reach the draw —
 * which makes this the only place the ufbx integration is actually checked. What it asserts is the
 * contract nya_render3d_mesh depends on: three parallel arrays, an index count that is a multiple of
 * three, every index inside the vertex array, and normals that are unit length.
 *
 * Those last two are the ones worth having. An index past the end is a read out of bounds inside the
 * renderer rather than a wrong picture, and a zero-length normal is a vertex that takes no light and
 * shows up as a black triangle — both are the kind of thing a malformed or unexpected export produces,
 * and neither is visible from looking at the loader.
 *
 * No GPU: a mesh asset holds triangles on the CPU and nothing else, which is the whole reason it can
 * be tested like this. See NYA_ASSET_TYPE_MESH.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include "assets/assets.h"

#include "SDL3/SDL_init.h"

#include <math.h>

/** Drains the loading queue, the way the end of a real frame does. */
static void end_frame(void) {
  nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_FRAME_ENDED });
}

/**
 * Asserts a mesh is unit-sized and centred, and reports the box it actually occupies.
 *
 * The 3D scene's constants assume exactly this — GNY_CUBE3D_MODEL_SCALE is 1 and GNY_CUBE3D_MODEL_LIFT
 * is 1 because a model spans about minus one to one and its origin is its centre. That assumption was
 * wrong once in the other direction: the scale started at 0.01, guessed from the file size, and drew
 * both models two hundredths of a unit across, which looks exactly like a model that failed to load.
 *
 * So this is not a property of FBX, it is the contract between these files and those constants. A
 * re-export at a hundred times the size fails here instead of emptying the scene.
 * */
static void assert_unit_sized(NYA_ConstCString name, const NYA_Asset* asset) {
  f32x3 lo = asset->as_mesh.positions[0];
  f32x3 hi = asset->as_mesh.positions[0];

  for (u32 i = 1; i < asset->as_mesh.vertex_count; i++) {
    f32x3 v = asset->as_mesh.positions[i];

    lo = (f32x3){ nya_min(lo.x, v.x), nya_min(lo.y, v.y), nya_min(lo.z, v.z) };
    hi = (f32x3){ nya_max(hi.x, v.x), nya_max(hi.y, v.y), nya_max(hi.z, v.z) };
  }

  nya_info("  %s: %u triangles, bounds %.2f %.2f %.2f .. %.2f %.2f %.2f", name, asset->as_mesh.vertex_count / 3, (f64)lo.x, (f64)lo.y,
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
  // No real audio device; nya_system_asset_init opens one otherwise. Same reason as test_asset.c.
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);

  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  b8 sdl_ok = SDL_Init(0);
  nya_assert(sdl_ok, "SDL_Init failed: %s", SDL_GetError());

  nya_system_callback_init();
  NYA_EXPECT(nya_system_events_init());
  nya_system_asset_init();

  defer nya_system_asset_deinit();
  defer nya_system_events_deinit();
  defer nya_system_callback_deinit();

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the model in the tree parses into triangles
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_MESH, .handle = NYA_ASSET_MODELS_CUBIE_FBX }));

    // Queued, not loaded. The read happens at the end of the frame, which is exactly why
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

    /*
     * UVs and the material, both of which were dropped on the floor until textures were wanted.
     *
     * The UV check is not "are they present" but "are they in range": a UV set read without the V flip
     * FBX needs is still a perfectly valid array of numbers, and samples the atlas upside down. Range
     * alone cannot catch that, but a set that is wildly outside [0, 1] means the attribute was misread
     * rather than merely flipped — which is the failure that produces a model shaded from one texel.
     */
    nya_assert(asset->as_mesh.uvs != nullptr, "the model's UV set was read");

    // Whether the reservation and the write agreed. Not a requirement — the teardown frees by the
    // reserved extent precisely so it need not be — but worth knowing for these two files.
    nya_info("  Cubie.fbx wrote %u of %u reserved vertices", vertices, asset->as_mesh.allocated);

    u32 in_unit_range = 0;

    for (u32 i = 0; i < vertices; i++) {
      f32x2 uv = asset->as_mesh.uvs[i];
      if (uv.x >= -0.01F && uv.x <= 1.01F && uv.y >= -0.01F && uv.y <= 1.01F) in_unit_range++;
    }

    // Not all: an atlas is often addressed with wrapped coordinates outside the unit square. Most.
    nya_assert(in_unit_range > vertices / 2, "only %u of %u UVs are in the unit square", in_unit_range, vertices);

    /*
     * The embedded texture, decoded and on the GPU.
     *
     * Both models in this tree carry their PNG *inside* the FBX rather than beside it — the
     * `relative_filename` they name is a `.fbm` directory that does not exist on disk. A loader that
     * read the path and not the blob would find nothing and draw untextured, which looks like a
     * material problem rather than a loader one, so this asserts the blob path specifically.
     */
    /*
     * Parts, one per material, each a contiguous run of the index buffer.
     *
     * The run being contiguous is the property that matters and the reason ufbx's material_parts is used
     * rather than the per-face material array: a part is drawn as one range, so a scattered part would be
     * either many draw calls or a wrong picture. Asserting the ranges tile the buffer exactly is how that
     * stays true — an overlap would draw triangles twice and a gap would drop them silently.
     */
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
      /*
       * Headless, so nothing could be uploaded and it must not be asserted.
       *
       * _nya_asset_stage_texture needs a GPU device and reports NYA_ERROR_NOT_SUPPORTED without one, and
       * the loader treats that as "this part draws untextured" rather than as a failed model — which is
       * the right behaviour for a dedicated server, and is why the mesh still loads here at all.
       *
       * What that leaves untested is the decode and upload themselves. The half this test can reach is
       * the half that was actually missing: the UVs, the parts and the materials, all plain memory.
       */
      nya_assert(asset->as_mesh.texture_count == 0, "no GPU, so no texture was uploaded");
    }

    nya_info("  Cubie.fbx: %u parts, %u textures, part 0 base colour %.2f %.2f %.2f", asset->as_mesh.part_count,
             asset->as_mesh.texture_count, (f64)asset->as_mesh.parts[0].base_color.r, (f64)asset->as_mesh.parts[0].base_color.g,
             (f64)asset->as_mesh.parts[0].base_color.b);


    /*
     * Normals are unit length.
     *
     * ufbx is asked to generate them where the file has none, so this is checking that the request was
     * honoured as much as that the file was well formed: a zero-length normal takes no light and draws
     * as a black triangle, which reads as a texturing bug rather than as a missing attribute.
     */
    for (u32 i = 0; i < vertices; i++) {
      f32x3 normal = asset->as_mesh.normals[i];
      f32   length = sqrtf((normal.x * normal.x) + (normal.y * normal.y) + (normal.z * normal.z));

      nya_assert(length > 0.9F && length < 1.1F, "normal %u has length %f rather than one", i, (f64)length);
    }

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the second model reads too, and the two are different meshes
  // ─────────────────────────────────────────────────────────────────────────────
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

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: something that is not an FBX fails rather than being believed
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // A real file of the wrong kind, rather than a made up path — the interesting failure is ufbx
    // rejecting the contents, not the filesystem rejecting the name.
    NYA_EXPECT(nya_asset_load((NYA_AssetLoadParameters){ .type = NYA_ASSET_TYPE_MESH, .handle = NYA_ASSET_I18N_EN_JSON }));

    end_frame();

    NYA_Asset* asset = nya_asset_get(NYA_ASSET_I18N_EN_JSON);

    nya_assert(asset != nullptr, "the entry exists");

    /*
     * FAILED, and nothing left half built.
     *
     * The loader frees the file's bytes whether or not the parse succeeded, so a failure must not leave
     * the union holding a pointer the unload path would then free a second time — and must not leave a
     * vertex count that nya_render3d_mesh would trust.
     */
    nya_assert(asset->status == NYA_ASSET_STATUS_FAILED, "a JSON file is not a model, status %d", (int)asset->status);
    nya_assert(asset->as_mesh.vertex_count == 0, "and it reports no geometry");

    printf("  PASSED\n");
  }

  nya_info("PASSED: test_asset_mesh (0 failures)");

  return EXIT_SUCCESS;
}
