/**
 * The Steam module against a fake client.
 *
 * NYA_SteamBackend and nya_steam_backend_set were written so this test could exist — the header says so
 * in as many words: "it exists so a test can run the lobby bookkeeping, the callback decoder and the
 * Steam network transport against a fake client". The test was never written, so the seam had no user
 * and neither did the 49 functions above it. Nothing in the tree called nya_steam_* at all.
 *
 * What is under test is the module's own bookkeeping: that a call reaches the backend with the
 * arguments it was given, that a refusal comes back as an error rather than a silent success, and that
 * a build with no client at all answers rather than crashing. Steam itself is not under test and
 * cannot be; there is no client in CI.
 **/

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────
 * THE FAKE
 * ─────────────────────────────────────────────────────────
 */

#define FAKE_USER_ID   0x1100001234ABCDEFULL
#define FAKE_USER_NAME "a test account"

/** Everything the fake was asked to do, so a call can be proved to have really reached it. */
static struct {
  u32 connects;
  u32 disconnects;

  u8  achievement_name[64];
  b8  achievement_unlocked;
  u32 achievement_sets;

  u8  progress_name[64];
  u32 progress_current;
  u32 progress_max;

  u8  stat_name[64];
  s32 stat_int;
  f32 stat_float;
  u32 stat_stores;

  u8  cloud_name[64];
  u8  cloud_data[64];
  u32 cloud_size;
  b8  cloud_present;

  /** The lobby search and the one lobby this account sits in. */
  u8  search_key[64];
  u8  search_value[64];
  u32 search_limit;
  u64 search_results[4];
  u32 search_result_count;

  u64 lobby_members[3];
  u32 lobby_member_count;
  u32 lobby_member_limit;
  u8  member_data_key[64];
  u8  member_data_value[64];
  u64 invited;

  /** Set to make the next write refuse, which is how a failure path is reached at all. */
  b8 refuse;
} fake;

#define FAKE_LOBBY_ID  0x0186000000000042ULL
#define FAKE_FRIEND_ID 0x1100001234000007ULL

static NYA_SteamInitResult fake_connect(OUT char* out_message, u64 capacity) {
  fake.connects++;
  (void)snprintf(out_message, capacity, "%s", "fake client");

  return NYA_SYSTEM_STEAM_INIT_OK;
}

static void fake_disconnect(void) { fake.disconnects++; }
static void fake_run_callbacks(void) {}

static u64              fake_user_id(void) { return FAKE_USER_ID; }
static NYA_ConstCString fake_user_name(void) { return FAKE_USER_NAME; }

static b8 fake_achievement_set(NYA_ConstCString name, b8 unlocked) {
  if (fake.refuse) return false;

  (void)snprintf((char*)fake.achievement_name, sizeof(fake.achievement_name), "%s", name);
  fake.achievement_unlocked = unlocked;
  fake.achievement_sets++;

  return true;
}

static b8 fake_achievement_get(NYA_ConstCString name, OUT b8* out_unlocked) {
  *out_unlocked = fake.achievement_sets > 0 && strcmp((const char*)fake.achievement_name, name) == 0 && fake.achievement_unlocked;

  return true;
}

static b8 fake_achievement_progress(NYA_ConstCString name, u32 current, u32 max) {
  if (fake.refuse) return false;

  (void)snprintf((char*)fake.progress_name, sizeof(fake.progress_name), "%s", name);
  fake.progress_current = current;
  fake.progress_max     = max;

  return true;
}

static b8 fake_stat_set_int(NYA_ConstCString name, s32 value) {
  if (fake.refuse) return false;

  (void)snprintf((char*)fake.stat_name, sizeof(fake.stat_name), "%s", name);
  fake.stat_int = value;

  return true;
}

static b8 fake_stat_get_int(NYA_ConstCString name, OUT s32* out_value) {
  nya_unused(name);
  *out_value = fake.stat_int;

  return true;
}

static b8 fake_stat_set_float(NYA_ConstCString name, f32 value) {
  if (fake.refuse) return false;

  (void)snprintf((char*)fake.stat_name, sizeof(fake.stat_name), "%s", name);
  fake.stat_float = value;

  return true;
}

static b8 fake_stat_get_float(NYA_ConstCString name, OUT f32* out_value) {
  nya_unused(name);
  *out_value = fake.stat_float;

  return true;
}

static b8 fake_stats_store(void) {
  fake.stat_stores++;

  return !fake.refuse;
}

static b8 fake_lobby_list_request(NYA_ConstCString key, NYA_ConstCString value, u32 max_results) {
  (void)snprintf((char*)fake.search_key, sizeof(fake.search_key), "%s", key != nullptr ? key : "");
  (void)snprintf((char*)fake.search_value, sizeof(fake.search_value), "%s", value != nullptr ? value : "");
  fake.search_limit = max_results;

  return !fake.refuse;
}

static u64 fake_lobby_list_at(u32 index) { return index < fake.search_result_count ? fake.search_results[index] : 0; }

static u32 fake_lobby_member_count(u64 lobby) { return lobby == FAKE_LOBBY_ID ? fake.lobby_member_count : 0; }
static u64 fake_lobby_member_at(u64 lobby, u32 index) { return lobby == FAKE_LOBBY_ID && index < fake.lobby_member_count ? fake.lobby_members[index] : 0; }
static u32 fake_lobby_member_limit(u64 lobby) { return lobby == FAKE_LOBBY_ID ? fake.lobby_member_limit : 0; }

static NYA_ConstCString fake_lobby_member_data_get(u64 lobby, u64 user, NYA_ConstCString key) {
  if (lobby != FAKE_LOBBY_ID || user != FAKE_USER_ID || strcmp((const char*)fake.member_data_key, key) != 0) return "";
  return (NYA_ConstCString)fake.member_data_value;
}

static void fake_lobby_member_data_set(u64 lobby, NYA_ConstCString key, NYA_ConstCString value) {
  if (lobby != FAKE_LOBBY_ID) return;

  (void)snprintf((char*)fake.member_data_key, sizeof(fake.member_data_key), "%s", key);
  (void)snprintf((char*)fake.member_data_value, sizeof(fake.member_data_value), "%s", value);
}

static b8 fake_lobby_invite(u64 lobby, u64 user) {
  if (lobby != FAKE_LOBBY_ID) return false;

  fake.invited = user;
  return !fake.refuse;
}

static b8 fake_cloud_enabled(void) { return true; }

static b8 fake_cloud_quota(OUT u64* out_total, OUT u64* out_available) {
  *out_total     = 1024;
  *out_available = 1024 - fake.cloud_size;

  return true;
}

static b8 fake_cloud_exists(NYA_ConstCString name) { return fake.cloud_present && strcmp((const char*)fake.cloud_name, name) == 0; }

static u64 fake_cloud_size(NYA_ConstCString name) { return fake_cloud_exists(name) ? fake.cloud_size : 0; }

static b8 fake_cloud_write(NYA_ConstCString name, const u8* data, u32 size) {
  if (fake.refuse || size > sizeof(fake.cloud_data)) return false;

  (void)snprintf((char*)fake.cloud_name, sizeof(fake.cloud_name), "%s", name);
  nya_memcpy(fake.cloud_data, data, size);
  fake.cloud_size    = size;
  fake.cloud_present = true;

  return true;
}

static s32 fake_cloud_read(NYA_ConstCString name, OUT u8* out_data, u32 capacity) {
  if (!fake_cloud_exists(name)) return -1;
  if (capacity < fake.cloud_size) return -1;

  nya_memcpy(out_data, fake.cloud_data, fake.cloud_size);

  return (s32)fake.cloud_size;
}

static b8 fake_cloud_delete(NYA_ConstCString name) {
  if (!fake_cloud_exists(name)) return false;

  fake.cloud_present = false;
  fake.cloud_size    = 0;

  return true;
}

/** Only what these tests need. Every entry may be null; the module checks before calling. */
static const NYA_SteamBackend FAKE_BACKEND = {
  .name = "fake",

  .connect       = fake_connect,
  .disconnect    = fake_disconnect,
  .run_callbacks = fake_run_callbacks,

  .user_id   = fake_user_id,
  .user_name = fake_user_name,

  .achievement_get      = fake_achievement_get,
  .achievement_set      = fake_achievement_set,
  .achievement_progress = fake_achievement_progress,

  .stat_get_int   = fake_stat_get_int,
  .stat_set_int   = fake_stat_set_int,
  .stat_get_float = fake_stat_get_float,
  .stat_set_float = fake_stat_set_float,
  .stats_store    = fake_stats_store,

  .lobby_list_request    = fake_lobby_list_request,
  .lobby_list_at         = fake_lobby_list_at,
  .lobby_member_count    = fake_lobby_member_count,
  .lobby_member_at       = fake_lobby_member_at,
  .lobby_member_limit    = fake_lobby_member_limit,
  .lobby_member_data_get = fake_lobby_member_data_get,
  .lobby_member_data_set = fake_lobby_member_data_set,
  .lobby_invite          = fake_lobby_invite,

  .cloud_enabled = fake_cloud_enabled,
  .cloud_quota   = fake_cloud_quota,
  .cloud_exists  = fake_cloud_exists,
  .cloud_size    = fake_cloud_size,
  .cloud_write   = fake_cloud_write,
  .cloud_read    = fake_cloud_read,
  .cloud_delete  = fake_cloud_delete,
};

s32 main(void) {
  _NYA_APP_INSTANCE = (NYA_App){ .initialized = true };

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: with no backend at all, every call answers rather than crashing
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The case every build without NYA_PLUGIN_STEAM is in, and the one a game on a machine with no
     * Steam is in. A refusal is the right answer; a crash is not, and neither is a silent success.
     */
    nya_steam_backend_set(nullptr);

    nya_check(!nya_steam_achievement_get("nothing"), "an achievement with no client reads false");
    nya_check(!nya_steam_achievement_set("nothing").ok, "and cannot be set");
    nya_check(!nya_steam_cloud_enabled(), "the cloud with no client is not enabled");
    nya_check(nya_steam_stat_get_int("nothing") == 0, "a stat with no client reads zero");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the fake is connected to, and who is signed in comes back
  // ─────────────────────────────────────────────────────────────────────────────
  {
    fake = (typeof(fake)){ 0 };

    nya_steam_backend_set(&FAKE_BACKEND);

    const NYA_SteamInitResult started = nya_system_steam_init();
    nya_check(started == NYA_SYSTEM_STEAM_INIT_OK, "the fake client connects, got %d", (int)started);
    nya_check(fake.connects == 1, "exactly once, got " FMTu32, fake.connects);

    nya_check(nya_steam_user_id().value == FAKE_USER_ID, "the signed in account comes back");
    nya_check(strcmp(nya_steam_user_name(), FAKE_USER_NAME) == 0, "and so does its name, got '%s'", nya_steam_user_name());

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the social facade answers with the Steam account when Steam is the provider
  // ─────────────────────────────────────────────────────────────────────────────
  {
    nya_check(nya_social_user_name()[0] == '\0', "before the facade runs it knows nobody");

    // the facade pumps its providers from a frame hook, so the event system has to be there to take it.
    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    defer nya_system_callback_deinit();
    defer nya_system_events_deinit();

    NYA_EXPECT(nya_social_init());
    nya_check(strcmp(nya_social_user_name(), FAKE_USER_NAME) == 0, "and then names the signed in account, got '%s'", nya_social_user_name());
    nya_social_deinit();

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: achievements reach the client with the name they were given
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EXPECT(nya_steam_achievement_set("first_light"));

    nya_check(strcmp((const char*)fake.achievement_name, "first_light") == 0, "the name reaches the client, got '%s'", (const char*)fake.achievement_name);
    nya_check(fake.achievement_unlocked, "unlocked rather than cleared");
    nya_check(nya_steam_achievement_get("first_light"), "and reads back as unlocked");

    NYA_EXPECT(nya_steam_achievement_clear("first_light"));
    nya_check(!fake.achievement_unlocked, "clearing sets it the other way");

    NYA_EXPECT(nya_steam_achievement_progress("long_haul", 30, 100));
    nya_check(strcmp((const char*)fake.progress_name, "long_haul") == 0, "progress names its achievement");
    nya_check(fake.progress_current == 30 && fake.progress_max == 100, "and carries both numbers, got " FMTu32 " of " FMTu32,
              fake.progress_current, fake.progress_max);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: stats round trip in both widths
  // ─────────────────────────────────────────────────────────────────────────────
  {
    NYA_EXPECT(nya_steam_stat_set_int("crates_broken", 42));
    nya_check(nya_steam_stat_get_int("crates_broken") == 42, "an integer stat round trips, got %d", nya_steam_stat_get_int("crates_broken"));

    NYA_EXPECT(nya_steam_stat_set_float("distance_km", 12.5F));
    nya_check(nya_steam_stat_get_float("distance_km") == 12.5F, "and a float one, got %f", (f64)nya_steam_stat_get_float("distance_km"));

    const u32 stores = fake.stat_stores;
    NYA_EXPECT(nya_steam_stats_store());
    nya_check(fake.stat_stores == stores + 1, "storing reaches the client once, got " FMTu32 " against " FMTu32, fake.stat_stores, stores + 1);

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: the cloud writes, reads back what was written, and forgets on delete
  // ─────────────────────────────────────────────────────────────────────────────
  {
    const u8 payload[] = { 'n', 'y', 'a', 0x00, 0x7F, 0xFF };

    nya_check(nya_steam_cloud_enabled(), "the fake cloud is enabled");
    nya_check(!nya_steam_cloud_exists("slot0.sav"), "and holds nothing yet");

    NYA_EXPECT(nya_steam_cloud_write("slot0.sav", payload, sizeof(payload)));

    nya_check(nya_steam_cloud_exists("slot0.sav"), "the file is there after a write");
    nya_check(nya_steam_cloud_size("slot0.sav") == sizeof(payload), "with the size it was written at, got " FMTu64,
              nya_steam_cloud_size("slot0.sav"));

    u8  read[64] = { 0 };
    u64 size     = 0;
    NYA_EXPECT(nya_steam_cloud_read("slot0.sav", read, sizeof(read), &size));

    nya_check(size == sizeof(payload), "and reads back the same length, got " FMTu64, size);
    nya_check(nya_memcmp(read, payload, sizeof(payload)) == 0, "and the same bytes, nulls and high bits included");

    u64 total = 0, available = 0;
    nya_check(nya_steam_cloud_quota(&total, &available), "the quota answers");
    nya_check(total > available, "and the written file counts against it, " FMTu64 " of " FMTu64, available, total);

    NYA_EXPECT(nya_steam_cloud_delete("slot0.sav"));
    nya_check(!nya_steam_cloud_exists("slot0.sav"), "and it is gone after a delete");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a lobby search reaches the client and its answer is read from the decoder
  // ─────────────────────────────────────────────────────────────────────────────
  {
    /*
     * The list is filled by the callback Steam sends when the search finishes, so the test sends it the
     * way the client would, through nya_steam_on_callback. A zero id in the middle is a row Steam could
     * not resolve and has to be skipped rather than shown as a lobby nobody can join.
     */
    NYA_EXPECT(nya_steam_lobby_list_request("mode", "coop", 3));
    nya_check(strcmp((const char*)fake.search_key, "mode") == 0 && strcmp((const char*)fake.search_value, "coop") == 0, "the filter reaches the client");
    nya_check(fake.search_limit == 3, "with its limit, got " FMTu32, fake.search_limit);

    nya_check(!nya_steam_lobby_list_request("mode", "", 3).ok, "a key with no value is refused before the client sees it");

    fake.search_results[0]    = 0x0186000000000001ULL;
    fake.search_results[1]    = 0;
    fake.search_results[2]    = 0x0186000000000003ULL;
    fake.search_result_count  = 3;
    _NYA_SteamLobbyMatchList list = { .lobbies_matching = 3 };
    nya_steam_on_callback(_NYA_STEAM_CALLBACK_LOBBY_MATCH_LIST, &list, sizeof(list));

    nya_check(nya_steam_lobby_list_count() == 2, "the unresolved row is skipped, got " FMTu32, nya_steam_lobby_list_count());
    nya_check(nya_steam_lobby_list_at(1).value == 0x0186000000000003ULL, "and the rows after it close up");
    nya_check(!nya_steam_id_is_set(nya_steam_lobby_list_at(2)), "an index past the end is no lobby rather than a crash");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: inside a lobby, members, the limit, member data and invites reach it
  // ─────────────────────────────────────────────────────────────────────────────
  {
    const NYA_SteamId lobby  = { .value = FAKE_LOBBY_ID };
    const NYA_SteamId self   = { .value = FAKE_USER_ID };
    const NYA_SteamId friend = { .value = FAKE_FRIEND_ID };

    // outside a lobby, the calls that act on the current one refuse rather than guessing which.
    nya_check(nya_steam_lobby_member_data_set("ready", "1").kind == NYA_ERROR_NOT_FOUND, "member data outside a lobby is refused");
    nya_check(nya_steam_lobby_invite(friend).kind == NYA_ERROR_NOT_FOUND, "and so is an invite");

    _NYA_SteamLobbyEnter entered = { .lobby = FAKE_LOBBY_ID, .response = 1 };
    nya_steam_on_callback(_NYA_STEAM_CALLBACK_LOBBY_ENTER, &entered, sizeof(entered));

    fake.lobby_members[0]   = FAKE_USER_ID;
    fake.lobby_members[1]   = FAKE_FRIEND_ID;
    fake.lobby_member_count = 2;
    fake.lobby_member_limit = 4;

    nya_check(nya_steam_lobby_member_at(lobby, 1).value == FAKE_FRIEND_ID, "the second member is the friend");
    nya_check(!nya_steam_id_is_set(nya_steam_lobby_member_at(lobby, 2)), "and there is no third");
    nya_check(nya_steam_lobby_member_limit(lobby) == 4, "the limit comes from the client, got " FMTu32, nya_steam_lobby_member_limit(lobby));
    nya_check(nya_steam_lobby_member_limit(NYA_STEAM_ID_NONE) == 0, "and no lobby has no limit");

    NYA_EXPECT(nya_steam_lobby_member_data_set("ready", "1"));
    nya_check(strcmp(nya_steam_lobby_member_data_get(lobby, self, "ready"), "1") == 0, "member data written is read back");
    nya_check(nya_steam_lobby_member_data_get(lobby, friend, "ready")[0] == '\0', "for this account only, not the friend");

    NYA_EXPECT(nya_steam_lobby_invite(friend));
    nya_check(fake.invited == FAKE_FRIEND_ID, "the invite reaches the client for the right friend");
    nya_check(!nya_steam_lobby_invite(NYA_STEAM_ID_NONE).ok, "and nobody cannot be invited");

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // TEST: a client that refuses produces an error, not a quiet success
  // ─────────────────────────────────────────────────────────────────────────────
  {
    fake.refuse = true;

    nya_check(!nya_steam_achievement_set("never").ok, "a refused achievement is an error");
    nya_check(!nya_steam_achievement_progress("never", 1, 2).ok, "and so is refused progress");
    nya_check(!nya_steam_stat_set_int("never", 1).ok, "and a refused stat");
    nya_check(!nya_steam_cloud_write("never.sav", (const u8*)"x", 1).ok, "and a refused cloud write");
    nya_check(!nya_steam_stats_store().ok, "and a refused store");
    nya_check(!nya_steam_lobby_list_request(nullptr, nullptr, 0).ok, "and a refused lobby search");

    // Reading a file that is not there is a failure too, rather than an empty success.
    u8  read[8] = { 0 };
    u64 size    = 0;
    nya_check(!nya_steam_cloud_read("never.sav", read, sizeof(read), &size).ok, "and a read of a file that is not there");

    fake.refuse = false;

    printf("  PASSED\n");
  }

  // ─────────────────────────────────────────────────────────────────────────────
  // CLEANUP
  // ─────────────────────────────────────────────────────────────────────────────
  nya_system_steam_deinit();
  nya_check(fake.disconnects == 1, "the client is disconnected exactly once, got " FMTu32, fake.disconnects);

  nya_steam_backend_set(nullptr);

  nya_log_info("PASSED: test_steam");

  return nya_check_failures() == 0 ? 0 : 1;
}
