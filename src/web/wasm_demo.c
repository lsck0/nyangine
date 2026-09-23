/**
 * @file wasm_demo.c
 *
 * The headless slice of the engine compiled to WebAssembly, exported to JavaScript. The seed of the
 * CSR path: proof that engine-shaped C runs in a browser and hands a string back across the JS
 * boundary, built and run by `./build wasm`.
 *
 * WHY THIS FILE IS SELF-CONTAINED, AND NOT `#include "nyangine/serde/serde.c"`
 *
 * The intended demo was to build a real NYA_Object and call nya_serialize(...). That does not compile
 * under emcc today, and the block is in the foundation every engine translation unit rests on rather
 * than in one leaf file, so narrowing the included set cannot reach past it:
 *
 *   - base/base_types.h declares `typedef _Float16 f16;`. clang for wasm32-unknown-emscripten answers
 *     "_Float16 is not supported on this target", and no flag turns it on. `__fp16` exists but is a
 *     storage-only type a function may not return, and the engine returns f16 by value (math_random.h),
 *     so it is not a drop-in either. Only float / a soft-float struct would do, and that changes the
 *     size and the ABI of a type the whole engine, its SIMD and its binary serde are built on.
 *   - base/base_basic.h includes <immintrin.h> unconditionally — x86 intrinsics with no wasm form.
 *   - math/ uses clang's matrix_type extension (needs -fenable-matrix, a flag we could pass) on f16
 *     matrices (which we cannot compile, per the first point).
 *   - os/os.c has no branch for OS_WASM — the platform macro exists (base_basic.h already defines
 *     OS_WASM and ARCH_WASM32), but no page/thread/time backend answers it, so the arena has no memory
 *     source. base/base.c then also pulls threads, sockets, libbacktrace and the vendored lz4, none of
 *     which a wasm build has.
 *
 * Making the engine compile here is a real platform port that touches base_types.h, base_basic.h and a
 * new os wasm backend — engine work, owned elsewhere, and out of scope for a build-system change. So
 * this file stands in for the real path with a bump allocator and a JSON writer in the same shape the
 * engine's own arena and serde produce: no malloc, a caller-visible buffer bound, and bounded writes.
 * When the port lands, the block under NYA_WASM_WITH_ENGINE below is what replaces the stand-in.
 * */

#include <emscripten/emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/*
 * The real path, kept compiling-ready and switched off. When the engine gains a wasm f16, drops
 * <immintrin.h> on wasm, and grows an os wasm backend, define NYA_WASM_WITH_ENGINE and this replaces
 * everything above: real arena, real NYA_Object, real serde. Left here so the port has a target to
 * light up rather than a paragraph to reconstruct.
 */
#ifdef NYA_WASM_WITH_ENGINE
#define NYA_NO_SDL
#include "nyangine/os/os.c"
#include "nyangine/base/base.c"
#include "nyangine/serde/serde.c"

EMSCRIPTEN_KEEPALIVE
const char* nyangine_demo_engine(void) {
    /* One arena for the object and the string it serializes to; a fresh one per call, freed on return. */
    NYA_Arena* arena = nya_arena_create(.name = "wasm_demo");

    NYA_Object* object = nya_object_create(arena);
    nya_object_add(object, "engine", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "nyangine" });
    nya_object_add(object, "target", (NYA_Value){ .type = NYA_TYPE_STRING, .as_string = "wasm32" });
    nya_object_add(object, "answer", (NYA_Value){ .type = NYA_TYPE_S64, .as_s64 = 42 });

    NYA_String* json = nya_serialize(arena, object, NYA_SERDE_FORMAT_JSON, NYA_SERDE_NONE);

    /* The string lives in `arena`; a real export copies it into linear memory the JS side owns before
     * destroying the arena. Sketched, not wired, because this whole block does not compile yet. */
    return nya_string_to_cstring(arena, json);
}
#endif
