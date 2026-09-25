/**
 * @file examples/tui_dashboard/main.c
 *
 * A live arena dashboard drawn through the engine's terminal backend and built out of `nya_ui_*`
 * widgets: occupancy bars that rise and fall while the arenas below them fill and empty, a panel of
 * controls beside them, and the whole thing driven from the keyboard, because a TUI with no keyboard
 * is not a TUI.
 *
 * ```
 * ./build run example tui_dashboard
 * ```
 *
 * Tab and shift-tab step through every widget, the arrows move within a panel and between them,
 * enter activates, escape quits, and the mouse still works where the terminal reports one.
 *
 * ## What draws it
 *
 * The cell presenter, installed on the window below: the widgets are the engine's, and what they look like
 * is `ui_present_cell.c` — `[ quit ]`, `[x] filling`, a box drawing frame, a bar of blocks. Take the
 * `nya_ui_presenter_set` out and the same program still runs, drawn by the shape presenter through the
 * terminal backend, which is rectangles rasterised into cells; that is what a TUI looked like here before.
 * Focus is reverse video and the angle delimiters, so it reads on a terminal with no colour at all.
 *
 * ## How a program picks the terminal backend
 *
 * `NYA_TERMINAL`, defined before the engine is included. It implies `NYA_HEADLESS` (see
 * base_basic.h), so this program has no GPU device, no swapchain and no window server connection,
 * and `nyangine.c` compiles `render2d_terminal.c` in place of `render2d.c`. Every
 * `nya_render2d_*` and `nya_ui_*` call below is the same call a program against a window makes; only
 * the backend under it differs. `-DNYA_TERMINAL` on the compiler's command line does exactly the
 * same thing, and is how a project that is a TUI would do it; it is defined here because the example
 * builds under the shared example flags.
 *
 * ## What a TUI has to stand up, and what it must not
 *
 * The callback, event and input systems, and no more: those three are what `nya_event_dispatch`,
 * `nya_input_*` and every `nya_ui_*` widget above them read, and they are the same three the SDL
 * backend feeds. `nya_app_init` would bring up the whole frame loop and with it SDL's video
 * subsystem, which a program over ssh has no display for.
 *
 * Two things follow from there and are easy to get wrong. The input system rolls its just-pressed
 * edges on `NYA_EVENT_UPDATING_ENDED`, so a program with no frame loop dispatches that itself once
 * per frame or every key is pressed forever. And a terminal reports a key press and never a release,
 * so each key is dispatched as a down and an up in the same drain: `just_pressed` is the call that
 * works here, and `pressed` is true for no time at all. See terminal.h.
 * */

/*
 * Before nyangine.h, and before anything else: base_basic.h resolves NYA_TERMINAL_ENABLED the first
 * time it is included, and the renderer that nyangine.c picks follows from it.
 */
#define NYA_TERMINAL 1

// nyangine.h first, always: base_basic.h defines _POSIX_C_SOURCE and _XOPEN_SOURCE before it pulls in libc, and a system header included ahead of it has already fixed them at another value.
#include "nyangine-core/nyangine.h"

#include "SDL3/SDL_timer.h"

#include "nyangine-core/nyangine.c"

/* CONSTANTS */

/** Frames drawn before the program exits on its own. Bounded, because an example must end. */
#define FRAME_COUNT 600

/** Wall clock between frames. Thirty a second is smooth to watch and leaves the terminal idle. */
#define FRAME_INTERVAL_MS 33

/** How much the worker arena holds when full, which is what its bar is a fraction of. */
#define WORKER_BUDGET_BYTES nya_kibyte_to_byte(256)

/** Bytes taken per frame while filling. The budget over a tenth of the frames, so it fills ten times. */
#define WORKER_STEP_BYTES (WORKER_BUDGET_BYTES / (FRAME_COUNT / 10))

/*
 * The layout, in pixels at scale 1, because that is what the UI takes. One cell is
 * NYA_TERMINAL_CELL_WIDTH_PX by NYA_TERMINAL_CELL_HEIGHT_PX of them, so every size here is a whole
 * number of cells: a size that is not lands between two and the cell it rounds to is nobody's
 * choice. That is the one thing a terminal asks of a layout written for pixels.
 */
#define CELL_W ((f32)NYA_TERMINAL_CELL_WIDTH_PX)
#define CELL_H ((f32)NYA_TERMINAL_CELL_HEIGHT_PX)

/** A widget is one row, gaps are one row, and a panel's edge is one row and two columns. */
#define ROW_HEIGHT CELL_H
#define GAP        CELL_H
#define PADDING    CELL_H

/**
 * The focus mark, in pixels, which here is one whole cell. The cell presenter marks focus with reverse
 * video and the angle delimiters instead and never draws a bar, but the number is still what the layout
 * adds up, so it is a whole cell like everything else here. See NYA_UIStyle and ui_present_cell.h.
 * */
#define FOCUS_BAR CELL_W

/** Bars kept from a pass until the UI's cells are on the screen. One per row a terminal can show. */
#define BARS_MAX NYA_TERMINAL_ROWS_MAX

/** The controls beside the arenas, and the widest an arena's name may be. Both in columns. */
#define CONTROLS_COLUMNS 28.0F
#define NAME_COLUMNS     18.0F

/*
 * The palette. Every colour is opaque: the terminal backend keeps the character under a translucent
 * fill and only dims it, which is what makes a scrim read as a scrim, and which would leave a panel
 * showing the text it was drawn over.
 */
#define COLOR_GROUND ((NYA_Color){ 0.06F, 0.06F, 0.09F, 1.0F })
#define COLOR_PANEL  ((NYA_Color){ 0.11F, 0.11F, 0.16F, 1.0F })
#define COLOR_BUTTON ((NYA_Color){ 0.16F, 0.16F, 0.22F, 1.0F })
#define COLOR_FOCUS  ((NYA_Color){ 0.22F, 0.26F, 0.38F, 1.0F })
#define COLOR_TRACK  ((NYA_Color){ 0.13F, 0.13F, 0.17F, 1.0F })
#define COLOR_FILL   ((NYA_Color){ 0.35F, 0.75F, 0.55F, 1.0F })
#define COLOR_HOT    ((NYA_Color){ 0.90F, 0.45F, 0.35F, 1.0F })
#define COLOR_ACCENT ((NYA_Color){ 0.45F, 0.65F, 0.95F, 1.0F })
#define COLOR_TEXT   ((NYA_Color){ 0.88F, 0.88F, 0.92F, 1.0F })
#define COLOR_DIM    ((NYA_Color){ 0.50F, 0.50F, 0.58F, 1.0F })

/** Share of a bar above which it is drawn hot rather than calm. */
#define HOT_SHARE 0.85F

/**
 * The legend picture, calm to hot, in the top right corner of terminals with the kitty protocol. Its
 * pixels are the terminal's own, not NYA_TERMINAL_CELL_WIDTH_PX's: 48 by 16 is five or six cells wide
 * and one tall in the fonts terminals ship with, which fits the empty margin row above the header.
 * */
#define LEGEND_WIDTH_PX  48
#define LEGEND_HEIGHT_PX 16

/** Columns from the right edge where the legend starts, room for its width in any common font. */
#define LEGEND_COLUMNS_FROM_RIGHT 8

/* TYPES */

/** What a bar is a share of, which is the dropdown's two options in its order. */
typedef enum {
    MEASURE_USED = 0,
    MEASURE_FREE_LIST,

    MEASURE_COUNT,
} Measure;

/**
 * One arena's occupancy bar: the room the layout gave it and how full that arena is.
 *
 * Kept rather than drawn where it is declared, because the UI is drawn into a grid of cells and put on the
 * screen in one go: anything the program draws itself has to go on after that, or the panel the bar sits in
 * covers it. See ui_present_cell.h.
 * */
typedef struct {
    NYA_Rectf track;
    f32       share;
} Bar;

/** Everything the frame needs that is not an arena. Plain data, passed down rather than global. */
typedef struct {
    u32 frame;
    u64 elapsed_ms;

    /** Which arena row is chosen, or U32_MAX when none is. */
    u32 selected;

    /** Whether the worker arena is still being filled, and what the bars are a share of. */
    b8  filling;
    u32 measure;

    /** Whether the terminal panel is folded away, and how many resizes the run has seen. */
    b8  terminal_open;
    u32 resizes;

    /** Whether the legend picture has been sent, and how many resizes had happened when it was. */
    b8  legend_sent;
    u32 legend_resizes;

    /** Set when a widget asked to stop, since a UI pass reports rather than exits. */
    b8 quit;

    /** What the last draw pass laid out room for, drawn once the UI's own cells are down. */
    Bar bars[BARS_MAX];
    u32 bar_count;
} Dashboard;

/* DRAWING */

/** One arena: a row that takes focus, and the room its bar is drawn into once the UI is on the screen. */
static void arena_row(NYA_UI* ui, Dashboard* dashboard, u32 index, const NYA_ArenaStats* stats) {
    nya_assert(ui != nullptr && dashboard != nullptr && stats != nullptr);

    NYA_ConstCString name = stats->name != nullptr ? stats->name : "(unnamed)";
    u64              part = dashboard->measure == MEASURE_USED ? stats->used_bytes : stats->free_list_bytes;

    // an empty arena has no capacity yet, and dividing by it would be the one crash this can have.
    f32 share = stats->reserved_bytes > 0 ? (f32)part / (f32)stats->reserved_bytes : 0.0F;
    share     = nya_clamp(share, 0.0F, 1.0F);

    // named by its place in the registry rather than by the arena: two arenas may share a name, and two rows sharing an id would share their widgets' ids with them.
    char id[16];
    (void)snprintf(id, sizeof(id), "arena%u", index);

    if (!nya_ui_panel_begin(ui, id, (NYA_UIPanel){ .direction = NYA_UI_DIRECTION_ROW, .frameless = true })) return;

    nya_ui_size(ui, nya_ui_fixed(NAME_COLUMNS * CELL_W));
    if (nya_ui_selectable(ui, name, dashboard->selected == index)) dashboard->selected = index;

    // the bar takes room the layout gave it, which is what nya_ui_space is for: the widgets above it are the engine's, this is the program's, and they share one column of pixels.
    nya_ui_size(ui, nya_ui_grow(1));
    NYA_Rectf track = nya_ui_space(ui, 0.0F, ROW_HEIGHT);

    if (track.width > 0.0F && dashboard->bar_count < nya_carray_length(dashboard->bars)) {
        dashboard->bars[dashboard->bar_count] = (Bar){ .track = track, .share = share };
        dashboard->bar_count                 += 1;
    }

    nya_ui_panel_end(ui);
}

/**
 * The bars, after the UI's cells have reached the screen. A program draws over the UI here rather than into
 * it: the presenter owns a grid of characters and puts it down in one go, so a rectangle drawn while the
 * pass was open would be covered by the panel the bar sits in.
 * */
static void bars_draw(NYA_Window* window, const Dashboard* dashboard) {
    nya_assert(window != nullptr && dashboard != nullptr);

    for (u32 i = 0; i < dashboard->bar_count; i++) {
        const Bar* bar = &dashboard->bars[i];
        b8         hot = bar->share >= HOT_SHARE;

        nya_render2d_rect(window, bar->track.x, bar->track.y, bar->track.width, bar->track.height, COLOR_TRACK);
        nya_render2d_rect(window, bar->track.x, bar->track.y, roundf(bar->track.width * bar->share), bar->track.height, hot ? COLOR_HOT : COLOR_FILL);

        // hot said a second time, in bold at the bar's end, because a terminal with no colour draws both bars alike.
        if (hot) nya_render2d_terminal_glyph(window, bar->track.x + bar->track.width - CELL_W, bar->track.y, '!', COLOR_TEXT, NYA_TERMINAL_ATTRIBUTE_BOLD);
    }
}

/** The controls beside the arenas: what the bars show, what the worker arena does, and the way out. */
static void controls_panel(NYA_UI* ui, Dashboard* dashboard, NYA_Arena* worker) {
    nya_assert(ui != nullptr && dashboard != nullptr && worker != nullptr);

    NYA_UIPanel panel = { .width = nya_ui_fixed(CONTROLS_COLUMNS * CELL_W), .height = nya_ui_grow(1) };
    if (!nya_ui_panel_begin(ui, "controls", panel)) return;

    NYA_ConstCString measures[MEASURE_COUNT] = {
        [MEASURE_USED]      = "used",
        [MEASURE_FREE_LIST] = "free list",
    };

    // the list hangs over the widgets under it instead of pushing them down the panel.
    (void)nya_ui_dropdown(ui, "bars show", measures, nya_carray_length(measures), &dashboard->measure);

    (void)nya_ui_toggle(ui, "filling", &dashboard->filling);

    if (nya_ui_button(ui, "free worker")) nya_arena_free_all(worker);

    if (nya_ui_section_begin(ui, "terminal", &dashboard->terminal_open)) {
        NYA_TerminalCapabilities capabilities = nya_terminal_capabilities();

        NYA_ConstCString depth = "no colour";
        switch (capabilities.color_depth) {
            case NYA_TERMINAL_COLOR_16:   depth = "16 colours"; break;
            case NYA_TERMINAL_COLOR_256:  depth = "256 colours"; break;
            case NYA_TERMINAL_COLOR_TRUE: depth = "truecolor"; break;
            case NYA_TERMINAL_COLOR_NONE: break;
            case NYA_TERMINAL_COLOR_COUNT:
            default:                      nya_unreachable();
        }

        char line[64];

        (void)snprintf(line, sizeof(line), "%ux%u cells", nya_terminal_columns(), nya_terminal_rows());
        nya_ui_label(ui, line, COLOR_DIM);
        nya_ui_label(ui, depth, COLOR_DIM);
        nya_ui_label(ui, capabilities.kitty_images ? "kitty images" : "no images", COLOR_DIM);

        (void)snprintf(line, sizeof(line), "%u resize(s)", dashboard->resizes);
        nya_ui_label(ui, line, COLOR_DIM);

        nya_ui_section_end(ui);
    }

    // a spacer, so the way out sits at the bottom of the panel however tall the terminal is.
    nya_ui_size(ui, nya_ui_grow(1));
    (void)nya_ui_space(ui, 0.0F, 0.0F);

    if (nya_ui_button(ui, "quit")) dashboard->quit = true;

    nya_ui_panel_end(ui);
}

/**
 * The whole frame, run twice: once from the input pass, which moves focus and returns what widgets
 * did, and once from the draw pass, which draws it. Nothing here knows it is a terminal except the
 * bold mark on a hot bar, which is the attribute a pixel surface does not have.
 * */
static void frame_pass(NYA_Window* window, NYA_UIPass pass, Dashboard* dashboard, NYA_Arena* worker) {
    nya_assert(window != nullptr && dashboard != nullptr && worker != nullptr);

    NYA_UI* ui = nya_ui_begin(window, pass);

    dashboard->bar_count = 0;

    NYA_UIPanel root = { .width = nya_ui_grow(1), .height = nya_ui_grow(1), .frameless = true };

    if (nya_ui_panel_begin(ui, "root", root)) {
        char header[80];
        (void)snprintf(header, sizeof(header), "nyangine arenas — frame %u/%u — " FMTu64 " ms", dashboard->frame + 1, FRAME_COUNT, dashboard->elapsed_ms);
        nya_ui_label(ui, header, COLOR_ACCENT);

        NYA_UIPanel body = { .direction = NYA_UI_DIRECTION_ROW, .height = nya_ui_grow(1), .frameless = true };

        if (nya_ui_panel_begin(ui, "body", body)) {
            // the arenas scroll when there are more of them than rows, which is the wheel and the focus following the keys, both for free.
            NYA_UIPanel arenas = { .width = nya_ui_grow(1), .height = nya_ui_grow(1) };

            if (nya_ui_panel_begin(ui, "arenas", arenas)) {
                u32 rows = nya_arena_registry_count();

                // straight off the registry, so every arena alive right now appears, the engine's included.
                for (u32 i = 0; i < rows; i++) {
                    NYA_Arena* arena = nya_arena_registry_at(i);
                    if (arena == nullptr) continue;

                    NYA_ArenaStats stats = nya_arena_stats(arena);
                    arena_row(ui, dashboard, i, &stats);
                }

                nya_ui_panel_end(ui);
            }

            controls_panel(ui, dashboard, worker);
            nya_ui_panel_end(ui);
        }

        nya_ui_label(ui, "tab / shift-tab and the arrows move · enter activates · escape quits", COLOR_DIM);
        nya_ui_panel_end(ui);
    }

    // escape reaches here as a cancel, and only once a dropdown has had its chance at it.
    if (nya_ui_cancelled(ui)) dashboard->quit = true;

    nya_ui_end(ui);
}

/** The legend's pixels, calm on the left to hot on the right: what the two bar colours mean, in one strip. */
static void legend_fill(u8* rgba) {
    nya_assert(rgba != nullptr);

    for (u32 x = 0; x < LEGEND_WIDTH_PX; x++) {
        f32       t     = (f32)x / (f32)(LEGEND_WIDTH_PX - 1);
        NYA_Color color = {
            nya_lerp(COLOR_FILL.r, COLOR_HOT.r, t),
            nya_lerp(COLOR_FILL.g, COLOR_HOT.g, t),
            nya_lerp(COLOR_FILL.b, COLOR_HOT.b, t),
            1.0F,
        };

        for (u32 y = 0; y < LEGEND_HEIGHT_PX; y++) {
            u8* pixel = &rgba[(((u64)y * LEGEND_WIDTH_PX) + x) * 4U];

            pixel[0] = (u8)(color.r * 255.0F);
            pixel[1] = (u8)(color.g * 255.0F);
            pixel[2] = (u8)(color.b * 255.0F);
            pixel[3] = (u8)(color.a * 255.0F);
        }
    }
}

/**
 * Sends the legend after the frame's cells, or it would sit under them. A placement stays until it is
 * cleared, so it goes out once and again only when a resize has repainted the screen beneath it.
 * */
static void legend_show(NYA_Window* window, Dashboard* dashboard, const u8* rgba) {
    nya_assert(window != nullptr && dashboard != nullptr && rgba != nullptr);

    if (!nya_terminal_capabilities().kitty_images) return;
    if (dashboard->legend_sent && dashboard->legend_resizes == dashboard->resizes) return;

    // marked before the send, so a screen with no room for it is not asked again every frame.
    dashboard->legend_sent    = true;
    dashboard->legend_resizes = dashboard->resizes;

    nya_terminal_image_clear();

    // a terminal narrower than the legend's room has no corner to put it in.
    u16 columns = nya_terminal_columns();
    if (columns <= LEGEND_COLUMNS_FROM_RIGHT) return;

    // false only where the protocol is missing, which was asked above; the picture is decoration either way.
    f32 x = (f32)(columns - LEGEND_COLUMNS_FROM_RIGHT) * CELL_W;
    (void)nya_render2d_terminal_image(window, x, 0.0F, rgba, LEGEND_WIDTH_PX, LEGEND_HEIGHT_PX);
}

/* INPUT */

/** Drains the terminal and feeds the input system every widget above it reads. */
static void input_pump(Dashboard* dashboard) {
    nya_assert(dashboard != nullptr);

    /* The terminal's keys and mouse reports become NYA_Events here, and this loop is the same one a program on the GPU backend writes: nothing below reads a terminal type, and nothing below knows which backend produced the event. That is the point of routing terminal input through nya_event_dispatch rather than handing a program NYA_TerminalInput. */
    nya_system_event_drain_terminal_events();

    NYA_Event event;
    while (nya_system_event_poll(&event)) {
        // the input system is what nya_input_* and every nya_ui_* widget read, and it is fed by the events, not by the terminal.
        nya_system_input_handle_event(&event);

        // the UI reads its own keys out of the input system; the only event this program wants for itself is the one that says the grid changed under it.
        if (event.type == NYA_EVENT_WINDOW_RESIZED) dashboard->resizes += 1;
    }
}

/** The look: one row per widget, one row between them, and a focus mark a whole cell wide. */
static NYA_UIStyle dashboard_style(void) {
    return (NYA_UIStyle){
        .margin      = CELL_H,
        .padding     = PADDING,
        .spacing     = GAP,
        .item_height = ROW_HEIGHT,
        .focus_bar   = FOCUS_BAR,
        .panel       = COLOR_PANEL,
        .track       = COLOR_TRACK,
        .accent      = COLOR_ACCENT,
        .text_dim    = COLOR_DIM,
        .button      = { .normal = COLOR_PANEL, .focused = COLOR_FOCUS, .pressed = COLOR_BUTTON, .disabled = COLOR_PANEL },
        .text        = { .normal = COLOR_TEXT, .focused = COLOR_TEXT, .pressed = COLOR_TEXT, .disabled = COLOR_DIM },
    };
}

/* MAIN */

s32 main(s32 argc, NYA_CString* argv) {
    nya_unused(argc, argv);
    nya_backtrace_init();

    /* Opened before anything is logged on purpose: a log line written after the switch to the alternate screen lands on the screen the dashboard is about to paint over, and one written during the loop would tear a frame. What the terminal probe found is logged by the open itself, and the first frame repaints over it. */
    NYA_Error opened = nya_render2d_terminal_open((NYA_TerminalOptions){ .alternate_screen = true, .mouse = true });

    if (!opened.ok) {
        // an operating error, not a crash: a program piped into a file is a normal thing to be.
        nya_log_error("The terminal backend could not open: %s", opened.message);
        nya_log_info("Run this from a terminal; it draws into one and reads keys from it.");

        nya_backtrace_deinit();
        return EXIT_FAILURE;
    }

    NYA_Window* window = nya_render2d_terminal_window();

    /* The three engine subsystems a TUI needs, and no more; see the file header. The callback system comes first because the input system registers a hook by name through it. */
    _NYA_APP_INSTANCE = (NYA_App){ .initialized = true, .frame_allocator = nya_arena_create(.name = "frame_allocator") };

    nya_system_callback_init();
    NYA_EXPECT(nya_system_events_init());
    nya_system_input_init();

    // what the UI reads confirm and cancel from. A TUI has no gamepad and no rebinding screen, so this is the whole of its input configuration.
    nya_input_action_rebind(NYA_INPUT_ACTION_CONFIRM, NYA_KEY_RETURN);
    nya_input_action_rebind(NYA_INPUT_ACTION_CANCEL, NYA_KEY_ESCAPE);

    nya_ui_style_set(window, dashboard_style());

    /* The presenter that makes this a TUI rather than a drawing of a GUI: buttons as `[ quit ]`, panels as box drawing, the slider as blocks. Without it the shape presenter draws rectangles, which the terminal backend rasterises into cells — the same widgets, doing the same things, in a look nobody expects from a terminal. Static because the grid is most of a megabyte and it outlives every pass. */
    static NYA_UICells cells;

    nya_ui_cells_init(&cells, (NYA_UICellOptions){ .cell = { CELL_W, CELL_H } });
    nya_ui_presenter_set(window, nya_ui_cells_presenter(&cells));

    // the arena the dashboard watches. Named, because the name is the label on its bar, and sized to the budget so the bar is a fraction of a number this file chose.
    NYA_Arena* worker = nya_arena_create(.name = "worker", .region_size = WORKER_BUDGET_BYTES);

    NYA_Arena* scratch = nya_arena_create(.name = "scratch");

    // filled once: the picture never changes, only whether the screen under it still holds it.
    static u8 legend[LEGEND_WIDTH_PX * LEGEND_HEIGHT_PX * 4];
    legend_fill(legend);

    /* Torn down at the bottom rather than with `defer`, which is what the rest of the tree uses. clang's static analyser models a `defer` as running where it is written, so every use of these three below would be reported as a use after free, and this file is one of the four translation units `./build check --strict` analyses. There is one exit path out of the loop, so the explicit teardown is not a second way to get it wrong. Delete this and go back to `defer` when clang-analyzer understands C2Y's defer. */

    u64       started_ms = nya_clock_get_monotonic_ms();
    Dashboard dashboard  = { .selected = U32_MAX, .filling = true, .terminal_open = true };

    for (u32 frame = 0; frame < FRAME_COUNT; frame++) {
        input_pump(&dashboard);

        dashboard.frame      = frame;
        dashboard.elapsed_ms = nya_clock_get_monotonic_ms() - started_ms;

        // the input pass, before the edges roll: this is where a key press becomes a focus move and a click becomes a button. It draws nothing.
        frame_pass(window, NYA_UI_PASS_INPUT, &dashboard, worker);

        // what a frame loop would dispatch. Without it every key stays just-pressed for the rest of the run and the first arrow walks the whole list.
        nya_event_dispatch((NYA_Event){ .type = NYA_EVENT_UPDATING_ENDED });

        if (dashboard.quit) break;

        // fills, is released at each tenth mark, and fills again, so the bars move rather than sit.
        if (frame > 0 && frame % (FRAME_COUNT / 10) == 0) nya_arena_free_all(worker);
        if (dashboard.filling) (void)nya_arena_alloc(worker, WORKER_STEP_BYTES);

        // a per frame scratch: taken, used, and given back whole at the end of the frame. Its bar never climbs, which is the point of showing it beside the other.
        NYA_String* line = nya_string_sprintf(scratch, "frame %u of %u", frame + 1, FRAME_COUNT);
        nya_assert(line->length > 0, "sprintf produced nothing");

        nya_render2d_terminal_frame_begin(window, COLOR_GROUND);

        // the UI into its own grid of characters, that grid onto the terminal, and then what this program draws itself. See ui_present_cell.h for why those are three steps and not one.
        nya_ui_cells_reset(&cells);
        frame_pass(window, NYA_UI_PASS_DRAW, &dashboard, worker);
        nya_ui_cells_present(&cells);
        bars_draw(window, &dashboard);

        nya_render2d_terminal_frame_end(window);
        legend_show(window, &dashboard, legend);

        nya_arena_free_all(scratch);

        // SDL's, not the engine's: platform/clock/ measures time but has no way to give it away. core_app.c's frame limiter calls SDL_DelayNS for the same reason.
        SDL_Delay(FRAME_INTERVAL_MS);
    }

    // removed by the program that placed it, rather than trusting each terminal to drop it with the alternate screen.
    nya_terminal_image_clear();

    // the window goes back to the shape presenter before the grid it was pointing at is thrown away.
    nya_ui_presenter_set(window, nullptr);
    nya_ui_cells_deinit(&cells);

    nya_arena_destroy(scratch);
    nya_arena_destroy(worker);

    nya_system_input_deinit();
    nya_system_events_deinit();
    nya_system_callback_deinit();

    nya_arena_destroy(_NYA_APP_INSTANCE.frame_allocator);
    _NYA_APP_INSTANCE = (NYA_App){ 0 };

    nya_render2d_terminal_close();

    nya_backtrace_deinit();
    return EXIT_SUCCESS;
}
