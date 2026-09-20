/**
 * @file examples/tui_dashboard/main.c
 *
 * A text dashboard that redraws itself: arena occupancy and fragmentation as bars, one frame at a
 * time, while the arenas below it fill and empty.
 *
 * ```
 * ./build run example tui_dashboard
 * ```
 *
 * ## There is no terminal backend yet
 *
 * A terminal backend is wanted and does not exist. `src/nyangine/renderer` targets SDL's GPU API
 * and nothing else; nothing in the tree opens a terminal, reads a keypress without a window, or
 * knows what a cell is. So this is a plain stdout program: it writes frames one after another with
 * ANSI escapes for the cursor and for colour, and it reads no input.
 *
 * What is missing, per TODO.md: a terminal backend beside the GPU one, ncurses or equivalent, with
 * the kitty image protocol so a TUI can still show pictures. That means raw mode, a cell grid with
 * its own damage tracking, key and resize events arriving as NYA_Event, and a back end per platform
 * (termios plus SIGWINCH, the Windows console API). The headless renderer is the precedent for a
 * second backend.
 *
 * Until it lands, do not take the escape codes below for an API. They are three literals in this
 * file, and a real backend would make every one of them disappear behind `nya_ui_*`.
 * */
// nyangine.h first, always: base_basic.h defines _POSIX_C_SOURCE and _XOPEN_SOURCE before it pulls
// in libc, and a system header included ahead of it has already fixed them at another value.
#include "nyangine/nyangine.h"

#include "SDL3/SDL_timer.h"

#include "nyangine/nyangine.c"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Frames drawn before the program exits. Bounded, because an example must end on its own. */
#define FRAME_COUNT 24

/** Wall clock between frames. Twelve a second is enough to read and cheap to watch. */
#define FRAME_INTERVAL_MS 80

/** Cells in one bar. Eighty column terminals are still the floor, and this leaves room for labels. */
#define BAR_CELLS 40

/** How much the worker arena holds when full, which is what the bar is a fraction of. */
#define WORKER_BUDGET_BYTES nya_kibyte_to_byte(256)

/** Bytes taken per frame while filling. The budget divided by half the frames, so it fills twice. */
#define WORKER_STEP_BYTES (WORKER_BUDGET_BYTES / (FRAME_COUNT / 2))

/*
 * The three escape sequences this file uses. Written out because there is no terminal module to ask
 * for them, and named because a bare "\x1b[2J" in the middle of a printf is unreadable.
 */
#define ANSI_CLEAR       "\x1b[2J\x1b[H"
#define ANSI_DIM         "\x1b[2m"
#define ANSI_RESET       "\x1b[0m"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DRAWING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One bar: `label`, a filled block run, and the numbers behind it. */
NYA_INTERNAL void bar_draw(NYA_ConstCString label, u64 used, u64 capacity) {
    nya_assert(label != nullptr);

    // An empty arena has no capacity yet, and dividing by it would be the one crash this program
    // could have.
    f32 fraction = capacity > 0 ? (f32)used / (f32)capacity : 0.0F;
    fraction     = nya_clamp(fraction, 0.0F, 1.0F);

    u32 filled = (u32)(fraction * (f32)BAR_CELLS);

    (void)printf("  %-18s [", label);
    for (u32 i = 0; i < BAR_CELLS; i++) (void)printf("%s", i < filled ? "#" : " ");
    (void)printf("] %6.1f%%  " ANSI_DIM "%llu / %llu bytes" ANSI_RESET "\n", (f64)fraction * 100.0, (unsigned long long)used,
                 (unsigned long long)capacity);
}

/** The whole frame, from the top of the screen down. */
NYA_INTERNAL void frame_draw(u32 frame, u64 elapsed_ms) {
    (void)printf(ANSI_CLEAR);
    (void)printf("nyangine — arenas\n\n");

    (void)printf("  frame %2u/%u   %llu ms\n\n", frame + 1, FRAME_COUNT, (unsigned long long)elapsed_ms);

    // Straight off the registry, so every arena alive right now appears, including the two the
    // engine brings up for the main thread.
    for (u32 i = 0; i < nya_arena_registry_count(); i++) {
        NYA_Arena* arena = nya_arena_registry_at(i);
        if (arena == nullptr) continue;

        NYA_ArenaStats stats = nya_arena_stats(arena);

        bar_draw(stats.name != nullptr ? stats.name : "(unnamed)", stats.used_bytes, stats.reserved_bytes);
    }

    (void)printf("\n" ANSI_DIM "  no terminal backend yet; this is stdout with escape codes" ANSI_RESET "\n");

    // stdout is a pipe when this is not a terminal, and a pipe buffers. Without the flush every
    // frame arrives at once at exit, which is not an animation.
    (void)fflush(stdout);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    // The arena the dashboard watches. Named, because the name is the label on its bar, and sized
    // to the budget so the bar is a fraction of a number this file chose rather than of the
    // allocator's default region.
    NYA_Arena* worker = nya_arena_create(.name = "worker", .region_size = WORKER_BUDGET_BYTES);
    defer      nya_arena_destroy(worker);

    NYA_Arena* scratch = nya_arena_create(.name = "scratch");
    defer      nya_arena_destroy(scratch);

    u64 started_ms = nya_clock_get_monotonic_ms();

    for (u32 frame = 0; frame < FRAME_COUNT; frame++) {
        // Fills for the first half and is released at the halfway mark, so the bar rises, drops and
        // rises again rather than sitting still.
        if (frame == FRAME_COUNT / 2) nya_arena_free_all(worker);

        (void)nya_arena_alloc(worker, WORKER_STEP_BYTES);

        // A per frame scratch: taken, used, and given back whole at the end of the frame. The
        // bar for it never climbs, which is the point of showing it beside the other.
        NYA_String* line = nya_string_sprintf(scratch, "frame %u of %u", frame + 1, FRAME_COUNT);
        nya_assert(line->length > 0, "sprintf produced nothing");

        frame_draw(frame, nya_clock_get_monotonic_ms() - started_ms);

        nya_arena_free_all(scratch);

        // SDL's, not the engine's: platform/clock/ measures time but has no way to give it away.
        // core_app.c's frame limiter calls SDL_DelayNS for the same reason.
        SDL_Delay(FRAME_INTERVAL_MS);
    }

    nya_log_info("Drew %u frames in %llu ms.", FRAME_COUNT, (unsigned long long)(nya_clock_get_monotonic_ms() - started_ms));

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
