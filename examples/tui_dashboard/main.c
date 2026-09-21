/**
 * @file examples/tui_dashboard/main.c
 *
 * A live arena dashboard drawn through the engine's terminal backend: occupancy bars that rise and
 * fall while the arenas below them fill and empty, keys and mouse clicks read from the terminal, a
 * resize handled while it runs, and a picture through the kitty graphics protocol where the terminal
 * has one.
 *
 * ```
 * ./build run example tui_dashboard
 * ```
 *
 * `q` or escape quits, space empties the worker arena, and clicking a bar selects it.
 *
 * ## How a program picks the terminal backend
 *
 * `NYA_TERMINAL`, defined before the engine is included. It implies `NYA_HEADLESS` (see
 * base_basic.h), so this program has no GPU device, no swapchain and no window server connection,
 * and `nyangine.c` compiles `render2d_terminal.c` in place of `render2d.c`. Every
 * `nya_render2d_*` call below is the same call a program against a window makes; only the backend
 * under it differs. `-DNYA_TERMINAL` on the compiler's command line does exactly the same thing, and
 * is how a project that is a TUI would do it; it is defined here because the example builds under
 * the shared example flags.
 * */

/*
 * Before nyangine.h, and before anything else: base_basic.h resolves NYA_TERMINAL_ENABLED the first
 * time it is included, and the renderer that nyangine.c picks follows from it.
 */
#define NYA_TERMINAL 1

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

/** Frames drawn before the program exits on its own. Bounded, because an example must end. */
#define FRAME_COUNT 240

/** Wall clock between frames. Thirty a second is smooth to watch and leaves the terminal idle. */
#define FRAME_INTERVAL_MS 33

/** How much the worker arena holds when full, which is what its bar is a fraction of. */
#define WORKER_BUDGET_BYTES nya_kibyte_to_byte(256)

/** Bytes taken per frame while filling. The budget over a quarter of the frames, so it fills four times. */
#define WORKER_STEP_BYTES (WORKER_BUDGET_BYTES / (FRAME_COUNT / 4))

/** Inputs taken from the terminal per frame. More than a person can produce in 33 ms. */
#define INPUT_PER_FRAME_MAX 32

/*
 * The layout, in pixels, because that is what the renderer takes. One cell is
 * NYA_TERMINAL_CELL_WIDTH_PX by NYA_TERMINAL_CELL_HEIGHT_PX, so these are cell counts times the cell.
 */
#define CELL_W ((f32)NYA_TERMINAL_CELL_WIDTH_PX)
#define CELL_H ((f32)NYA_TERMINAL_CELL_HEIGHT_PX)

/** Rows the title takes, and the row the first bar starts on. */
#define HEADER_ROWS 3
#define FOOTER_ROWS 2

/** Columns before a bar starts, leaving room for the arena's name beside it. */
#define LABEL_COLUMNS 20

/** The swatch sent through the kitty protocol, in pixels. Small, because it is a demonstration. */
#define SWATCH_SIZE_PX 64

/*
 * The palette. Named because a hex triple in the middle of a draw call says nothing about what it is
 * for, and because these are the only colours this program has.
 */
#define COLOR_GROUND  ((NYA_Color){ 0.06F, 0.06F, 0.09F, 1.0F })
#define COLOR_HEADER  ((NYA_Color){ 0.16F, 0.14F, 0.30F, 1.0F })
#define COLOR_TRACK   ((NYA_Color){ 0.13F, 0.13F, 0.17F, 1.0F })
#define COLOR_FILL    ((NYA_Color){ 0.35F, 0.75F, 0.55F, 1.0F })
#define COLOR_HOT     ((NYA_Color){ 0.90F, 0.45F, 0.35F, 1.0F })
#define COLOR_TEXT    ((NYA_Color){ 0.88F, 0.88F, 0.92F, 1.0F })
#define COLOR_DIM     ((NYA_Color){ 0.50F, 0.50F, 0.58F, 1.0F })
#define COLOR_SELECT  ((NYA_Color){ 0.25F, 0.22F, 0.42F, 1.0F })

/** Share of a bar above which it is drawn hot rather than calm. */
#define HOT_SHARE 0.85F

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Everything the frame needs that is not an arena. Plain data, passed down rather than global. */
typedef struct {
    u32 frame;
    u64 elapsed_ms;

    /** Which bar the last click landed on, or the row count when none. */
    u32 selected;

    /** Whether the terminal has been resized since the program started, for the footer to say so. */
    u32 resizes;
} Dashboard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * DRAWING
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** One bar: the arena's name, a track, the filled share of it, and the numbers. */
static void bar_draw(NYA_Window* window, u32 index, NYA_ConstCString label, u64 used, u64 capacity, b8 selected) {
    nya_assert(window != nullptr && label != nullptr);

    f32 y = (f32)(HEADER_ROWS + index) * CELL_H;

    // an empty arena has no capacity yet, and dividing by it would be the one crash this can have.
    f32 share = capacity > 0 ? (f32)used / (f32)capacity : 0.0F;
    share     = nya_clamp(share, 0.0F, 1.0F);

    f32 track_x     = LABEL_COLUMNS * CELL_W;
    f32 numbers     = 24.0F * CELL_W;
    f32 track_width = nya_max((f32)window->screen_width - track_x - numbers, CELL_W);

    if (selected) nya_render2d_rect(window, 0.0F, y, (f32)window->screen_width, CELL_H, COLOR_SELECT);

    nya_render2d_text(window, label, CELL_W, y, selected ? COLOR_TEXT : COLOR_DIM);

    nya_render2d_rect(window, track_x, y, track_width, CELL_H, COLOR_TRACK);
    nya_render2d_rect(window, track_x, y, track_width * share, CELL_H, share >= HOT_SHARE ? COLOR_HOT : COLOR_FILL);

    nya_render2d_textf(window, track_x + track_width + CELL_W, y, COLOR_DIM, "%5.1f%%  " FMTu64 " B", (f64)share * 100.0, used);
}

/** The whole frame. Nothing here knows it is a terminal; these are the calls a window takes. */
static void frame_draw(NYA_Window* window, const Dashboard* dashboard) {
    nya_assert(window != nullptr && dashboard != nullptr);

    nya_render2d_terminal_frame_begin(window, COLOR_GROUND);

    f32 width = (f32)window->screen_width;

    nya_render2d_rect(window, 0.0F, 0.0F, width, CELL_H, COLOR_HEADER);
    nya_render2d_text(window, "nyangine — arenas", CELL_W, 0.0F, COLOR_TEXT);
    nya_render2d_textf(window, width - (28.0F * CELL_W), 0.0F, COLOR_TEXT, "frame %3u/%u  " FMTu64 " ms", dashboard->frame + 1, FRAME_COUNT,
                       dashboard->elapsed_ms);

    // straight off the registry, so every arena alive right now appears, the engine's included.
    u32 rows = nya_arena_registry_count();

    for (u32 i = 0; i < rows; i++) {
        NYA_Arena* arena = nya_arena_registry_at(i);
        if (arena == nullptr) continue;

        NYA_ArenaStats stats = nya_arena_stats(arena);

        bar_draw(window, i, stats.name != nullptr ? stats.name : "(unnamed)", stats.used_bytes, stats.reserved_bytes, dashboard->selected == i);
    }

    NYA_TerminalCapabilities capabilities = nya_terminal_capabilities();

    NYA_ConstCString depth = "no colour";
    switch (capabilities.color_depth) {
        case NYA_TERMINAL_COLOR_16:    depth = "16 colours"; break;
        case NYA_TERMINAL_COLOR_256:   depth = "256 colours"; break;
        case NYA_TERMINAL_COLOR_TRUE:  depth = "truecolor"; break;
        case NYA_TERMINAL_COLOR_NONE:  break;
        case NYA_TERMINAL_COLOR_COUNT:
        default:                       nya_unreachable();
    }

    f32 footer_y = (f32)(window->screen_height) - (FOOTER_ROWS * CELL_H);

    nya_render2d_textf(window, CELL_W, footer_y, COLOR_DIM, "%ux%u cells · %s · %s · %u resize(s)", nya_terminal_columns(), nya_terminal_rows(), depth,
                       capabilities.kitty_images ? "kitty images" : "no images", dashboard->resizes);

    nya_render2d_text(window, "q quit · space free the worker arena · click a bar to select it", CELL_W, footer_y + CELL_H, COLOR_DIM);
}

/**
 * A colour swatch through the kitty protocol, to the right of the footer.
 *
 * Generated rather than loaded, so the example needs no asset and the bytes are obviously RGBA. On a
 * terminal without the protocol this draws nothing and says false, which is the documented
 * degradation: the rest of the dashboard is unaffected.
 * */
static b8 swatch_draw(NYA_Window* window, f32 x, f32 y) {
    static u8 pixels[SWATCH_SIZE_PX * SWATCH_SIZE_PX * 4];

    for (u32 row = 0; row < SWATCH_SIZE_PX; row++) {
        for (u32 column = 0; column < SWATCH_SIZE_PX; column++) {
            u32 at = ((row * SWATCH_SIZE_PX) + column) * 4;

            pixels[at + 0] = (u8)((column * 255U) / (SWATCH_SIZE_PX - 1U));
            pixels[at + 1] = (u8)((row * 255U) / (SWATCH_SIZE_PX - 1U));
            pixels[at + 2] = 0x80;
            pixels[at + 3] = 0xFF;
        }
    }

    return nya_render2d_terminal_image(window, x, y, pixels, SWATCH_SIZE_PX, SWATCH_SIZE_PX);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INPUT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Drains the terminal and acts on what came out. False when the program was asked to stop. */
static b8 input_pump(Dashboard* dashboard, NYA_Arena* worker) {
    nya_assert(dashboard != nullptr && worker != nullptr);

    NYA_TerminalInput input[INPUT_PER_FRAME_MAX];
    u32               count = nya_terminal_poll(input, nya_carray_length(input));

    for (u32 i = 0; i < count; i++) {
        switch (input[i].kind) {
            case NYA_TERMINAL_INPUT_KEY: {
                if (input[i].key == NYA_TERMINAL_KEY_ESCAPE) return false;
                if (input[i].codepoint == 'q') return false;
                if (input[i].codepoint == ' ') nya_arena_free_all(worker);
            } break;

            case NYA_TERMINAL_INPUT_MOUSE_BUTTON: {
                // the header is above the bars, so a click on it selects nothing.
                if (input[i].is_down && input[i].row >= HEADER_ROWS) dashboard->selected = (u32)input[i].row - HEADER_ROWS;
            } break;

            case NYA_TERMINAL_INPUT_RESIZE: {
                dashboard->resizes += 1;
            } break;

            // the mouse moving and the wheel turning are reported and this program has no use for
            // them; a TUI that scrolls would.
            case NYA_TERMINAL_INPUT_MOUSE_MOVED:
            case NYA_TERMINAL_INPUT_MOUSE_WHEEL:  break;

            case NYA_TERMINAL_INPUT_NONE:
            case NYA_TERMINAL_INPUT_KIND_COUNT:
            default:                              nya_unreachable();
        }
    }

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * MAIN
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    /*
     * Opened before anything is logged on purpose: a log line written after the switch to the
     * alternate screen lands on the screen the dashboard is about to paint over, and one written
     * during the loop would tear a frame. What the terminal probe found is logged by the open
     * itself, and the first frame repaints over it.
     */
    NYA_Error opened = nya_render2d_terminal_open((NYA_TerminalOptions){ .alternate_screen = true, .mouse = true });

    if (!opened.ok) {
        // an operating error, not a crash: a program piped into a file is a normal thing to be.
        nya_log_error("The terminal backend could not open: %s", opened.message);
        nya_log_info("Run this from a terminal; it draws into one and reads keys from it.");

        nya_backtrace_deinit();
        return EXIT_FAILURE;
    }

    NYA_Window* window = nya_render2d_terminal_window();

    // the arena the dashboard watches. Named, because the name is the label on its bar, and sized to
    // the budget so the bar is a fraction of a number this file chose.
    NYA_Arena* worker = nya_arena_create(.name = "worker", .region_size = WORKER_BUDGET_BYTES);

    NYA_Arena* scratch = nya_arena_create(.name = "scratch");

    /*
     * Torn down at the bottom rather than with `defer`, which is what the rest of the tree uses.
     * clang's static analyser models a `defer` as running where it is written, so every use of these
     * three below would be reported as a use after free, and this file is one of the four
     * translation units `./build check --strict` analyses. There is one exit path out of the loop,
     * so the explicit teardown is not a second way to get it wrong. Delete this and go back to
     * `defer` when clang-analyzer understands C2Y's defer.
     */

    u64       started_ms = nya_clock_get_monotonic_ms();
    Dashboard dashboard  = { .selected = UINT32_MAX };

    /** The resize count the swatch on screen was drawn at, so it is re-sent exactly when it is gone. */
    u32 swatch_resizes = 0;

    for (u32 frame = 0; frame < FRAME_COUNT; frame++) {
        if (!input_pump(&dashboard, worker)) break;

        // fills, is released at each quarter mark, and fills again, so the bars move rather than sit.
        if (frame > 0 && frame % (FRAME_COUNT / 4) == 0) nya_arena_free_all(worker);
        (void)nya_arena_alloc(worker, WORKER_STEP_BYTES);

        // a per frame scratch: taken, used, and given back whole at the end of the frame. Its bar
        // never climbs, which is the point of showing it beside the other.
        NYA_String* line = nya_string_sprintf(scratch, "frame %u of %u", frame + 1, FRAME_COUNT);
        nya_assert(line->length > 0, "sprintf produced nothing");

        dashboard.frame      = frame;
        dashboard.elapsed_ms = nya_clock_get_monotonic_ms() - started_ms;

        /*
         * A kitty placement stays on screen until it is deleted, so the swatch is sent once and then
         * only when the screen it sits on has been thrown away: on the first frame, and after a
         * resize. Sending it every frame would stack two hundred placements and five megabytes of
         * base64 to do it, which is what the first version of this example did.
         */
        b8 resend_swatch = frame == 0 || dashboard.resizes != swatch_resizes;
        if (resend_swatch) nya_terminal_image_clear();

        frame_draw(window, &dashboard);
        nya_render2d_terminal_frame_end(window);

        // after the present, so the cells it paints do not land on top of the picture.
        if (resend_swatch) {
            (void)swatch_draw(window, (f32)window->screen_width - (SWATCH_SIZE_PX + (2 * NYA_TERMINAL_CELL_WIDTH_PX)),
                              (f32)window->screen_height - (f32)SWATCH_SIZE_PX);

            swatch_resizes = dashboard.resizes;
        }

        nya_arena_free_all(scratch);

        // SDL's, not the engine's: platform/clock/ measures time but has no way to give it away.
        // core_app.c's frame limiter calls SDL_DelayNS for the same reason.
        SDL_Delay(FRAME_INTERVAL_MS);
    }

    nya_arena_destroy(scratch);
    nya_arena_destroy(worker);
    nya_render2d_terminal_close();

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
