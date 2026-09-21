/**
 * @file render2d_terminal.h
 *
 * The terminal 2D backend's own lifetime, and the two things a caller can do in a terminal that it
 * cannot do on a GPU. Everything else a program draws with is `render2d.h` unchanged: this file
 * replaces `render2d.c`, so `nya_render2d_rect`, `nya_render2d_text` and every `nya_ui_*` call above
 * them land in character cells with no call site edited. That is the whole point of the exercise.
 *
 * Compiled only under `-DNYA_TERMINAL`. A GPU build carries none of it, and a terminal build carries
 * no GPU renderer, because `nyangine.c` picks one file or the other.
 *
 * ```c
 * // examples/tui_dashboard/main.c, in full.
 * NYA_EXPECT(nya_render2d_terminal_open((NYA_TerminalOptions){ .alternate_screen = true, .mouse = true }));
 * defer nya_render2d_terminal_close();
 *
 * NYA_Window* window = nya_render2d_terminal_window();
 *
 * while (running) {
 *     nya_render2d_terminal_frame_begin(window, NYA_COLOR_BLACK);
 *
 *     nya_render2d_rect(window, 0, 0, 240, 32, (NYA_Color){ 0.1F, 0.1F, 0.2F, 1.0F });
 *     nya_render2d_text(window, "arenas", 8, 8, NYA_COLOR_WHITE);
 *
 *     nya_render2d_terminal_frame_end(window);
 * }
 * ```
 *
 * ## Units
 *
 * A caller draws in pixels, exactly as it would against a window, and one cell is
 * `NYA_TERMINAL_CELL_WIDTH_PX` by `NYA_TERMINAL_CELL_HEIGHT_PX` of them. The window this backend
 * owns reports its size in those pixels, so a layout written for a window lands on cells without a
 * second set of numbers. Positions are rounded down to the cell that contains them.
 *
 * ## What a terminal cannot do, and says so
 *
 * The backend answers `render_features.h` honestly at open rather than pretending: shadows,
 * lighting, point lights, reflections, fog, sky, decals, LOD, the whole post chain and colour
 * grading are `NYA_RENDER_TOGGLE_OFF`, because there is no pass to run them in. Read them back with
 * `nya_render_features` and the debug overlay's disabled line names every one.
 *
 * What does work: filled and outlined rectangles, rounded ones as their square selves, lines,
 * polylines, triangles, circles, gradients, text, scissors, the 2D camera, and images through the
 * kitty protocol. What draws nothing: textures by asset handle (a terminal has no sampler and the
 * pixels are not the backend's to read), render textures, shader passes and procedural draws.
 * Every one of those is a call that succeeds and does nothing, the way the headless backend does it,
 * so a program written for a window still runs.
 *
 * Text is monospace by definition, so `nya_render2d_text_measure` returns cells times the cell size
 * rather than asking a font. A layout measured here lands exactly where it is drawn, which is not
 * true of measuring a proportional font and then drawing into a grid.
 *
 * ## The one font
 *
 * A terminal has exactly one face and it belongs to whoever configured the terminal, so
 * `nya_render2d_terminal_open` registers `NYA_RENDER2D_TERMINAL_FONT` and makes it the default face
 * unless a caller already set one. Without that a TUI would have to name a `.ttf` nothing opens
 * before `nya_ui_*` would measure anything at all, since an invalid face measures zero and the UI
 * lays out without drawing while it does. A caller that names another font gets the same cells.
 *
 * ## Layers do not sort here
 *
 * `nya_render2d_layer_set` is recorded and read back, and changes nothing: cells are painted where
 * they are drawn. The UI's stacking of top level panels is therefore hit tested correctly and drawn
 * in call order, so two overlapping panels in a TUI show the one declared last. Put a TUI's panels
 * beside each other rather than over each other until there is a sorted cell buffer.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_basic.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"
#include "nyangine/platform/terminal/terminal.h"
#include "nyangine/renderer/render_color.h"

#if NYA_TERMINAL_ENABLED

typedef struct NYA_Window NYA_Window;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The name and path the terminal's one face is registered under at open. Not a file: nothing loads it, and every
 * measurement of it is cells. It exists because a face has to be valid for the UI to measure with it at all.
 * */
#define NYA_RENDER2D_TERMINAL_FONT "terminal"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Opens the terminal, sizes this backend's window to it, and turns off every render feature a
 * terminal has no pass for. Fails, having changed nothing, when standard input is not a terminal.
 * */
NYA_API NYA_Error nya_render2d_terminal_open(NYA_TerminalOptions options) __attr_no_discard;

/** Closes the terminal and puts it back. Safe when open failed or was never called. */
NYA_API void nya_render2d_terminal_close(void);

/**
 * The window this backend draws into. Its `screen_width` and `screen_height` are the grid in pixels
 * and change when the terminal is resized, so a caller reads them every frame rather than caching.
 * */
NYA_API NYA_Window* nya_render2d_terminal_window(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FRAMES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Takes the terminal's current size, resizes the window to match, and fills the grid with `clear`.
 * Everything drawn after this lands in the grid and nothing reaches the screen until the frame ends.
 * */
NYA_API void nya_render2d_terminal_frame_begin(NYA_Window* window, NYA_Color clear);

/** Writes the cells that changed since the last frame, and nothing else. */
NYA_API void nya_render2d_terminal_frame_end(NYA_Window* window);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE TWO TERMINAL ONLY CALLS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * A picture, at `x`/`y` in pixels, through the kitty graphics protocol. `rgba` is `width * height`
 * pixels of eight bit RGBA.
 *
 * False, having drawn nothing, on a terminal without the protocol. That is the degradation the
 * header promises: the rest of the TUI still draws.
 *
 * It goes straight to the terminal rather than into the grid, so it is drawn *after*
 * `nya_render2d_terminal_frame_end`, or the cells that frame paints land on top of it. A placement
 * stays until `nya_terminal_image_clear` removes it, so a caller sends a picture once and again
 * when the screen under it has been thrown away, not every frame.
 * */
NYA_API b8 nya_render2d_terminal_image(NYA_Window* window, f32 x, f32 y, const u8* rgba, u32 width, u32 height) __attr_no_discard;

/**
 * Writes one character into the cell at `x`/`y`, with the attributes a terminal has and a pixel
 * surface does not: bold, dim, underline, reverse. What a TUI uses where a GPU program would reach
 * for a second font weight.
 * */
NYA_API void nya_render2d_terminal_glyph(NYA_Window* window, f32 x, f32 y, u32 codepoint, NYA_Color color, u8 attributes);

#endif // NYA_TERMINAL_ENABLED
