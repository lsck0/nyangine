/**
 * Every vendored dependency the project links, exercised rather than merely linked.
 *
 * Linking proves a symbol resolved. It does not prove the library was built for the right platform,
 * configured with the feature you need, or usable at all — libluajit-linux.a spent a while full of
 * COFF objects that the linker silently skipped, and nothing noticed because nothing called into
 * Lua. This file calls into each one and checks the answer.
 *
 * Deliberately shallow. One round trip, one version string, one object created and destroyed: the
 * question is "is this library alive and correct for this platform", not "does it work", which is
 * its own maintainers' job. Anything deeper would be testing box2d rather than testing our build.
 *
 * **Nothing here touches the network.** The curl check builds a handle and reads its version; it
 * never resolves a host. A test that needed the internet would fail in CI for reasons that have
 * nothing to do with the code.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

#include <box2d/box2d.h>
#include <box3d/box3d.h>
#include <curl/curl.h>
#include <lz4.h>
#include <lz4frame.h>
#include <sqlite3.h>

// Generated from a template at build time; declares sqlite3_vec_init. See vendor_sqlvec.h.
#include "sqlite-vec.h"

#include "SDL3/SDL_init.h"
#include "SDL3_image/SDL_image.h"
#include "SDL3_mixer/SDL_mixer.h"
#include "SDL3_net/SDL_net.h"
#include "SDL3_ttf/SDL_ttf.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

/** sqlean's bundle entry point, the one symbol libsqlean.a exports. See sqlean_extensions.c. */
extern int nya_sqlean_init(sqlite3* db, char** error_message, const sqlite3_api_routines* api);

s32 main(void) {
  NYA_Arena* arena = nya_arena_create(.name = "test_vendors");
  defer      nya_arena_destroy(arena);

  // ─────────────────────────────────────────────────────────────────────────────
  // VENDOR: SDL3, and the three satellite libraries built against it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Nothing that needs a device: no video, no audio. This is a link and version check, and a CI
    // container has neither.
    b8 ok = SDL_Init(0);
    nya_assert(ok, "SDL_Init(0) failed: %s", SDL_GetError());
    defer SDL_Quit();

    int version = SDL_GetVersion();
    nya_assert(version > 0, "SDL reported version %d", version);
    nya_info("SDL %d.%d.%d", SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version));

    // Each satellite is compiled against SDL's headers, so a version mismatch between them is the
    // failure this catches — they would link and then disagree about struct layouts.
    int image_version = IMG_Version();
    int ttf_version   = TTF_Version();
    int mixer_version = MIX_Version();
    nya_assert(image_version > 0, "SDL_image reported %d", image_version);
    nya_assert(ttf_version > 0, "SDL_ttf reported %d", ttf_version);
    nya_assert(mixer_version > 0, "SDL_mixer reported %d", mixer_version);

    nya_assert(SDL_VERSIONNUM_MAJOR(image_version) == SDL_VERSIONNUM_MAJOR(version), "SDL_image is built against a different SDL major");
    nya_assert(SDL_VERSIONNUM_MAJOR(ttf_version) == SDL_VERSIONNUM_MAJOR(version), "SDL_ttf is built against a different SDL major");
    nya_assert(SDL_VERSIONNUM_MAJOR(mixer_version) == SDL_VERSIONNUM_MAJOR(version), "SDL_mixer is built against a different SDL major");

    // SDL_ttf brings FreeType and HarfBuzz with it, and initialising is what actually loads them.
    nya_assert(TTF_Init(), "TTF_Init() failed, so FreeType or HarfBuzz did not come through: %s", SDL_GetError());
    TTF_Quit();
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // VENDOR: SDL_net
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Brings the library up and tears it down without opening a socket. Enough to prove it is not
    // the stub build — vendor/sdl-net carries an SDL_net_stub_only.c, and linking that instead
    // would resolve every symbol and do nothing.
    nya_assert(NET_Init(), "NET_Init() failed: %s", SDL_GetError());
    defer NET_Quit();

    int version = NET_Version();
    nya_assert(version > 0, "SDL_net reported %d", version);
    nya_info("SDL_net %d.%d.%d", SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version));
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // VENDOR: box2d — a world that actually simulates
  // ─────────────────────────────────────────────────────────────────────────────
  {
    b2WorldDef world_def = b2DefaultWorldDef();
    world_def.gravity    = (b2Vec2){ 0.0F, -10.0F };

    b2WorldId world = b2CreateWorld(&world_def);
    nya_assert(b2World_IsValid(world), "b2CreateWorld returned an invalid world");
    defer b2DestroyWorld(world);

    b2BodyDef body_def = b2DefaultBodyDef();
    body_def.type      = b2_dynamicBody;
    body_def.position  = (b2Vec2){ 0.0F, 100.0F };

    b2BodyId body = b2CreateBody(world, &body_def);
    nya_assert(b2Body_IsValid(body), "b2CreateBody returned an invalid body");

    // A shape, because a dynamic body without one has zero mass and box2d does not integrate it.
    // Gravity alone is not enough to make something fall.
    b2Polygon  box       = b2MakeBox(0.5F, 0.5F);
    b2ShapeDef shape_def = b2DefaultShapeDef();
    shape_def.density    = 1.0F;
    (void)b2CreatePolygonShape(body, &shape_def, &box);

    nya_assert(b2Body_GetMass(body) > 0.0F, "the shape gave the body no mass");

    // Stepping is the part that proves the library works rather than merely loads: a body under
    // gravity has to have fallen.
    for (u32 i = 0; i < 60; i++) b2World_Step(world, 1.0F / 60.0F, 4);

    b2Vec2 position = b2Body_GetPosition(body);
    nya_assert(position.y < 100.0F, "the body did not fall, y is %f", (f64)position.y);
    nya_info("box2d: body fell to y=%.2f after one second", (f64)position.y);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // VENDOR: box3d
  // ─────────────────────────────────────────────────────────────────────────────
  {
    b3WorldDef world_def = b3DefaultWorldDef();
    world_def.gravity    = (b3Vec3){ 0.0F, -10.0F, 0.0F };

    b3WorldId world = b3CreateWorld(&world_def);
    nya_assert(b3World_IsValid(world), "b3CreateWorld returned an invalid world");
    defer b3DestroyWorld(world);

    b3BodyDef body_def = b3DefaultBodyDef();
    body_def.type      = b3_dynamicBody;
    body_def.position  = (b3Vec3){ 0.0F, 100.0F, 0.0F };

    b3BodyId body = b3CreateBody(world, &body_def);
    nya_assert(b3Body_IsValid(body), "b3CreateBody returned an invalid body");

    // Same reason as box2d above: no shape, no mass, no fall.
    b3Sphere   sphere    = { .center = { 0.0F, 0.0F, 0.0F }, .radius = 0.5F };
    b3ShapeDef shape_def = b3DefaultShapeDef();
    shape_def.density    = 1.0F;
    (void)b3CreateSphereShape(body, &shape_def, &sphere);

    nya_assert(b3Body_GetMass(body) > 0.0F, "the shape gave the body no mass");

    for (u32 i = 0; i < 60; i++) b3World_Step(world, 1.0F / 60.0F, 4);

    b3Vec3 position = b3Body_GetPosition(body);
    nya_assert(position.y < 100.0F, "the body did not fall, y is %f", (f64)position.y);
    nya_info("box3d: body fell to y=%.2f after one second", (f64)position.y);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // VENDOR: lz4 — a compress/decompress round trip
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_info("lz4 %s", LZ4_versionString());
    nya_assert(LZ4_versionNumber() > 0);

    // Repetitive on purpose, so the result is meaningfully smaller and a compressor that silently
    // did nothing would be visible.
    char source[1024];
    for (u64 i = 0; i < sizeof(source); i++) source[i] = (char)('a' + (i % 8));

    int   bound      = LZ4_compressBound((int)sizeof(source));
    char* compressed = nya_arena_alloc(arena, (u64)bound);

    int compressed_size = LZ4_compress_default(source, compressed, (int)sizeof(source), bound);
    nya_assert(compressed_size > 0, "LZ4_compress_default returned %d", compressed_size);
    nya_assert(compressed_size < (int)sizeof(source), "compressing 1 KiB of repeats produced %d bytes", compressed_size);

    char* restored = nya_arena_alloc(arena, sizeof(source));
    int   restored_size = LZ4_decompress_safe(compressed, restored, compressed_size, (int)sizeof(source));

    nya_assert(restored_size == (int)sizeof(source), "decompressed to %d bytes", restored_size);
    nya_assert(memcmp(source, restored, sizeof(source)) == 0, "the round trip did not preserve the bytes");

    nya_info("lz4: 1024 bytes -> %d -> 1024", compressed_size);

    // The frame API is a separate translation unit inside the same archive, so it is worth touching
    // independently: a partial build would resolve one and not the other.
    LZ4F_preferences_t preferences = { 0 };
    u64                frame_bound = LZ4F_compressFrameBound(sizeof(source), &preferences);
    nya_assert(frame_bound > 0, "LZ4F_compressFrameBound returned zero");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // VENDOR: LuaJIT — a state that runs a script
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // This is the one that was silently broken: libluajit-linux.a held COFF objects, lld skipped
    // every member with a warning, and the link still succeeded because nothing called Lua. Calling
    // it is the only thing that would have caught it.
    lua_State* state = luaL_newstate();
    nya_assert(state != nullptr, "luaL_newstate() failed, is the archive built for this platform?");
    defer lua_close(state);

    luaL_openlibs(state);

    // LUA_RELEASE rather than LUAJIT_VERSION: the latter lives in luajit.h, which LuaJIT's Makefile
    // generates and does not track, so a checkout whose archive came from a cache does not have it.
    // A test that names the library should not be the thing that breaks on that.
    nya_info("%s (LuaJIT)", LUA_RELEASE);

    int loaded = luaL_dostring(state, "return 6 * 7");
    nya_assert(loaded == 0, "luaL_dostring failed: %s", lua_tostring(state, -1));

    nya_assert(lua_isnumber(state, -1), "the script did not return a number");
    nya_assert((s64)lua_tointeger(state, -1) == 42, "got " FMTs64, (s64)lua_tointeger(state, -1));
    lua_pop(state, 1);

    // A string round trip too, since that exercises Lua's allocator rather than just its stack.
    int concat = luaL_dostring(state, "return 'nya' .. 'ngine'");
    nya_assert(concat == 0, "luaL_dostring failed: %s", lua_tostring(state, -1));
    nya_assert(nya_string_equals((NYA_CString)lua_tostring(state, -1), "nyangine"));
    lua_pop(state, 1);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // VENDOR: SQLite
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_info("SQLite %s", sqlite3_libversion());
    nya_assert(sqlite3_libversion_number() >= 3000000, "SQLite reported %d", sqlite3_libversion_number());

    // Threadsafe() reports how the library was compiled, which is the sort of configure time
    // decision that a linked-but-wrong build gets wrong.
    nya_info("SQLite threadsafe: %d", sqlite3_threadsafe());

    // The plugin covers real usage; this only proves the library underneath it is the one we built.
    sqlite3* handle = nullptr;
    nya_assert(sqlite3_open(":memory:", &handle) == SQLITE_OK, "sqlite3_open failed: %s", sqlite3_errmsg(handle));
    nya_assert(sqlite3_close(handle) == SQLITE_OK);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // VENDOR: sqlean and sqlvec — the archives exist and their entry points link
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // Both are static archives whose whole surface is one init function; see vendor_sqlean.h and
    // vendor_sqlvec.h. This file links against the archives directly rather than going through
    // nya_sql_open, so it fails if the archive is missing or listed after libsqlite3 — which is a
    // build problem, and belongs here rather than in the plugin's own suite. What those functions do
    // once registered is tested in test_sql_extensions.c.
    sqlite3* handle = nullptr;
    nya_assert(sqlite3_open(":memory:", &handle) == SQLITE_OK);
    defer     (void)sqlite3_close_v2(handle);

    nya_assert(nya_sqlean_init(handle, nullptr, nullptr) == SQLITE_OK, "sqlean's entry point is linked but refused to register");
    nya_assert(sqlite3_vec_init(handle, nullptr, nullptr) == SQLITE_OK, "sqlite-vec's entry point is linked but refused to register");

    nya_info("sqlean: linked, sqlite-vec %s: linked", SQLITE_VEC_VERSION);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // VENDOR: libcurl — no network, just the library
  // ─────────────────────────────────────────────────────────────────────────────
  {
    curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    nya_assert(info != nullptr, "curl_version_info returned null");
    nya_info("libcurl %s, %s", info->version, info->ssl_version != nullptr ? info->ssl_version : "no TLS");

    // TLS is the configure time decision most likely to be silently wrong, and an https request
    // without it fails at runtime with a confusing protocol error rather than at build time.
    nya_assert((info->features & CURL_VERSION_SSL) != 0, "libcurl was built without TLS support");
    nya_assert(info->ssl_version != nullptr && info->ssl_version[0] != '\0', "libcurl reports TLS but names no backend");

    // https has to be in the protocol list for the same reason.
    b8 has_https = false;
    for (const char* const* protocol = info->protocols; *protocol != nullptr; protocol++) {
      if (nya_string_equals((NYA_CString)*protocol, "https")) has_https = true;
    }
    nya_assert(has_https, "libcurl does not list https among its protocols");

    CURL* handle = curl_easy_init();
    nya_assert(handle != nullptr, "curl_easy_init() failed");
    curl_easy_cleanup(handle);
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // VENDOR: libbacktrace — already wired into base_backtrace
  // ─────────────────────────────────────────────────────────────────────────────
  {
    // base_backtrace keys off __has_include("backtrace.h") and degrades to a null backend when the
    // vendor is absent, which means a missing libbacktrace does not fail the build — it silently
    // produces stack traces with no frames. Capturing one is what tells the two apart.
    // Initialised first. libbacktrace builds its debug info state lazily on the first call, and
    // without this the capture succeeds and reports zero frames — which is exactly what the null
    // backend does, so the two are indistinguishable unless the real one has been brought up.
    nya_backtrace_init();

    NYA_Backtrace trace = { 0 };
    nya_backtrace_capture(&trace, 0);

    nya_assert(trace.count > 0, "captured a stack trace with no frames, so libbacktrace is the null backend");
    nya_info("libbacktrace: captured " FMTu32 " frames", trace.count);
  }

  printf("PASSED: test_vendors\n");
  return 0;
}
