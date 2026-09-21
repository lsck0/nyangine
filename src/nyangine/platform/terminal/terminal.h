/**
 * @file terminal.h
 *
 * The terminal as a device: a grid of character cells to draw into, a byte stream to read keys and
 * mouse reports out of, and a size that changes under you. Everything a TUI needs from the OS and
 * nothing about what is drawn — `render2d_terminal.c` is the renderer that draws through this, and
 * `examples/tui_dashboard/main.c` is a program that drives both.
 *
 * ```c
 * NYA_EXPECT(nya_terminal_open((NYA_TerminalOptions){ .mouse = true, .alternate_screen = true }));
 * defer nya_terminal_close();
 *
 * while (running) {
 *     nya_terminal_clear(nya_terminal_ink(0.05F, 0.05F, 0.08F));
 *
 *     nya_terminal_cell_set(2, 1, (NYA_TerminalCell){ .codepoint = 'h', .foreground = 0xFFFFFF });
 *     nya_terminal_present();
 *
 *     NYA_TerminalInput input[NYA_TERMINAL_INPUT_MAX];
 *     u32               count = nya_terminal_poll(input, nya_carray_length(input));
 *     for (u32 i = 0; i < count; i++) if (input[i].key == NYA_TERMINAL_KEY_ESCAPE) running = false;
 * }
 * ```
 *
 * ## The functions
 *
 * | Function                             | What it does                                            |
 * | :----------------------------------- | :------------------------------------------------------ |
 * | `nya_terminal_open`/`_close`          | Raw mode, the alternate screen, mouse reporting, SIGWINCH |
 * | `nya_terminal_is_open`                | Whether the pair above is currently open                 |
 * | `nya_terminal_capabilities`           | What the probe at open found, including what is degraded |
 * | `nya_terminal_columns`/`_rows`        | The grid right now, which a resize changes               |
 * | `nya_terminal_clear`                  | Fills the back buffer with one background                |
 * | `nya_terminal_cell_set`/`_get`        | One cell of the back buffer                              |
 * | `nya_terminal_present`                | Writes the cells that changed, and nothing else          |
 * | `nya_terminal_poll`                   | Decoded keys, mouse reports and resizes since last call  |
 * | `nya_terminal_input_decode`           | The same decoder as a pure function, over your bytes     |
 * | `nya_terminal_image_draw`/`_clear`    | A picture, through the kitty graphics protocol           |
 * | `nya_terminal_key_name`               | A key's name, for a log line or a help row               |
 *
 * ## Colour, and what the probe degrades
 *
 * Cells hold full 24-bit colour whatever the terminal can show, so the grid is one source of truth
 * and only `nya_terminal_present` degrades. `nya_terminal_open` probes once, logs one line naming
 * what was lost, and never looks at the environment again:
 *
 * | Found                                     | Depth                        | What is lost          |
 * | :---------------------------------------- | :--------------------------- | :--------------------- |
 * | `COLORTERM` is `truecolor` or `24bit`     | `NYA_TERMINAL_COLOR_TRUE`     | nothing                |
 * | `TERM` contains `256color`                | `NYA_TERMINAL_COLOR_256`      | quantised to the cube  |
 * | any other terminal                        | `NYA_TERMINAL_COLOR_16`       | quantised to 16        |
 * | `TERM` is `dumb`, unset, or not a terminal | `NYA_TERMINAL_COLOR_NONE`     | all colour             |
 *
 * Images degrade the same way and further: without the kitty protocol `nya_terminal_image_draw`
 * draws nothing and says so, rather than printing a field of blocks that pretends to be a picture.
 *
 * ## Why raw termios and not ncurses
 *
 * ncurses was the obvious choice and lost on four counts, written here so nobody spends an evening
 * reintroducing it. It is a permanent dependency that also needs a terminfo database present at
 * *runtime*, which a single static binary cannot promise. It owns a global `SCREEN` and its own
 * idea of when to refresh, which fights a renderer that already has a frame loop. Its colour model
 * is pairs, so truecolor per cell is an extension you fight for. And it cannot carry the kitty
 * graphics protocol at all: the escape has to reach the terminal unmangled and with the cursor
 * where we put it, which is precisely what ncurses exists to take away. What replaced it is
 * `tcsetattr` plus the escapes in `terminal.c`, with no dependency and no runtime database.
 *
 * notcurses was rejected for the same dependency reason alone; it would do all of this well.
 *
 * ## Bounds
 *
 * Every buffer here is fixed and registered with `nya_ceiling_register`, so the debug overlay shows
 * how close a run came. A terminal larger than the grid is clamped and logged once, not grown.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_error.h"
#include "nyangine/base/base_types.h"

typedef enum NYA_TerminalColorDepth     NYA_TerminalColorDepth;
typedef enum NYA_TerminalAttributeFlag  NYA_TerminalAttributeFlag;
typedef enum NYA_TerminalKey            NYA_TerminalKey;
typedef enum NYA_TerminalInputKind      NYA_TerminalInputKind;
typedef enum NYA_TerminalModifierFlag   NYA_TerminalModifierFlag;
typedef enum NYA_TerminalMouseButton    NYA_TerminalMouseButton;
typedef struct NYA_TerminalCell         NYA_TerminalCell;
typedef struct NYA_TerminalCapabilities NYA_TerminalCapabilities;
typedef struct NYA_TerminalInput        NYA_TerminalInput;
typedef struct NYA_TerminalOptions      NYA_TerminalOptions;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Widest grid held. A 3840 pixel display at a 10 pixel cell is 384 columns, which is the widest a
 * terminal on hardware that exists can be; 400 covers it with room and keeps the grid under 2 MiB.
 * A wider terminal is clamped to this and the columns past it are not drawn.
 * */
#ifndef NYA_TERMINAL_COLUMNS_MAX
#define NYA_TERMINAL_COLUMNS_MAX 400
#endif

/** Tallest grid held. 2160 pixels at an 18 pixel row is 120 rows, by the same reasoning. */
#ifndef NYA_TERMINAL_ROWS_MAX
#define NYA_TERMINAL_ROWS_MAX 120
#endif

/** Cells in the largest grid. Two buffers of these are taken at open: the back one and the front one. */
#define NYA_TERMINAL_CELL_MAX ((u64)NYA_TERMINAL_COLUMNS_MAX * (u64)NYA_TERMINAL_ROWS_MAX)

/**
 * Bytes read from the terminal in one `nya_terminal_poll`. A key is at most 8 bytes and a mouse
 * report 16, so this holds a quarter of a second of the fastest key repeat anyone has configured
 * plus a paste, and a paste longer than it simply arrives over two polls.
 * */
#ifndef NYA_TERMINAL_READ_MAX
#define NYA_TERMINAL_READ_MAX 4096
#endif

/**
 * Inputs decoded from one read. `NYA_TERMINAL_READ_MAX` bytes of single byte keys would be more
 * than this, so the decoder stops and leaves the rest for the next poll rather than dropping any.
 * */
#ifndef NYA_TERMINAL_INPUT_MAX
#define NYA_TERMINAL_INPUT_MAX 128
#endif

/**
 * Bytes of a partly arrived escape sequence carried to the next poll. The longest sequence anything
 * here decodes is an SGR mouse report with three five digit numbers, 20 bytes; 32 is the next power
 * of two above it and bounds a hostile terminal that never sends a final byte.
 * */
#ifndef NYA_TERMINAL_RESIDUE_MAX
#define NYA_TERMINAL_RESIDUE_MAX 32
#endif

/**
 * Bytes buffered before a write goes out. One 64 KiB write per frame beats one per cell: a full
 * repaint of a 200x50 grid in truecolor is about 400 KiB, so a frame is seven writes rather than
 * ten thousand.
 * */
#ifndef NYA_TERMINAL_WRITE_MAX
#define NYA_TERMINAL_WRITE_MAX 65536
#endif

/**
 * Pixels one cell stands for horizontally, so a program lays a terminal out in the same units it
 * lays a window out in and the renderer divides. Eight is what a monospace cell is at a readable
 * size, and a power of two so the division is a shift.
 * */
#ifndef NYA_TERMINAL_CELL_WIDTH_PX
#define NYA_TERMINAL_CELL_WIDTH_PX 8
#endif

/** The same vertically. A terminal cell is about twice as tall as it is wide; this makes that exact. */
#ifndef NYA_TERMINAL_CELL_HEIGHT_PX
#define NYA_TERMINAL_CELL_HEIGHT_PX 16
#endif

/**
 * Pixels in the largest image `nya_terminal_image_draw` will send. 1920x1080 RGBA is 8 MiB of
 * payload and about 11 MiB once base64'd, which is already more than a terminal will draw in a
 * frame; anything larger is a caller mistake and is refused rather than buffered.
 * */
#ifndef NYA_TERMINAL_IMAGE_PIXELS_MAX
#define NYA_TERMINAL_IMAGE_PIXELS_MAX ((u64)1920 * (u64)1080)
#endif

/**
 * Payload bytes per kitty escape. The protocol specifies 4096 base64 characters per chunk, so this
 * is not a tunable in the usual sense: it is the number the protocol names.
 * */
#define NYA_TERMINAL_IMAGE_CHUNK_BYTES 4096

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** How much colour reaches the screen. Resolved once by the probe at open; see the table above. */
enum NYA_TerminalColorDepth {
    /** No colour at all. `TERM` is `dumb` or unset, or output is not a terminal. Attributes still work. */
    NYA_TERMINAL_COLOR_NONE = 0,

    /** The original eight, bright and dim. Every terminal that has colour has these. */
    NYA_TERMINAL_COLOR_16,

    /** The xterm 256 palette: a 6x6x6 cube plus 24 greys. */
    NYA_TERMINAL_COLOR_256,

    /** 24-bit, which is what a cell holds, so nothing is lost. */
    NYA_TERMINAL_COLOR_TRUE,

    NYA_TERMINAL_COLOR_COUNT,
};

/** Bits in `NYA_TerminalCell.attributes`. */
enum NYA_TerminalAttributeFlag {
    NYA_TERMINAL_ATTRIBUTE_NONE      = 0,
    NYA_TERMINAL_ATTRIBUTE_BOLD      = 1U << 0U,
    NYA_TERMINAL_ATTRIBUTE_DIM       = 1U << 1U,
    NYA_TERMINAL_ATTRIBUTE_UNDERLINE = 1U << 2U,

    /** Foreground and background swapped by the terminal. What carries emphasis when colour is NONE. */
    NYA_TERMINAL_ATTRIBUTE_REVERSE = 1U << 3U,
};

/**
 * One cell. Plain data, 16 bytes, and the grid is an array of these.
 *
 * Colours are packed `0x00RRGGBB` rather than `NYA_Color`, because a cell is compared against last
 * frame's on every present and four floats would make that four times the memory traffic for a
 * precision no terminal can show.
 * */
struct NYA_TerminalCell {
    /** The Unicode code point drawn. Zero and `' '` both mean blank; only the background shows. */
    u32 codepoint;

    /** Ink, `0x00RRGGBB`. */
    u32 foreground;

    /** Paper, `0x00RRGGBB`. */
    u32 background;

    /** A mask of NYA_TerminalAttributeFlag. */
    u8 attributes;

    u8 _padding[3];
};

static_assert(sizeof(NYA_TerminalCell) == 16, "a cell is compared every frame, so its size is load bearing");

/** What the probe at open found. Fixed for the run, except the size, which a resize updates. */
struct NYA_TerminalCapabilities {
    NYA_TerminalColorDepth color_depth;

    /** Whether the kitty graphics protocol is available. False means `nya_terminal_image_draw` draws nothing. */
    b8 kitty_images;

    /** Whether mouse reports were asked for and the terminal is one that sends them. */
    b8 mouse;

    /** Whether standard output is a terminal at all. False when the program is piped into something. */
    b8 is_terminal;

    /** `TERM` as it was found, truncated, never null. "" when unset. */
    char term[32];
};

/** What kind of thing `nya_terminal_poll` handed back. */
enum NYA_TerminalInputKind {
    /** The zero value, so a zeroed input is not mistaken for a keystroke. Never returned. */
    NYA_TERMINAL_INPUT_NONE = 0,

    /** A key. `key` says which, and `codepoint` is non-zero when it also produced a character. */
    NYA_TERMINAL_INPUT_KEY,

    /** A mouse button went down or up. `is_down`, `button`, `column` and `row`. */
    NYA_TERMINAL_INPUT_MOUSE_BUTTON,

    /** The mouse moved, with `column` and `row` where it now is. */
    NYA_TERMINAL_INPUT_MOUSE_MOVED,

    /** The wheel turned. `wheel` is +1 for up and -1 for down. */
    NYA_TERMINAL_INPUT_MOUSE_WHEEL,

    /** The terminal changed size. `column` and `row` carry the new columns and rows. */
    NYA_TERMINAL_INPUT_RESIZE,

    NYA_TERMINAL_INPUT_KIND_COUNT,
};

/**
 * The keys that are not simply a character. A key that *is* a character reports
 * `NYA_TERMINAL_KEY_NONE` and carries it in `codepoint`, so a decoder never has to invent a name
 * for `'q'`.
 * */
enum NYA_TerminalKey {
    NYA_TERMINAL_KEY_NONE = 0,

    NYA_TERMINAL_KEY_ESCAPE,
    NYA_TERMINAL_KEY_ENTER,
    NYA_TERMINAL_KEY_TAB,
    NYA_TERMINAL_KEY_BACKSPACE,
    NYA_TERMINAL_KEY_DELETE,
    NYA_TERMINAL_KEY_INSERT,

    NYA_TERMINAL_KEY_UP,
    NYA_TERMINAL_KEY_DOWN,
    NYA_TERMINAL_KEY_LEFT,
    NYA_TERMINAL_KEY_RIGHT,
    NYA_TERMINAL_KEY_HOME,
    NYA_TERMINAL_KEY_END,
    NYA_TERMINAL_KEY_PAGE_UP,
    NYA_TERMINAL_KEY_PAGE_DOWN,

    NYA_TERMINAL_KEY_F1,
    NYA_TERMINAL_KEY_F2,
    NYA_TERMINAL_KEY_F3,
    NYA_TERMINAL_KEY_F4,
    NYA_TERMINAL_KEY_F5,
    NYA_TERMINAL_KEY_F6,
    NYA_TERMINAL_KEY_F7,
    NYA_TERMINAL_KEY_F8,
    NYA_TERMINAL_KEY_F9,
    NYA_TERMINAL_KEY_F10,
    NYA_TERMINAL_KEY_F11,
    NYA_TERMINAL_KEY_F12,

    NYA_TERMINAL_KEY_COUNT,
};

/** Modifier bits, matching the xterm encoding minus its bias so they can be or'd. */
enum NYA_TerminalModifierFlag {
    NYA_TERMINAL_MODIFIER_NONE  = 0,
    NYA_TERMINAL_MODIFIER_SHIFT = 1U << 0U,
    NYA_TERMINAL_MODIFIER_ALT   = 1U << 1U,
    NYA_TERMINAL_MODIFIER_CTRL  = 1U << 2U,
};

/** Which button a mouse report named. The terminal reports three and a wheel, and no more. */
enum NYA_TerminalMouseButton {
    NYA_TERMINAL_MOUSE_BUTTON_NONE = 0,
    NYA_TERMINAL_MOUSE_BUTTON_LEFT,
    NYA_TERMINAL_MOUSE_BUTTON_MIDDLE,
    NYA_TERMINAL_MOUSE_BUTTON_RIGHT,

    NYA_TERMINAL_MOUSE_BUTTON_COUNT,
};

/**
 * One decoded thing from the terminal. Which fields mean anything is decided by `kind`; the rest
 * are zero.
 * */
struct NYA_TerminalInput {
    NYA_TerminalInputKind kind;

    /** Which key, for a KEY. `NYA_TERMINAL_KEY_NONE` when the key is just its character. */
    NYA_TerminalKey key;

    /** The character produced, for a KEY that produced one. Zero otherwise. */
    u32 codepoint;

    /** A mask of NYA_TerminalModifierFlag. */
    u16 modifiers;

    /** Zero based cell the mouse is over, for the three mouse kinds. New size, for a RESIZE. */
    u16 column;
    u16 row;

    NYA_TerminalMouseButton button;

    /** Whether a MOUSE_BUTTON is a press. */
    b8 is_down;

    /** +1 up, -1 down, for a MOUSE_WHEEL. Zero otherwise. */
    s8 wheel;
};

/** What `nya_terminal_open` is asked for. Zeroed asks for the plainest terminal that works. */
struct NYA_TerminalOptions {
    /**
     * Switch to the alternate screen, so what the user had in their scrollback comes back on close.
     * A program that wants its output to stay in the scrollback leaves this off.
     * */
    b8 alternate_screen;

    /** Ask the terminal for mouse reports. Off means `nya_terminal_poll` returns no mouse kinds. */
    b8 mouse;

    /**
     * Leave the cursor visible. Off, which is the default, hides it, because a cursor parked
     * wherever the last cell was written is the single most obvious tell of a TUI drawn badly.
     * */
    b8 cursor_visible;
};

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Puts the terminal into raw mode, probes it once, takes the two cell buffers and starts listening
 * for resizes. Logs one line saying what the probe found and what is degraded.
 *
 * Fails, without having changed anything, when standard input is not a terminal or the OS refuses
 * raw mode. That is an operating error and not a crash: a program piped into a file should say so
 * and carry on, and `nya_terminal_is_open` stays false.
 *
 * Calling it twice is the programmer error and asserts.
 * */
NYA_API NYA_Error nya_terminal_open(NYA_TerminalOptions options) __attr_no_discard;

/**
 * Puts everything back: cooked mode, mouse reporting off, the cursor visible, the main screen. Safe
 * to call when open failed or was never called, which is what makes it usable from a crash handler.
 * */
NYA_API void nya_terminal_close(void);

/** Whether the pair above currently holds the terminal. */
NYA_API b8 nya_terminal_is_open(void) __attr_no_discard;

/** What the probe found. Zeroed, meaning no colour and no images, when the terminal is not open. */
NYA_API NYA_TerminalCapabilities nya_terminal_capabilities(void) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE GRID
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Columns right now, clamped to NYA_TERMINAL_COLUMNS_MAX. Zero when not open. */
NYA_API u16 nya_terminal_columns(void) __attr_no_discard;

/** Rows right now, clamped to NYA_TERMINAL_ROWS_MAX. Zero when not open. */
NYA_API u16 nya_terminal_rows(void) __attr_no_discard;

/** Fills the back buffer with blanks on `background`. What a frame starts with. */
NYA_API void nya_terminal_clear(u32 background);

/** Writes one cell of the back buffer. Out of range is ignored, because clipping is the caller's win. */
NYA_API void nya_terminal_cell_set(u16 column, u16 row, NYA_TerminalCell cell);

/** Reads one cell of the back buffer. A zeroed cell when out of range or not open. */
NYA_API NYA_TerminalCell nya_terminal_cell_get(u16 column, u16 row) __attr_no_discard;

/**
 * Writes every cell that differs from what is on screen, then makes the back buffer the front one.
 * Nothing at all goes out when nothing changed, which is what makes an idle TUI cost nothing.
 * */
NYA_API void nya_terminal_present(void);

/**
 * Forgets what is on screen, so the next present writes every cell. What a resize needs, and what
 * a program calls after shelling out to something that drew over the screen.
 * */
NYA_API void nya_terminal_invalidate(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INPUT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Everything the terminal has said since the last call, decoded. Never blocks: a terminal with
 * nothing to say returns zero. Bytes that are half an escape sequence are kept and joined with the
 * next read, so a sequence split across two reads still decodes once.
 *
 * Returns how many of `capacity` were filled.
 * */
NYA_API u32 nya_terminal_poll(OUT NYA_TerminalInput* out, u32 capacity) __attr_no_discard;

/**
 * The same decoder, as a pure function over bytes you already have. What the tests and the fuzzer
 * drive, and what makes the escape parsing checkable without a terminal.
 *
 * `is_final` says there are no more bytes coming for now, which is the only thing that tells a lone
 * `ESC` from the start of an arrow key. Writes how many bytes were consumed to `out_consumed`; what
 * is left is an incomplete sequence the caller keeps for next time.
 * */
NYA_API u32 nya_terminal_input_decode(const u8* bytes, u64 size, b8 is_final, OUT NYA_TerminalInput* out, u32 capacity, OUT u64* out_consumed)
    __attr_no_discard;

/** A key's name, for a log line or a help row. "escape", "f10", "none". Never null. */
NYA_API NYA_ConstCString nya_terminal_key_name(NYA_TerminalKey key) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * IMAGES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Sends `width` x `height` RGBA pixels to the terminal through the kitty graphics protocol, with
 * its top left corner at the given cell. Goes out immediately rather than into the cell grid, since
 * a picture is not made of cells; the cells it covers are left as they are, so a caller that wants
 * them blank clears them.
 *
 * False, having drawn nothing, when the terminal has no kitty protocol, when the terminal is not
 * open, or when the image is larger than NYA_TERMINAL_IMAGE_PIXELS_MAX. That is the documented
 * degradation: a TUI on a terminal without images shows the rest of itself.
 * */
NYA_API b8 nya_terminal_image_draw(u16 column, u16 row, const u8* rgba, u32 width, u32 height) __attr_no_discard;

/** Removes every image this module has placed. A no-op where images are unsupported. */
NYA_API void nya_terminal_image_clear(void);

/** `NYA_Color`'s four floats as the `0x00RRGGBB` a cell holds. Each component is clamped. */
NYA_API u32 nya_terminal_ink(f32 red, f32 green, f32 blue) __attr_no_discard;

/**
 * One code point out of UTF-8 bytes. Returns how many bytes it took, and 0 when the character has
 * not all arrived so the caller should wait for more.
 *
 * Public because the decoder and the terminal renderer both walk UTF-8, and a second copy of this
 * is the kind of near-duplicate that drifts. Malformed input yields one byte of U+FFFD rather than
 * resynchronising, so a hostile stream cannot make a caller's loop scan forward.
 * */
NYA_API u32 nya_terminal_utf8_decode(const u8* bytes, u64 size, OUT u32* out_codepoint) __attr_no_discard;
