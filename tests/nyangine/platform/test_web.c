/**
 * The platform/web primitives through their native fallbacks: the clock and CSPRNG answer for real from
 * os/, the store round-trips in its run-lifetime table, and the two async I/O seams refuse off wasm the
 * way the header says they do. The browser backends run under node from src/web/wasm_demo.c's
 * nyangine_web_probe; this is what the native tree can prove.
 */
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
    NYA_Arena* arena = nya_arena_create(.name = "test_web");

    // ── clock: real times off os/, monotonic never running backward ──
    u64 wall_ms = nya_web_clock_wall_ms();
    nya_check(wall_ms > 1'600'000'000'000ULL, "the wall clock reads a plausible epoch millisecond, not zero");

    u64 first  = nya_web_clock_monotonic_ms();
    u64 second = nya_web_clock_monotonic_ms();
    nya_check(second >= first, "the monotonic clock does not run backward across two reads");

    // ── random: fills the buffer, refuses the mistakes os_random refuses ──
    u8 key[32] = { 0 };
    nya_check(nya_web_random_bytes(key, sizeof(key)), "the CSPRNG fills a 32 byte key");
    b8 any_nonzero = false;
    for (u32 i = 0; i < sizeof(key); i++) any_nonzero = any_nonzero || key[i] != 0;
    nya_check(any_nonzero, "the filled key is not all zero");

    u8 again[32] = { 0 };
    nya_check(nya_web_random_bytes(again, sizeof(again)), "a second fill succeeds");
    nya_check(nya_memcmp(key, again, sizeof(key)) != 0, "two fills differ");

    nya_check(!nya_web_random_bytes(nullptr, 8), "a null buffer is refused");
    nya_check(!nya_web_random_bytes(key, 0), "a zero size is refused");

    // ── storage: a value round-trips, truncation reports the full length, delete removes it ──
    const u8 value[6] = { 1, 2, 3, 4, 5, 6 };
    nya_check(nya_web_storage_set("token", value, sizeof(value)), "a value stores");

    u8  read[16] = { 0 };
    s64 length   = nya_web_storage_get("token", read, sizeof(read));
    nya_check(length == (s64)sizeof(value), "get returns the stored length");
    nya_check(nya_memcmp(read, value, sizeof(value)) == 0, "the bytes come back unchanged");

    // A buffer too small: only capacity bytes written, but the full length reported so the caller can grow.
    u8  small[3] = { 0 };
    s64 needed   = nya_web_storage_get("token", small, sizeof(small));
    nya_check(needed == (s64)sizeof(value), "a short read still reports the full length");
    nya_check(nya_memcmp(small, value, sizeof(small)) == 0, "the first capacity bytes are the value's");

    nya_check(nya_web_storage_get("absent", read, sizeof(read)) == -1, "an absent key reads -1");

    nya_check(nya_web_storage_delete("token"), "delete removes the key");
    nya_check(!nya_web_storage_delete("token"), "deleting again reports it was already gone");
    nya_check(nya_web_storage_get("token", read, sizeof(read)) == -1, "the deleted key is now absent");

    // An empty value is stored and read back as length 0, distinct from absent's -1.
    nya_check(nya_web_storage_set("empty", nullptr, 0), "an empty value stores");
    nya_check(nya_web_storage_get("empty", read, sizeof(read)) == 0, "the empty value reads back as length 0");

    // ── fetch: the browser-only seam refuses off wasm, and every accessor tolerates a null handle ──
    NYA_WebFetch* fetch = nya_web_fetch_create(arena, "GET", "https://example.com", nullptr, 0, nullptr);
    nya_check(fetch == nullptr, "fetch has no backend off wasm and refuses");
    nya_check(nya_web_fetch_poll(fetch) == NYA_WEB_FETCH_FAILED, "polling a null fetch reads FAILED");
    nya_check(nya_web_fetch_status_code(fetch) == 0, "a null fetch has no status code");
    u8 body[8] = { 0 };
    nya_check(nya_web_fetch_body(fetch, body, sizeof(body)) == -1, "a null fetch has no body");
    nya_web_fetch_destroy(fetch); // a no-op on null, must not crash

    // ── socket: the same, browser-only and null-tolerant ──
    NYA_WebSocketLink* socket = nya_web_socket_open(arena, "wss://example.com/socket");
    nya_check(socket == nullptr, "the client WebSocket has no backend off wasm and refuses");
    nya_check(nya_web_socket_phase(socket) == NYA_WEB_SOCKET_CLOSED, "a null link reads CLOSED");
    const u8 frame[2] = { 0xAB, 0xCD };
    nya_check(!nya_web_socket_send(socket, frame, sizeof(frame)), "sending on a null link is refused");
    nya_check(nya_web_socket_receive(socket, body, sizeof(body)) == -1, "receiving on a null link reads -1");
    nya_web_socket_close(socket); // a no-op on null, must not crash

    // ── input: the canvas queue has no DOM off wasm, so attach refuses and poll never yields ──
    nya_check(!nya_web_input_attach("#canvas"), "canvas input has no DOM off wasm and refuses to attach");
    nya_check(!nya_web_input_attach(nullptr), "a null selector is refused");
    NYA_WebInputEvent event = { 0 };
    nya_check(!nya_web_input_poll(&event), "poll yields nothing off wasm");
    nya_check(!nya_web_input_poll(nullptr), "a null event pointer is refused");
    nya_web_input_detach(); // a no-op when nothing is attached, must not crash

    if (nya_check_failures() == 0) printf("  PASSED\n");
    return nya_check_failures() == 0 ? 0 : 1;
}
