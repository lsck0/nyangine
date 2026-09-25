/**
 * @file wasm_demo.c
 *
 * The headless slice of the engine compiled to WebAssembly, exported to JavaScript. The seed of the
 * CSR path: proof that real engine C — its arena, NYA_Object and JSON serde — runs in a browser and
 * hands a string back across the JS boundary, built and run by `./build wasm`.
 *
 * TWO PATHS, SELECTED BY NYA_WASM_WITH_ENGINE
 *
 * `./build wasm` defines NYA_WASM_WITH_ENGINE (see FLAGS_WASM), which takes the real path below: it
 * includes the leaf engine translation units the arena → object → serialize chain needs and the export
 * returns exactly what nya_serialize(...) produced. The #ifndef path above it is the original
 * stand-in — a bump allocator and a hand-written JSON writer in the same shape — kept as a
 * dependency-free fallback for building this file without the engine (plain `emcc wasm_demo.c`).
 *
 * The port that made the real path compile was small and lives beside the engine, each change guarded
 * so the native build is untouched:
 *
 *   - base/base_types.h: `_Float16` is rejected for wasm32-unknown-emscripten, so f16 is a plain float
 *     there (a soft-float widening to 4 bytes). It is invisible to this demo, which serialises to JSON
 *     *text* where a value's in-memory width never reaches the wire; it is gated so native keeps
 *     _Float16 exactly. The binary .nya format, which does depend on the width, is not built for wasm.
 *   - base/base_basic.h: <immintrin.h> (x86-only) is gated off on wasm.
 *   - math/ declares matrix types with clang's matrix_type extension; FLAGS_WASM passes -fenable-matrix
 *     (and -fdefer-ts, for `defer`), the two language flags emcc's clang shares with the native CFLAGS.
 *   - os/os_wasm.c is a new wasm-only page/time/random backend, since os.c's branches are Linux and
 *     Windows and pull threads, sockets, libbacktrace and lz4. Only the leaves are included, never
 *     base.c/os.c wholesale, for that reason.
 * */

#include <emscripten/emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef NYA_WASM_WITH_ENGINE
/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * A BUMP ARENA, THE SAME BARGAIN base/base_arena.c MAKES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * One static page, handed out front to back and never freed one allocation at a time. The engine's
 * arena takes its pages from os/os_page_*.c; a wasm build has no such backend yet, so the page is a
 * file-scope array here — the linear memory the module ships with rather than memory it maps. 64 KiB is
 * one wasm page and far more than this demo's document needs.
 */
#define WASM_DEMO_ARENA_BYTES (64u * 1024u)

static uint8_t g_arena_bytes[WASM_DEMO_ARENA_BYTES];
static size_t  g_arena_used = 0;

/** Resets the arena to empty, so a second call to the export does not run the buffer down. */
static void arena_reset(void) { g_arena_used = 0; }

/**
 * Bumps `size` bytes off the arena, or nullptr when the page is spent. Nullptr rather than a panic
 * because the one caller checks it and there is no logging in a wasm module worth crashing for.
 */
static void* arena_alloc(size_t size) {
    if (g_arena_used + size > sizeof(g_arena_bytes)) return nullptr;

    void* pointer = &g_arena_bytes[g_arena_used];
    g_arena_used += size;
    return pointer;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * A MINIMAL JSON WRITER, THE SHAPE serde_json.c EMITS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** A cursor over an arena-owned buffer, so every write is bounded by the buffer it was opened on. */
typedef struct {
    char*  items;
    size_t capacity;
    size_t length;
} JsonBuffer;

/** Appends one key/string-value pair, comma-separated, exactly as the engine's compact JSON does. */
static void json_add_string(JsonBuffer* buffer, const char* key, const char* value, bool first) {
    /* snprintf, never sprintf/strcat: the write can never run past what the arena handed out. */
    int written = snprintf(buffer->items + buffer->length, buffer->capacity - buffer->length, "%s\"%s\":\"%s\"", first ? "" : ",", key, value);

    if (written > 0 && (size_t)written < buffer->capacity - buffer->length) buffer->length += (size_t)written;
}

/** The same, for a number, so the value is emitted unquoted the way a JSON reader expects. */
static void json_add_number(JsonBuffer* buffer, const char* key, long long value, bool first) {
    int written = snprintf(buffer->items + buffer->length, buffer->capacity - buffer->length, "%s\"%s\":%lld", first ? "" : ",", key, value);

    if (written > 0 && (size_t)written < buffer->capacity - buffer->length) buffer->length += (size_t)written;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE EXPORT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Builds a small document and returns it as a JSON string, the way the real path will return
 * nya_serialize(...)'s NYA_String data. EMSCRIPTEN_KEEPALIVE keeps the symbol through dead-code
 * elimination; `./build wasm` also names it in -sEXPORTED_FUNCTIONS. JS reads the returned pointer with
 * cwrap('nyangine_demo', 'string', []).
 *
 * The returned pointer is into the static arena and stays valid until the next call, which resets it —
 * the same contract an arena string has: valid until its arena is reset or destroyed.
 */
EMSCRIPTEN_KEEPALIVE
const char* nyangine_demo(void) {
    arena_reset();

    JsonBuffer buffer = {
        .items    = arena_alloc(WASM_DEMO_ARENA_BYTES / 2),
        .capacity = WASM_DEMO_ARENA_BYTES / 2,
        .length   = 0,
    };
    if (buffer.items == nullptr) return "{}";

    buffer.items[buffer.length++] = '{';

    json_add_string(&buffer, "engine", "nyangine", true);
    json_add_string(&buffer, "target", "wasm32", false);
    json_add_string(&buffer, "renderer", "csr", false);
    json_add_number(&buffer, "arena_bytes", (long long)sizeof(g_arena_bytes), false);
    json_add_number(&buffer, "answer", 42, false);

    /* No bounds worry: the object is a handful of short pairs against a 32 KiB buffer. */
    buffer.items[buffer.length++] = '}';
    buffer.items[buffer.length]   = '\0';

    return buffer.items;
}

#else // NYA_WASM_WITH_ENGINE
/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE REAL PATH: THE ENGINE'S OWN ARENA, NYA_Object AND SERDE, COMPILED TO WASM
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 *
 * This is what the stand-in above stood in for, now live. `./build wasm` defines NYA_WASM_WITH_ENGINE
 * and compiles the leaf translation units named below, so the export builds a real NYA_Object and hands
 * back what nya_serialize(...) produced — the same code the native engine runs.
 *
 * The set is hand-picked, not base.c/os.c: those unity headers pull threads, sockets, libbacktrace and
 * lz4, none of which a wasm module has. Only the leaves the arena → object → JSON path reaches are
 * included, plus os_wasm.c, the wasm-only page/time/random backend (native f16 and <immintrin.h> are
 * gated off for this target in base_types.h and base_basic.h). The order matches base.c's: a unity
 * build has one definition of each symbol, so a file is included exactly once and after what it needs.
 */
#define NYA_NO_SDL
#include "nyangine-core/nyangine.h"

#include "nyangine-std/os/os_wasm.c"

// The web platform primitives, beside os_wasm.c: the clock, CSPRNG, storage and the fetch/WebSocket
// seams the client path stands on. Each is behind OS_WASM and takes its browser backend here, the same
// way os_wasm.c is the wasm page/time/random. nyangine_web_probe below drives every one of them.
#include "nyangine-std/platform/web/web.c"

#include "nyangine-std/base/base_arena.c"
// NYA_BACKTRACE_SUPPORTED is 0 here (it needs OS_LINUX or OS_WINDOWS), so this compiles to the same
// no-op capture/format the build tool itself links against — no libbacktrace, which wasm has none of.
#include "nyangine-std/base/base_backtrace.c"
#include "nyangine-std/base/base_ceiling.c"
#include "nyangine-std/base/base_error.c"
#include "nyangine-std/base/base_hash.c"
#include "nyangine-std/base/base_logging.c"
// base_logging's fatal path calls _nya_supervisor_on_fatal; the definition lives here (a no-op off Linux).
#include "nyangine-std/base/base_supervisor.c"
#include "nyangine-std/base/base_object.c"
#include "nyangine-std/base/base_reflection.c"
#include "nyangine-std/base/base_string.c"
#include "nyangine-std/base/base_types.c"

// serde.c's leaves, minus serde_nya_binary.c: its wire format hardcodes x87 80-bit long double, which
// wasm's IEEE-quad f128 is not, so the binary format is not built for this target. serde_dispatch.c's
// two binary arms are gated off under OS_WASM to match.
#include "nyangine-std/serde/serde_dispatch.c"
#include "nyangine-std/serde/serde_json.c"
#include "nyangine-std/serde/serde_jsonc.c"
#include "nyangine-std/serde/serde_nya.c"
#include "nyangine-std/serde/serde_reflect.c"

/**
 * The real export. Builds a small NYA_Object on the engine's arena and returns the string
 * nya_serialize(...) writes for it, so what the page renders is genuine engine output, not a
 * hand-written echo. Same symbol name and same `string` cwrap contract as the stand-in it replaces.
 *
 * The returned pointer is a cstring on `arena`, valid until the next call: this one arena is static, so
 * the buffer outlives the return the way the stand-in's static page did. A later, multi-call export
 * would copy into JS-owned linear memory and destroy the arena; the demo has one document and one
 * reader, so it keeps the arena and resets nothing.
 */
EMSCRIPTEN_KEEPALIVE
const char* nyangine_demo(void) {
    // Static, not per-call: the returned cstring lives on this arena and must outlive the return, and
    // the demo serializes one document, so there is nothing to reclaim between calls.
    static NYA_Arena* arena = nullptr;
    if (arena == nullptr) arena = nya_arena_create(.name = "wasm_demo");

    NYA_Object* object = nya_object_create(arena);
    nya_object_add(object, "engine", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "nyangine" });
    nya_object_add(object, "target", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "wasm32" });
    nya_object_add(object, "renderer", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "csr" });
    nya_object_add(object, "answer", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 42 });

    NYA_String* json = nya_serialize(arena, object, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);

    return nya_string_to_cstring(arena, json);
}

/**
 * Exercises every platform/web primitive and returns what each one did as a JSON string, the same
 * `string` cwrap contract as nyangine_demo. This is the seam's proof-of-life: the clock returns two
 * browser times, the CSPRNG fills a buffer, the store round-trips a value, and the two async seams —
 * fetch and a client WebSocket — are kicked off and polled.
 *
 * It is stateful across calls on purpose. The fetch and the socket are async, so a single call only ever
 * catches them mid-flight; the handles are static, so calling this again after the JS event loop has
 * turned shows them settle — the fetch reaching DONE with a status and a body, the socket reaching its
 * next phase. The node harness in `./build wasm` calls it twice with a tick between for exactly that.
 */
EMSCRIPTEN_KEEPALIVE
const char* nyangine_web_probe(void) {
    static NYA_Arena*        arena  = nullptr;
    static NYA_WebFetch*     fetch  = nullptr;
    static NYA_WebSocketLink* socket = nullptr;
    if (arena == nullptr) arena = nya_arena_create(.name = "wasm_web_probe");

    // The synchronous three, done fresh each call. The clock is two reads; the CSPRNG fills a key and we
    // report only whether it came back non-zero, never the bytes; the store round-trips one value.
    u64 monotonic_ms = nya_web_clock_monotonic_ms();
    u64 wall_ms      = nya_web_clock_wall_ms();

    u8 entropy[32] = { 0 };
    b8 random_ok   = nya_web_random_bytes(entropy, sizeof(entropy));
    if (random_ok) {
        b8 nonzero = false;
        for (u32 i = 0; i < sizeof(entropy); i++) nonzero = nonzero || entropy[i] != 0;
        random_ok = nonzero;
    }

    const u8 stored[5] = { 'n', 'y', 'a', '0', '1' };
    u8       read[8]   = { 0 };
    b8       storage_ok =
        nya_web_storage_set("probe", stored, sizeof(stored)) && nya_web_storage_get("probe", read, sizeof(read)) == (s64)sizeof(stored) &&
        read[0] == 'n' && read[4] == '1' && nya_web_storage_delete("probe") && nya_web_storage_get("probe", read, sizeof(read)) == -1;

    // The async two: create once, poll every call. A data: URL lets the fetch actually complete under
    // node with no server; the socket dials a dead port so it moves connecting → closed, which still
    // proves the event wiring. Both handles outlive the call on the static arena.
    if (fetch == nullptr) fetch = nya_web_fetch_create(arena, "GET", "data:text/plain,hello", nullptr, 0, nullptr);
    if (socket == nullptr) socket = nya_web_socket_open(arena, "ws://127.0.0.1:9/");

    NYA_WebFetchStatus fetch_status = nya_web_fetch_poll(fetch);
    s64                fetch_body   = -1;
    if (fetch_status == NYA_WEB_FETCH_DONE) {
        u8 body[64] = { 0 };
        fetch_body  = nya_web_fetch_body(fetch, body, sizeof(body));
    }

    NYA_Object* object = nya_object_create(arena);
    nya_object_add(object, "clock_monotonic_ms", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = monotonic_ms });
    nya_object_add(object, "clock_wall_ms", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = wall_ms });
    nya_object_add(object, "random_ok", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = random_ok });
    nya_object_add(object, "storage_ok", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = storage_ok });
    nya_object_add(object, "fetch_created", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = fetch != nullptr });
    nya_object_add(object, "fetch_status", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = (s64)fetch_status });
    nya_object_add(object, "fetch_code", (NYA_Value){ .type = NYA_TYPE_U64, .as_u64 = nya_web_fetch_status_code(fetch) });
    nya_object_add(object, "fetch_body_length", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = fetch_body });
    nya_object_add(object, "socket_created", (NYA_Value){ .type = NYA_TYPE_B8, .as_b8 = socket != nullptr });
    nya_object_add(object, "socket_phase", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = (s64)nya_web_socket_phase(socket) });

    NYA_String* json = nya_serialize(arena, object, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);
    return nya_string_to_cstring(arena, json);
}

#endif // NYA_WASM_WITH_ENGINE
