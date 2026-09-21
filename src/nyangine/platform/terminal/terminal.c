/**
 * @file terminal.c
 *
 * Everything about a terminal that is the same on every OS: the cell grid and its damage, the
 * escapes that paint it, the colour degradation, the escape sequence decoder, and the kitty
 * graphics protocol. The six calls that are not the same are declared here and defined in
 * terminal_linux.c and terminal_windows.c. See terminal.h.
 * */
#include "nyangine/base/base.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * The six calls that differ per OS. terminal_linux.c and terminal_windows.c define exactly these and
 * nothing else; everything below is shared. Declared here rather than in terminal.h because
 * NYA_INTERNAL is static plus hidden visibility and never belongs in a public header.
 */

/** Raw mode on, saving what to put back. False when this is not a terminal or the OS refused. */
NYA_INTERNAL b8 _nya_terminal_raw_mode_begin(void) __attr_no_discard;

/** Raw mode off, from whatever state. Idempotent, and safe after a failed begin. */
NYA_INTERNAL void _nya_terminal_raw_mode_end(void);

/** The terminal's size in cells. False when the OS will not say, leaving the outputs alone. */
NYA_INTERNAL b8 _nya_terminal_size_query(OUT u16* out_columns, OUT u16* out_rows) __attr_no_discard;

/** Whether a resize arrived since the last call, clearing the flag. */
NYA_INTERNAL b8 _nya_terminal_resized_take(void) __attr_no_discard;

/** Writes `size` bytes to the terminal, retrying a short write. False when the terminal went away. */
NYA_INTERNAL b8 _nya_terminal_send(const u8* bytes, u64 size) __attr_no_discard;

/** Reads what is waiting without blocking. Zero when nothing is, which is the common case. */
NYA_INTERNAL u64 _nya_terminal_receive(OUT u8* buffer, u64 capacity) __attr_no_discard;

/** Whatever `name` is set to in the environment, or null. One place, so the probe is one function. */
NYA_INTERNAL NYA_ConstCString _nya_terminal_environment(NYA_ConstCString name) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The environment the probe reads, once, at open. Named because a typo in one of these degrades a
 * perfectly capable terminal silently, which is the hardest kind of bug to notice.
 */
#define _NYA_TERMINAL_ENV_TERM       "TERM"
#define _NYA_TERMINAL_ENV_COLORTERM  "COLORTERM"
#define _NYA_TERMINAL_ENV_KITTY      "KITTY_WINDOW_ID"
#define _NYA_TERMINAL_ENV_TERM_PROGRAM "TERM_PROGRAM"

/** `TERM` for a terminal that can do nothing at all. The one value with a defined meaning. */
#define _NYA_TERMINAL_TERM_DUMB "dumb"

/**
 * The xterm palette's first sixteen colours, which is what NYA_TERMINAL_COLOR_16 quantises to.
 * Straight out of xterm's `charproc.c` defaults, which every terminal emulator copies; they are not
 * measured or chosen, and a terminal with a user theme will show its own and that is fine.
 * */
NYA_INTERNAL const u32 _NYA_TERMINAL_PALETTE_16[16] = {
    0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
    0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

/** Levels the 6x6x6 cube samples each channel at, per the xterm 256 colour definition. */
NYA_INTERNAL const u8 _NYA_TERMINAL_CUBE_LEVELS[6] = { 0, 95, 135, 175, 215, 255 };

/** The default ink a cleared cell carries: plain light grey, so text drawn without a colour is readable. */
#define _NYA_TERMINAL_INK_DEFAULT 0xC0C0C0U

/** Longest decimal a u32 parameter can be, so the escape parser can reject a number that cannot be one. */
#define _NYA_TERMINAL_PARAMETER_DIGITS_MAX 5

/** Parameters kept from one CSI sequence. SGR mouse uses three and nothing here uses more. */
#define _NYA_TERMINAL_PARAMETERS_MAX 4

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * TYPES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** What one step of the decoder did with the bytes it was shown. */
typedef enum {
    /** Filled the input and consumed bytes. */
    _NYA_TERMINAL_DECODE_EMIT,

    /** Consumed bytes and had nothing to report: a sequence this module does not decode. */
    _NYA_TERMINAL_DECODE_SKIP,

    /** Consumed nothing; the sequence has not finished arriving. */
    _NYA_TERMINAL_DECODE_INCOMPLETE,
} _NYA_TerminalDecode;

/** The module's one piece of state. */
typedef struct {
    b8                       open;
    NYA_TerminalOptions      options;
    NYA_TerminalCapabilities capabilities;

    u16 columns;
    u16 rows;

    /** Both taken once at open, at NYA_TERMINAL_CELL_MAX, and never resized. */
    NYA_TerminalCell* back;
    NYA_TerminalCell* front;

    /** Cells the current size uses, which is what the ceiling registry watches. */
    u32 cells_live;

    /** False until a present has written the whole screen. A resize puts it back. */
    b8 front_valid;

    NYA_Arena* arena;

    u8  write[NYA_TERMINAL_WRITE_MAX];
    u64 write_used;

    /** Half an escape sequence, carried to the next poll. */
    u8  residue[NYA_TERMINAL_RESIDUE_MAX];
    u64 residue_size;
    u32 residue_live;

    /** Ids handed to the kitty protocol, so a later clear names exactly what this module placed. */
    u32 image_next_id;
    u32 image_count;
} _NYA_Terminal;

NYA_INTERNAL _NYA_Terminal _nya_terminal = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL: THE WRITE BUFFER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Pushes the buffer at the terminal and empties it. */
NYA_INTERNAL void _nya_terminal_flush(void) {
    if (_nya_terminal.write_used == 0) return;

    // a terminal that went away is an operating error on every frame after the first, so it is
    // logged by the write itself and dropped here rather than retried forever.
    (void)_nya_terminal_send(_nya_terminal.write, _nya_terminal.write_used);
    _nya_terminal.write_used = 0;
}

/** Appends bytes, flushing whenever the buffer is full, so the buffer bounds memory and not output. */
NYA_INTERNAL void _nya_terminal_push(const u8* bytes, u64 size) {
    nya_assert(bytes != nullptr || size == 0);

    u64 at = 0;
    while (at < size) {
        u64 room = NYA_TERMINAL_WRITE_MAX - _nya_terminal.write_used;
        if (room == 0) {
            _nya_terminal_flush();
            continue;
        }

        u64 take = nya_min(room, size - at);
        nya_memcpy(&_nya_terminal.write[_nya_terminal.write_used], &bytes[at], take);

        _nya_terminal.write_used += take;
        at                       += take;
    }
}

NYA_INTERNAL void _nya_terminal_push_cstring(NYA_ConstCString text) {
    nya_assert(text != nullptr);
    _nya_terminal_push((const u8*)text, (u64)strlen(text));
}

/** A decimal number, without printf, because this runs per cell and printf would dominate the frame. */
NYA_INTERNAL void _nya_terminal_push_number(u32 value) {
    u8  digits[10];
    u32 count = 0;

    do {
        digits[count++] = (u8)('0' + (value % 10U));
        value          /= 10U;
    } while (value > 0 && count < nya_carray_length(digits));

    u8 reversed[10];
    for (u32 i = 0; i < count; i++) reversed[i] = digits[count - 1 - i];

    _nya_terminal_push(reversed, count);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL: COLOUR
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Squared distance in RGB. Good enough to pick a palette entry, and no square root on a per cell path. */
NYA_INTERNAL u32 _nya_terminal_distance(u32 a, u32 b) {
    s32 delta_red   = (s32)((a >> 16U) & 0xFFU) - (s32)((b >> 16U) & 0xFFU);
    s32 delta_green = (s32)((a >> 8U) & 0xFFU) - (s32)((b >> 8U) & 0xFFU);
    s32 delta_blue  = (s32)(a & 0xFFU) - (s32)(b & 0xFFU);

    return (u32)((delta_red * delta_red) + (delta_green * delta_green) + (delta_blue * delta_blue));
}

/** The nearest of the sixteen. */
NYA_INTERNAL u8 _nya_terminal_index_16(u32 rgb) {
    u8  best          = 0;
    u32 best_distance = 0xFFFFFFFFU;

    for (u8 i = 0; i < 16; i++) {
        u32 distance = _nya_terminal_distance(rgb, _NYA_TERMINAL_PALETTE_16[i]);
        if (distance < best_distance) {
            best          = i;
            best_distance = distance;
        }
    }

    return best;
}

/** The nearest of the 256: the grey ramp when the channels agree, the 6x6x6 cube otherwise. */
NYA_INTERNAL u8 _nya_terminal_index_256(u32 rgb) {
    u32 red   = (rgb >> 16U) & 0xFFU;
    u32 green = (rgb >> 8U) & 0xFFU;
    u32 blue  = rgb & 0xFFU;

    // the ramp has 24 steps against the cube's 6, so a grey that is nearly grey still belongs on it.
    u32 spread = nya_max(red, nya_max(green, blue)) - nya_min(red, nya_min(green, blue));
    if (spread < 8) {
        u32 level = (red + green + blue) / 3U;

        // the ramp runs 8, 18, ... 238 as indices 232..255; outside it the cube's own black and white are closer.
        if (level < 8) return 16;
        if (level > 238) return 231;

        return (u8)(232U + ((level - 8U) / 10U));
    }

    u8 axis[3] = { 0, 0, 0 };
    u32 channels[3] = { red, green, blue };

    for (u32 channel = 0; channel < 3; channel++) {
        u8  best          = 0;
        u32 best_distance = 0xFFFFFFFFU;

        for (u8 level = 0; level < 6; level++) {
            s32 delta    = (s32)channels[channel] - (s32)_NYA_TERMINAL_CUBE_LEVELS[level];
            u32 distance = (u32)(delta * delta);

            if (distance < best_distance) {
                best          = level;
                best_distance = distance;
            }
        }

        axis[channel] = best;
    }

    return (u8)(16U + (36U * axis[0]) + (6U * axis[1]) + axis[2]);
}

/** One SGR colour, degraded to whatever the probe found. Writes nothing at NYA_TERMINAL_COLOR_NONE. */
NYA_INTERNAL void _nya_terminal_push_color(b8 is_foreground, u32 rgb) {
    switch (_nya_terminal.capabilities.color_depth) {
        case NYA_TERMINAL_COLOR_NONE: return;

        case NYA_TERMINAL_COLOR_16: {
            u8 index = _nya_terminal_index_16(rgb);

            // 30..37 and 40..47 are the dim eight; 90..97 and 100..107 the bright eight.
            u32 base = index < 8 ? (is_foreground ? 30U : 40U) : (is_foreground ? 82U : 92U);

            _nya_terminal_push_cstring("\x1b[");
            _nya_terminal_push_number(base + index);
            _nya_terminal_push_cstring("m");
        } break;

        case NYA_TERMINAL_COLOR_256: {
            _nya_terminal_push_cstring(is_foreground ? "\x1b[38;5;" : "\x1b[48;5;");
            _nya_terminal_push_number(_nya_terminal_index_256(rgb));
            _nya_terminal_push_cstring("m");
        } break;

        case NYA_TERMINAL_COLOR_TRUE: {
            _nya_terminal_push_cstring(is_foreground ? "\x1b[38;2;" : "\x1b[48;2;");
            _nya_terminal_push_number((rgb >> 16U) & 0xFFU);
            _nya_terminal_push_cstring(";");
            _nya_terminal_push_number((rgb >> 8U) & 0xFFU);
            _nya_terminal_push_cstring(";");
            _nya_terminal_push_number(rgb & 0xFFU);
            _nya_terminal_push_cstring("m");
        } break;

        // a depth outside the enum means the probe wrote something that is not a depth.
        case NYA_TERMINAL_COLOR_COUNT:
        default:                       nya_unreachable();
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL: UTF-8
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Encodes one code point. Returns how many bytes it wrote, and 0 for a code point that is not one. */
NYA_INTERNAL u32 _nya_terminal_utf8_encode(u32 codepoint, OUT u8 out[4]) {
    nya_assert(out != nullptr);

    // surrogates are not code points, and above 0x10FFFF is not Unicode. Both would produce bytes a
    // terminal reads as something else entirely, so neither is encoded.
    if (codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) return 0;

    if (codepoint < 0x80U) {
        out[0] = (u8)codepoint;
        return 1;
    }

    if (codepoint < 0x800U) {
        out[0] = (u8)(0xC0U | (codepoint >> 6U));
        out[1] = (u8)(0x80U | (codepoint & 0x3FU));
        return 2;
    }

    if (codepoint < 0x10000U) {
        out[0] = (u8)(0xE0U | (codepoint >> 12U));
        out[1] = (u8)(0x80U | ((codepoint >> 6U) & 0x3FU));
        out[2] = (u8)(0x80U | (codepoint & 0x3FU));
        return 3;
    }

    out[0] = (u8)(0xF0U | (codepoint >> 18U));
    out[1] = (u8)(0x80U | ((codepoint >> 12U) & 0x3FU));
    out[2] = (u8)(0x80U | ((codepoint >> 6U) & 0x3FU));
    out[3] = (u8)(0x80U | (codepoint & 0x3FU));
    return 4;
}

u32 nya_terminal_utf8_decode(const u8* bytes, u64 size, OUT u32* out_codepoint) {
    nya_assert(bytes != nullptr && out_codepoint != nullptr && size > 0);

    u8 lead = bytes[0];

    if (lead < 0x80U) {
        *out_codepoint = lead;
        return 1;
    }

    u32 length = 0;
    u32 value  = 0;

    if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
        value  = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
        value  = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
        value  = lead & 0x07U;
    } else {
        *out_codepoint = 0xFFFDU;
        return 1;
    }

    if (size < length) return 0;

    for (u32 i = 1; i < length; i++) {
        if ((bytes[i] & 0xC0U) != 0x80U) {
            *out_codepoint = 0xFFFDU;
            return 1;
        }

        value = (value << 6U) | (bytes[i] & 0x3FU);
    }

    *out_codepoint = value;
    return length;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL: THE PROBE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Reads the environment once and decides what this terminal can do, then says out loud what was
 * lost. The style guide's environment rule: probe at startup, in one place, never mid-operation.
 * */
NYA_INTERNAL NYA_TerminalCapabilities _nya_terminal_probe(NYA_TerminalOptions options, b8 is_terminal) {
    NYA_TerminalCapabilities capabilities = { .is_terminal = is_terminal };

    NYA_ConstCString term       = _nya_terminal_environment(_NYA_TERMINAL_ENV_TERM);
    NYA_ConstCString color_term = _nya_terminal_environment(_NYA_TERMINAL_ENV_COLORTERM);
    NYA_ConstCString kitty_id   = _nya_terminal_environment(_NYA_TERMINAL_ENV_KITTY);
    NYA_ConstCString program    = _nya_terminal_environment(_NYA_TERMINAL_ENV_TERM_PROGRAM);

    if (term != nullptr) {
        u64 length = nya_min((u64)strlen(term), (u64)sizeof(capabilities.term) - 1U);
        nya_memcpy(capabilities.term, term, length);
        capabilities.term[length] = '\0';
    }

    b8 dumb = term == nullptr || term[0] == '\0' || nya_string_equals(term, _NYA_TERMINAL_TERM_DUMB);

    if (!is_terminal || dumb) {
        capabilities.color_depth = NYA_TERMINAL_COLOR_NONE;
    } else if (color_term != nullptr && (nya_string_contains(color_term, "truecolor") || nya_string_contains(color_term, "24bit"))) {
        capabilities.color_depth = NYA_TERMINAL_COLOR_TRUE;
    } else if (nya_string_contains(term, "256color") || nya_string_contains(term, "direct")) {
        capabilities.color_depth = NYA_TERMINAL_COLOR_256;
    } else {
        capabilities.color_depth = NYA_TERMINAL_COLOR_16;
    }

    /*
     * There is no reply-free way to ask whether the kitty protocol is there, and the query form
     * needs a round trip this module deliberately does not do at startup: a terminal that ignores it
     * leaves the program waiting on a read that never answers. So this is by name, which is the same
     * list every other TUI carries, and being wrong degrades to a missing picture rather than to
     * garbage on screen.
     */
    capabilities.kitty_images = is_terminal
                                && ((kitty_id != nullptr && kitty_id[0] != '\0') || nya_string_contains(capabilities.term, "kitty")
                                    || (program != nullptr && (nya_string_equals(program, "ghostty") || nya_string_equals(program, "WezTerm"))));

    capabilities.mouse = options.mouse && is_terminal && !dumb;

    return capabilities;
}

/** One line naming everything the probe took away, so a washed out TUI is explained and not guessed at. */
NYA_INTERNAL void _nya_terminal_report(const NYA_TerminalCapabilities* capabilities, NYA_TerminalOptions options) {
    NYA_ConstCString depth_names[NYA_TERMINAL_COLOR_COUNT] = {
        [NYA_TERMINAL_COLOR_NONE] = "no colour",
        [NYA_TERMINAL_COLOR_16]   = "16 colours",
        [NYA_TERMINAL_COLOR_256]  = "256 colours",
        [NYA_TERMINAL_COLOR_TRUE] = "truecolor",
    };

    nya_log_info("Terminal opened: TERM=%s, %s, %ux%u cells.", capabilities->term[0] != '\0' ? capabilities->term : "(unset)",
                 depth_names[capabilities->color_depth], _nya_terminal.columns, _nya_terminal.rows);

    if (capabilities->color_depth != NYA_TERMINAL_COLOR_TRUE) {
        nya_log_warn("Terminal colour is degraded to %s; set COLORTERM=truecolor on a terminal that supports it.",
                     depth_names[capabilities->color_depth]);
    }

    if (!capabilities->kitty_images) {
        nya_log_warn("Terminal has no kitty graphics protocol; images will not be drawn. kitty, ghostty and WezTerm have it.");
    }

    if (options.mouse && !capabilities->mouse) nya_log_warn("Terminal mouse reporting was asked for and is not available.");
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL: SIZE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Re-reads the size, clamps it to the grid, and reports whether it moved. */
NYA_INTERNAL b8 _nya_terminal_size_refresh(void) {
    u16 columns = 0;
    u16 rows    = 0;

    // a terminal that will not say its size keeps the one it had, which is better than collapsing to nothing.
    if (!_nya_terminal_size_query(&columns, &rows)) return false;

    if (columns > NYA_TERMINAL_COLUMNS_MAX || rows > NYA_TERMINAL_ROWS_MAX) {
        // once, not per resize: a terminal dragged wider than the grid would otherwise log per drag step.
        static b8 warned = false;
        if (!warned) {
            warned = true;
            nya_log_warn("Terminal is %ux%u cells and the grid holds %dx%d; the rest is not drawn.", columns, rows, NYA_TERMINAL_COLUMNS_MAX,
                         NYA_TERMINAL_ROWS_MAX);
        }
    }

    columns = (u16)nya_min((u32)columns, (u32)NYA_TERMINAL_COLUMNS_MAX);
    rows    = (u16)nya_min((u32)rows, (u32)NYA_TERMINAL_ROWS_MAX);

    if (columns == _nya_terminal.columns && rows == _nya_terminal.rows) return false;

    _nya_terminal.columns    = columns;
    _nya_terminal.rows       = rows;
    _nya_terminal.cells_live = (u32)columns * (u32)rows;

    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_terminal_open(NYA_TerminalOptions options) {
    nya_assert(!_nya_terminal.open, "nya_terminal_open called twice without a close");

    if (!_nya_terminal_raw_mode_begin()) {
        return nya_error(NYA_ERROR_NOT_SUPPORTED, "the terminal will not go into raw mode; is standard input a terminal?");
    }

    _nya_terminal.options = options;
    _nya_terminal.arena   = nya_arena_create(.name = "terminal");

    _nya_terminal.back  = nya_arena_alloc(_nya_terminal.arena, (u64)sizeof(NYA_TerminalCell) * (u64)NYA_TERMINAL_CELL_MAX);
    _nya_terminal.front = nya_arena_alloc(_nya_terminal.arena, (u64)sizeof(NYA_TerminalCell) * (u64)NYA_TERMINAL_CELL_MAX);

    if (_nya_terminal.back == nullptr || _nya_terminal.front == nullptr) {
        nya_arena_destroy(_nya_terminal.arena);
        _nya_terminal.arena = nullptr;
        _nya_terminal_raw_mode_end();

        return nya_error(NYA_ERROR_OUT_OF_MEMORY, "the terminal grid needs " FMTu64 " bytes",
                         (u64)sizeof(NYA_TerminalCell) * (u64)NYA_TERMINAL_CELL_MAX * 2U);
    }

    // 80x24 is the floor every terminal has honoured since the VT100, so a terminal that will not
    // say its size gets one rather than a grid with no cells in it.
    _nya_terminal.columns = 80;
    _nya_terminal.rows    = 24;
    (void)_nya_terminal_size_refresh();
    _nya_terminal.cells_live = (u32)_nya_terminal.columns * (u32)_nya_terminal.rows;

    _nya_terminal.capabilities = _nya_terminal_probe(options, true);
    _nya_terminal.open         = true;

#ifndef NYA_NO_SDL
    // skipped under -DNYA_NO_SDL, which is the build tool and has no ceiling registry.
    nya_ceiling_register("terminal_cells", (u32)NYA_TERMINAL_CELL_MAX, &_nya_terminal.cells_live);
    nya_ceiling_register("terminal_input_residue", NYA_TERMINAL_RESIDUE_MAX, &_nya_terminal.residue_live);
#endif

    if (options.alternate_screen) _nya_terminal_push_cstring("\x1b[?1049h");
    if (!options.cursor_visible) _nya_terminal_push_cstring("\x1b[?25l");

    // 1000 button reports, 1002 motion while a button is held, 1006 the SGR encoding that is the
    // only one with coordinates past column 223. Asked for together, since a terminal that has any
    // of them has all three.
    if (_nya_terminal.capabilities.mouse) _nya_terminal_push_cstring("\x1b[?1000h\x1b[?1002h\x1b[?1006h");

    _nya_terminal_push_cstring("\x1b[2J");
    _nya_terminal_flush();

    nya_terminal_clear(0);
    nya_terminal_invalidate();

    _nya_terminal_report(&_nya_terminal.capabilities, options);

    return NYA_OK;
}

void nya_terminal_close(void) {
    // idempotent, and safe after a failed open, which is what lets a crash handler call it.
    if (!_nya_terminal.open) return;

    nya_terminal_image_clear();

    if (_nya_terminal.capabilities.mouse) _nya_terminal_push_cstring("\x1b[?1006l\x1b[?1002l\x1b[?1000l");

    // attributes off before anything else, or the shell inherits whatever the last cell was.
    _nya_terminal_push_cstring("\x1b[0m");

    if (!_nya_terminal.options.cursor_visible) _nya_terminal_push_cstring("\x1b[?25h");
    if (_nya_terminal.options.alternate_screen) _nya_terminal_push_cstring("\x1b[?1049l");

    _nya_terminal_flush();
    _nya_terminal_raw_mode_end();

    nya_arena_destroy(_nya_terminal.arena);

    // wholesale, so a second open starts from the same state the first did.
    _nya_terminal = (_NYA_Terminal){ 0 };

    nya_log_info("Terminal closed.");
}

b8 nya_terminal_is_open(void) {
    return _nya_terminal.open;
}

NYA_TerminalCapabilities nya_terminal_capabilities(void) {
    return _nya_terminal.capabilities;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE GRID
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 nya_terminal_ink(f32 red, f32 green, f32 blue) {
    u32 r = (u32)(nya_clamp(red, 0.0F, 1.0F) * 255.0F + 0.5F);
    u32 g = (u32)(nya_clamp(green, 0.0F, 1.0F) * 255.0F + 0.5F);
    u32 b = (u32)(nya_clamp(blue, 0.0F, 1.0F) * 255.0F + 0.5F);

    return (r << 16U) | (g << 8U) | b;
}

u16 nya_terminal_columns(void) {
    return _nya_terminal.open ? _nya_terminal.columns : 0;
}

u16 nya_terminal_rows(void) {
    return _nya_terminal.open ? _nya_terminal.rows : 0;
}

void nya_terminal_clear(u32 background) {
    if (!_nya_terminal.open) return;

    NYA_TerminalCell blank = { .codepoint = ' ', .foreground = _NYA_TERMINAL_INK_DEFAULT, .background = background & 0x00FFFFFFU };

    for (u32 i = 0; i < _nya_terminal.cells_live; i++) _nya_terminal.back[i] = blank;
}

void nya_terminal_cell_set(u16 column, u16 row, NYA_TerminalCell cell) {
    // clipping here rather than at every call site is the whole reason a renderer can be simple.
    if (!_nya_terminal.open || column >= _nya_terminal.columns || row >= _nya_terminal.rows) return;

    cell.foreground &= 0x00FFFFFFU;
    cell.background &= 0x00FFFFFFU;

    _nya_terminal.back[((u32)row * _nya_terminal.columns) + column] = cell;
}

NYA_TerminalCell nya_terminal_cell_get(u16 column, u16 row) {
    if (!_nya_terminal.open || column >= _nya_terminal.columns || row >= _nya_terminal.rows) return (NYA_TerminalCell){ 0 };

    return _nya_terminal.back[((u32)row * _nya_terminal.columns) + column];
}

void nya_terminal_invalidate(void) {
    _nya_terminal.front_valid = false;
}

void nya_terminal_present(void) {
    if (!_nya_terminal.open) return;

    /*
     * One SGR run for as long as the style holds, and one cursor move only where the run breaks. A
     * per cell "move, colour, character" is correct and costs about four times the bytes, which on a
     * remote session is the difference between a smooth redraw and a visible sweep.
     */
    u32 style_foreground = 0xFFFFFFFFU;
    u32 style_background = 0xFFFFFFFFU;
    u32 style_attributes = 0xFFFFFFFFU;

    b8  cursor_known  = false;
    u16 cursor_column = 0;
    u16 cursor_row    = 0;

    // everything, not just the damage, when nothing on screen can be trusted.
    b8 repaint = !_nya_terminal.front_valid;
    if (repaint) _nya_terminal_push_cstring("\x1b[H\x1b[2J");

    for (u16 row = 0; row < _nya_terminal.rows; row++) {
        for (u16 column = 0; column < _nya_terminal.columns; column++) {
            u32                     index = ((u32)row * _nya_terminal.columns) + column;
            const NYA_TerminalCell* cell  = &_nya_terminal.back[index];

            if (!repaint && nya_memcmp(cell, &_nya_terminal.front[index], sizeof(NYA_TerminalCell)) == 0) continue;

            if (!cursor_known || cursor_row != row || cursor_column != column) {
                _nya_terminal_push_cstring("\x1b[");
                _nya_terminal_push_number((u32)row + 1U);
                _nya_terminal_push_cstring(";");
                _nya_terminal_push_number((u32)column + 1U);
                _nya_terminal_push_cstring("H");

                cursor_known = true;
                cursor_row   = row;
            }

            if (cell->attributes != style_attributes) {
                // reset first: there is no "underline off" that leaves the colours, so the colours follow.
                _nya_terminal_push_cstring("\x1b[0m");

                if ((cell->attributes & NYA_TERMINAL_ATTRIBUTE_BOLD) != 0) _nya_terminal_push_cstring("\x1b[1m");
                if ((cell->attributes & NYA_TERMINAL_ATTRIBUTE_DIM) != 0) _nya_terminal_push_cstring("\x1b[2m");
                if ((cell->attributes & NYA_TERMINAL_ATTRIBUTE_UNDERLINE) != 0) _nya_terminal_push_cstring("\x1b[4m");
                if ((cell->attributes & NYA_TERMINAL_ATTRIBUTE_REVERSE) != 0) _nya_terminal_push_cstring("\x1b[7m");

                style_attributes = cell->attributes;
                style_foreground = 0xFFFFFFFFU;
                style_background = 0xFFFFFFFFU;
            }

            if (cell->foreground != style_foreground) {
                _nya_terminal_push_color(true, cell->foreground);
                style_foreground = cell->foreground;
            }

            if (cell->background != style_background) {
                _nya_terminal_push_color(false, cell->background);
                style_background = cell->background;
            }

            u8  utf8[4];
            u32 length = _nya_terminal_utf8_encode(cell->codepoint == 0 ? ' ' : cell->codepoint, utf8);

            // a code point that is not one draws as a space rather than as whatever the bytes happen to mean.
            if (length == 0) {
                utf8[0] = ' ';
                length  = 1;
            }

            _nya_terminal_push(utf8, length);

            // where the terminal's own cursor now is, so the next damaged cell in this row needs no move.
            cursor_column = (u16)(column + 1U);
        }
    }

    // attributes released at the end of the frame, so a program that writes to stdout between
    // frames, or crashes, does not inherit the last cell's colours.
    if (style_attributes != 0xFFFFFFFFU || style_foreground != 0xFFFFFFFFU) _nya_terminal_push_cstring("\x1b[0m");

    _nya_terminal_flush();

    nya_memcpy(_nya_terminal.front, _nya_terminal.back, (u64)sizeof(NYA_TerminalCell) * (u64)_nya_terminal.cells_live);
    _nya_terminal.front_valid = true;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL: THE DECODER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** xterm's modifier parameter: one past a mask of 1 shift, 2 alt, 4 ctrl. Zero and 1 both mean none. */
NYA_INTERNAL u16 _nya_terminal_modifiers_of(u32 parameter) {
    if (parameter < 2) return NYA_TERMINAL_MODIFIER_NONE;

    u32 mask      = parameter - 1U;
    u16 modifiers = 0;

    if ((mask & 1U) != 0) modifiers |= NYA_TERMINAL_MODIFIER_SHIFT;
    if ((mask & 2U) != 0) modifiers |= NYA_TERMINAL_MODIFIER_ALT;
    if ((mask & 4U) != 0) modifiers |= NYA_TERMINAL_MODIFIER_CTRL;

    return modifiers;
}

/** What `ESC[<n>~` means. NYA_TERMINAL_KEY_NONE for the numbers nothing here decodes. */
NYA_INTERNAL NYA_TerminalKey _nya_terminal_key_of_tilde(u32 number) {
    switch (number) {
        case 1:
        case 7:  return NYA_TERMINAL_KEY_HOME;
        case 2:  return NYA_TERMINAL_KEY_INSERT;
        case 3:  return NYA_TERMINAL_KEY_DELETE;
        case 4:
        case 8:  return NYA_TERMINAL_KEY_END;
        case 5:  return NYA_TERMINAL_KEY_PAGE_UP;
        case 6:  return NYA_TERMINAL_KEY_PAGE_DOWN;
        case 11: return NYA_TERMINAL_KEY_F1;
        case 12: return NYA_TERMINAL_KEY_F2;
        case 13: return NYA_TERMINAL_KEY_F3;
        case 14: return NYA_TERMINAL_KEY_F4;
        case 15: return NYA_TERMINAL_KEY_F5;
        case 17: return NYA_TERMINAL_KEY_F6;
        case 18: return NYA_TERMINAL_KEY_F7;
        case 19: return NYA_TERMINAL_KEY_F8;
        case 20: return NYA_TERMINAL_KEY_F9;
        case 21: return NYA_TERMINAL_KEY_F10;
        case 23: return NYA_TERMINAL_KEY_F11;
        case 24: return NYA_TERMINAL_KEY_F12;

        // 9, 10, 16 and 22 are gaps in the xterm table and 25 up are shifted function keys nothing
        // here binds. Rejected rather than guessed at.
        default: return NYA_TERMINAL_KEY_NONE;
    }
}

/** What a CSI or SS3 final byte means when it is one of the letters. NONE when it is not. */
NYA_INTERNAL NYA_TerminalKey _nya_terminal_key_of_letter(u8 final) {
    switch (final) {
        case 'A': return NYA_TERMINAL_KEY_UP;
        case 'B': return NYA_TERMINAL_KEY_DOWN;
        case 'C': return NYA_TERMINAL_KEY_RIGHT;
        case 'D': return NYA_TERMINAL_KEY_LEFT;
        case 'H': return NYA_TERMINAL_KEY_HOME;
        case 'F': return NYA_TERMINAL_KEY_END;
        case 'P': return NYA_TERMINAL_KEY_F1;
        case 'Q': return NYA_TERMINAL_KEY_F2;
        case 'R': return NYA_TERMINAL_KEY_F3;
        case 'S': return NYA_TERMINAL_KEY_F4;
        default:  return NYA_TERMINAL_KEY_NONE;
    }
}

/** The SGR mouse report `ESC[<b;x;yM`, already split into its three numbers and its final byte. */
NYA_INTERNAL _NYA_TerminalDecode _nya_terminal_decode_mouse(const u32* parameters, u32 count, u8 final, OUT NYA_TerminalInput* out) {
    if (count < 3) return _NYA_TERMINAL_DECODE_SKIP;

    u32 code = parameters[0];

    // the terminal counts from one and everything above counts from zero.
    out->column    = (u16)(parameters[1] > 0 ? parameters[1] - 1U : 0U);
    out->row       = (u16)(parameters[2] > 0 ? parameters[2] - 1U : 0U);
    out->modifiers = (u16)(((code & 4U) != 0 ? NYA_TERMINAL_MODIFIER_SHIFT : 0U) | ((code & 8U) != 0 ? NYA_TERMINAL_MODIFIER_ALT : 0U)
                           | ((code & 16U) != 0 ? NYA_TERMINAL_MODIFIER_CTRL : 0U));

    // bit 6 is the wheel, bit 5 is motion, and the low two bits are the button otherwise.
    if ((code & 64U) != 0) {
        out->kind  = NYA_TERMINAL_INPUT_MOUSE_WHEEL;
        out->wheel = (code & 3U) == 0 ? (s8)1 : (s8)-1;

        return _NYA_TERMINAL_DECODE_EMIT;
    }

    if ((code & 32U) != 0) {
        out->kind = NYA_TERMINAL_INPUT_MOUSE_MOVED;
        return _NYA_TERMINAL_DECODE_EMIT;
    }

    // 3 is "some button came up" in the older encodings and has no button of its own here.
    u32 button = code & 3U;
    if (button == 3) return _NYA_TERMINAL_DECODE_SKIP;

    out->kind    = NYA_TERMINAL_INPUT_MOUSE_BUTTON;
    out->button  = (NYA_TerminalMouseButton)(NYA_TERMINAL_MOUSE_BUTTON_LEFT + button);
    out->is_down = final == 'M';

    return _NYA_TERMINAL_DECODE_EMIT;
}

/** Everything after `ESC[`. `size` counts from the `[`. */
NYA_INTERNAL _NYA_TerminalDecode _nya_terminal_decode_csi(const u8* bytes, u64 size, OUT NYA_TerminalInput* out, OUT u64* out_used) {
    u64 at       = 1;
    b8  is_mouse = false;

    if (at < size && (bytes[at] == '<' || bytes[at] == '?' || bytes[at] == '>')) {
        is_mouse = bytes[at] == '<';
        at      += 1;
    }

    u32 parameters[_NYA_TERMINAL_PARAMETERS_MAX] = { 0 };
    u32 count                                    = 0;
    u32 digits                                   = 0;
    b8  overflowed                               = false;

    while (at < size && ((bytes[at] >= '0' && bytes[at] <= '9') || bytes[at] == ';' || bytes[at] == ':')) {
        if (bytes[at] == ';' || bytes[at] == ':') {
            if (count < _NYA_TERMINAL_PARAMETERS_MAX) count += 1;
            digits = 0;
        } else {
            digits += 1;

            // a number longer than any real parameter is a terminal misbehaving, and a u32 that wraps
            // would turn it into a plausible one. Bounded here, at the boundary, where the bytes arrive.
            if (digits > _NYA_TERMINAL_PARAMETER_DIGITS_MAX) overflowed = true;

            if (!overflowed && count < _NYA_TERMINAL_PARAMETERS_MAX) {
                parameters[count] = (parameters[count] * 10U) + (u32)(bytes[at] - '0');
            }
        }

        at += 1;
    }

    if (at >= size) return _NYA_TERMINAL_DECODE_INCOMPLETE;

    u8 final = bytes[at];

    // anything outside the final byte range is not a CSI sequence at all; consumed so a terminal
    // sending noise cannot wedge the parser on the same byte forever.
    if (final < 0x40U || final > 0x7EU) {
        *out_used = at + 1;
        return _NYA_TERMINAL_DECODE_SKIP;
    }

    *out_used = at + 1;

    if (count < _NYA_TERMINAL_PARAMETERS_MAX) count += 1;
    if (overflowed) return _NYA_TERMINAL_DECODE_SKIP;

    if (is_mouse) return _nya_terminal_decode_mouse(parameters, count, final, out);

    out->kind = NYA_TERMINAL_INPUT_KEY;

    if (final == '~') {
        out->key       = _nya_terminal_key_of_tilde(parameters[0]);
        out->modifiers = count > 1 ? _nya_terminal_modifiers_of(parameters[1]) : NYA_TERMINAL_MODIFIER_NONE;

        return out->key == NYA_TERMINAL_KEY_NONE ? _NYA_TERMINAL_DECODE_SKIP : _NYA_TERMINAL_DECODE_EMIT;
    }

    // shift-tab, which is the one key with a sequence of its own rather than a modifier parameter.
    if (final == 'Z') {
        out->key       = NYA_TERMINAL_KEY_TAB;
        out->modifiers = NYA_TERMINAL_MODIFIER_SHIFT;

        return _NYA_TERMINAL_DECODE_EMIT;
    }

    out->key = _nya_terminal_key_of_letter(final);
    if (out->key == NYA_TERMINAL_KEY_NONE) return _NYA_TERMINAL_DECODE_SKIP;

    // `ESC[1;5C` is ctrl+right: the first parameter is always 1 and the second carries the modifiers.
    out->modifiers = count > 1 ? _nya_terminal_modifiers_of(parameters[1]) : NYA_TERMINAL_MODIFIER_NONE;

    return _NYA_TERMINAL_DECODE_EMIT;
}

/** One thing, from the front of `bytes`. */
NYA_INTERNAL _NYA_TerminalDecode _nya_terminal_decode_one(const u8* bytes, u64 size, b8 is_final, OUT NYA_TerminalInput* out, OUT u64* out_used) {
    nya_assert(bytes != nullptr && out != nullptr && out_used != nullptr);
    nya_assert(size > 0, "the decoder is only ever handed bytes it has");

    *out_used = 1;
    *out      = (NYA_TerminalInput){ .kind = NYA_TERMINAL_INPUT_KEY };

    u8 lead = bytes[0];

    if (lead == 0x1BU) {
        if (size == 1) {
            // the one genuinely ambiguous byte in the protocol: alone it is the escape key, and
            // followed by anything it is the start of a sequence. Only "nothing more is coming"
            // tells them apart, which is why is_final exists.
            if (!is_final) return _NYA_TERMINAL_DECODE_INCOMPLETE;

            out->key = NYA_TERMINAL_KEY_ESCAPE;
            return _NYA_TERMINAL_DECODE_EMIT;
        }

        if (bytes[1] == '[') {
            u64                 used   = 0;
            _NYA_TerminalDecode result = _nya_terminal_decode_csi(&bytes[1], size - 1, out, &used);

            if (result == _NYA_TERMINAL_DECODE_INCOMPLETE) {
                // a sequence that cannot fit the residue is never going to complete, so it is thrown
                // away rather than held forever against a terminal that stopped mid-escape.
                if (size >= NYA_TERMINAL_RESIDUE_MAX) {
                    *out_used = size;
                    return _NYA_TERMINAL_DECODE_SKIP;
                }

                return _NYA_TERMINAL_DECODE_INCOMPLETE;
            }

            *out_used = used + 1;
            return result;
        }

        if (bytes[1] == 'O') {
            if (size < 3) return _NYA_TERMINAL_DECODE_INCOMPLETE;

            *out_used = 3;
            out->key  = _nya_terminal_key_of_letter(bytes[2]);

            return out->key == NYA_TERMINAL_KEY_NONE ? _NYA_TERMINAL_DECODE_SKIP : _NYA_TERMINAL_DECODE_EMIT;
        }

        // alt+something: the terminal sends the escape and then the key exactly as it would alone.
        u64                 used   = 0;
        _NYA_TerminalDecode result = _nya_terminal_decode_one(&bytes[1], size - 1, is_final, out, &used);

        if (result == _NYA_TERMINAL_DECODE_INCOMPLETE) return result;

        out->modifiers |= NYA_TERMINAL_MODIFIER_ALT;
        *out_used       = used + 1;

        return result;
    }

    switch (lead) {
        case '\r':
        case '\n':   out->key = NYA_TERMINAL_KEY_ENTER; return _NYA_TERMINAL_DECODE_EMIT;
        case '\t':   out->key = NYA_TERMINAL_KEY_TAB; return _NYA_TERMINAL_DECODE_EMIT;
        case 0x08U:
        case 0x7FU:  out->key = NYA_TERMINAL_KEY_BACKSPACE; return _NYA_TERMINAL_DECODE_EMIT;
        default:     break;
    }

    if (lead < 0x20U) {
        // 0x01..0x1A are ctrl+a..ctrl+z, and 0x00 is ctrl+space. The three with names of their own
        // are already gone above, so anything left here really is a control chord.
        out->modifiers = NYA_TERMINAL_MODIFIER_CTRL;
        out->codepoint = lead == 0 ? (u32)' ' : (u32)(lead + 0x60U);

        return _NYA_TERMINAL_DECODE_EMIT;
    }

    u32 codepoint = 0;
    u32 used      = nya_terminal_utf8_decode(bytes, size, &codepoint);

    if (used == 0) {
        if (!is_final) return _NYA_TERMINAL_DECODE_INCOMPLETE;

        // a truncated character at the end of the stream is dropped, not guessed at.
        *out_used = size;
        return _NYA_TERMINAL_DECODE_SKIP;
    }

    *out_used      = used;
    out->codepoint = codepoint;

    return _NYA_TERMINAL_DECODE_EMIT;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INPUT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

u32 nya_terminal_input_decode(const u8* bytes, u64 size, b8 is_final, OUT NYA_TerminalInput* out, u32 capacity, OUT u64* out_consumed) {
    nya_assert(out != nullptr && out_consumed != nullptr);
    nya_assert(bytes != nullptr || size == 0);

    u64 at    = 0;
    u32 count = 0;

    while (at < size && count < capacity) {
        NYA_TerminalInput input = { 0 };
        u64               used  = 0;

        _NYA_TerminalDecode result = _nya_terminal_decode_one(&bytes[at], size - at, is_final, &input, &used);
        if (result == _NYA_TERMINAL_DECODE_INCOMPLETE) break;

        nya_assert(used > 0 && used <= size - at, "the decoder consumed " FMTu64 " of " FMTu64 " bytes", used, size - at);
        at += used;

        if (result == _NYA_TERMINAL_DECODE_EMIT) out[count++] = input;
    }

    *out_consumed = at;

    nya_assert(*out_consumed <= size);
    return count;
}

u32 nya_terminal_poll(OUT NYA_TerminalInput* out, u32 capacity) {
    nya_assert(out != nullptr);
    nya_assert(capacity > 0, "polling for no inputs is a caller mistake, not an empty read");

    if (!_nya_terminal.open) return 0;

    u32 count = 0;

    /*
     * The resize goes first, so a program that stops at the first input it understands still gets
     * the new size before it draws into the old one. This is the bug the GPU path already had: a
     * resize seen after the frame it applies to paints one frame at the wrong size.
     */
    if (_nya_terminal_resized_take()) {
        if (_nya_terminal_size_refresh()) {
            nya_terminal_clear(0);
            nya_terminal_invalidate();
        }

        out[count++] = (NYA_TerminalInput){
            .kind   = NYA_TERMINAL_INPUT_RESIZE,
            .column = _nya_terminal.columns,
            .row    = _nya_terminal.rows,
        };

        if (count == capacity) return count;
    }

    u8  buffer[NYA_TERMINAL_RESIDUE_MAX + NYA_TERMINAL_READ_MAX];
    u64 held = _nya_terminal.residue_size;

    nya_assert(held <= NYA_TERMINAL_RESIDUE_MAX, "the residue holds " FMTu64 " bytes and its capacity is %d", held, NYA_TERMINAL_RESIDUE_MAX);
    if (held > 0) nya_memcpy(buffer, _nya_terminal.residue, held);

    u64 read  = _nya_terminal_receive(&buffer[held], NYA_TERMINAL_READ_MAX);
    u64 total = held + read;

    if (total == 0) return count;

    // a read that did not fill the buffer is everything the terminal had, so a trailing escape
    // really is the escape key rather than the start of something still on its way.
    b8 is_final = read < NYA_TERMINAL_READ_MAX;

    u64 consumed = 0;
    count       += nya_terminal_input_decode(buffer, total, is_final, &out[count], capacity - count, &consumed);

    u64 left = total - consumed;

    if (left > NYA_TERMINAL_RESIDUE_MAX) {
        // unreachable through the decoder, which never leaves more than one incomplete sequence, and
        // asserted rather than deleted so a change to the decoder cannot smuggle an overflow in.
        nya_log_error("Terminal left " FMTu64 " undecoded bytes, which does not fit the residue; dropping them.", left);
        left = 0;
    }

    if (left > 0) nya_memcpy(_nya_terminal.residue, &buffer[consumed], left);

    _nya_terminal.residue_size = left;
    _nya_terminal.residue_live = (u32)left;

    return count;
}

NYA_ConstCString nya_terminal_key_name(NYA_TerminalKey key) {
    NYA_ConstCString names[NYA_TERMINAL_KEY_COUNT] = {
        [NYA_TERMINAL_KEY_NONE]      = "none",
        [NYA_TERMINAL_KEY_ESCAPE]    = "escape",
        [NYA_TERMINAL_KEY_ENTER]     = "enter",
        [NYA_TERMINAL_KEY_TAB]       = "tab",
        [NYA_TERMINAL_KEY_BACKSPACE] = "backspace",
        [NYA_TERMINAL_KEY_DELETE]    = "delete",
        [NYA_TERMINAL_KEY_INSERT]    = "insert",
        [NYA_TERMINAL_KEY_UP]        = "up",
        [NYA_TERMINAL_KEY_DOWN]      = "down",
        [NYA_TERMINAL_KEY_LEFT]      = "left",
        [NYA_TERMINAL_KEY_RIGHT]     = "right",
        [NYA_TERMINAL_KEY_HOME]      = "home",
        [NYA_TERMINAL_KEY_END]       = "end",
        [NYA_TERMINAL_KEY_PAGE_UP]   = "page up",
        [NYA_TERMINAL_KEY_PAGE_DOWN] = "page down",
        [NYA_TERMINAL_KEY_F1]        = "f1",
        [NYA_TERMINAL_KEY_F2]        = "f2",
        [NYA_TERMINAL_KEY_F3]        = "f3",
        [NYA_TERMINAL_KEY_F4]        = "f4",
        [NYA_TERMINAL_KEY_F5]        = "f5",
        [NYA_TERMINAL_KEY_F6]        = "f6",
        [NYA_TERMINAL_KEY_F7]        = "f7",
        [NYA_TERMINAL_KEY_F8]        = "f8",
        [NYA_TERMINAL_KEY_F9]        = "f9",
        [NYA_TERMINAL_KEY_F10]       = "f10",
        [NYA_TERMINAL_KEY_F11]       = "f11",
        [NYA_TERMINAL_KEY_F12]       = "f12",
    };

    if ((u32)key >= (u32)NYA_TERMINAL_KEY_COUNT) return "none";

    nya_assert(names[key] != nullptr, "NYA_TerminalKey %d has no name", (s32)key);
    return names[key];
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * IMAGES
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** RFC 4648 base64, which is what the kitty protocol carries its payload in. */
NYA_INTERNAL const char _NYA_TERMINAL_BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static_assert(sizeof(_NYA_TERMINAL_BASE64) == 65, "base64 has 64 digits, and the terminator makes 65");

/**
 * Encodes three bytes at a time straight into the write buffer.
 *
 * Not nya_base64_encode: that writes into an NYA_String, which for a 1080p frame means eleven
 * megabytes encoded and held before a single byte reaches the terminal, where the protocol wants it
 * in 4 KiB chunks anyway.
 * */
NYA_INTERNAL void _nya_terminal_push_base64(const u8* data, u64 size) {
    u8  chunk[4];
    u64 at = 0;

    while (at + 3 <= size) {
        u32 triple = ((u32)data[at] << 16U) | ((u32)data[at + 1] << 8U) | (u32)data[at + 2];

        chunk[0] = (u8)_NYA_TERMINAL_BASE64[(triple >> 18U) & 0x3FU];
        chunk[1] = (u8)_NYA_TERMINAL_BASE64[(triple >> 12U) & 0x3FU];
        chunk[2] = (u8)_NYA_TERMINAL_BASE64[(triple >> 6U) & 0x3FU];
        chunk[3] = (u8)_NYA_TERMINAL_BASE64[triple & 0x3FU];

        _nya_terminal_push(chunk, 4);
        at += 3;
    }

    u64 left = size - at;
    if (left == 0) return;

    u32 triple = (u32)data[at] << 16U;
    if (left == 2) triple |= (u32)data[at + 1] << 8U;

    chunk[0] = (u8)_NYA_TERMINAL_BASE64[(triple >> 18U) & 0x3FU];
    chunk[1] = (u8)_NYA_TERMINAL_BASE64[(triple >> 12U) & 0x3FU];
    chunk[2] = left == 2 ? (u8)_NYA_TERMINAL_BASE64[(triple >> 6U) & 0x3FU] : (u8)'=';
    chunk[3] = '=';

    _nya_terminal_push(chunk, 4);
}

b8 nya_terminal_image_draw(u16 column, u16 row, const u8* rgba, u32 width, u32 height) {
    nya_assert(rgba != nullptr);
    nya_assert(width > 0 && height > 0, "an image with no pixels is a caller mistake");

    if (!_nya_terminal.open || !_nya_terminal.capabilities.kitty_images) return false;

    u64 pixels = (u64)width * (u64)height;
    if (pixels > NYA_TERMINAL_IMAGE_PIXELS_MAX) {
        nya_log_warn("Terminal image is " FMTu64 " pixels and the limit is " FMTu64 "; not drawn.", pixels, (u64)NYA_TERMINAL_IMAGE_PIXELS_MAX);
        return false;
    }

    u32 id = ++_nya_terminal.image_next_id;
    _nya_terminal.image_count += 1;

    // the cursor decides where the image lands, and C=1 below stops the terminal moving it after.
    _nya_terminal_push_cstring("\x1b[");
    _nya_terminal_push_number((u32)row + 1U);
    _nya_terminal_push_cstring(";");
    _nya_terminal_push_number((u32)column + 1U);
    _nya_terminal_push_cstring("H");

    u64 size = pixels * 4U;

    /*
     * Chunked because the protocol requires it: 4096 base64 characters per escape, which is 3072
     * source bytes. The first chunk carries the keys and every chunk but the last says m=1.
     */
    const u64 source_per_chunk = ((u64)NYA_TERMINAL_IMAGE_CHUNK_BYTES / 4U) * 3U;

    u64 at    = 0;
    b8  first = true;

    while (at < size) {
        u64 take = nya_min(source_per_chunk, size - at);
        b8  last = at + take >= size;

        _nya_terminal_push_cstring("\x1b_G");

        if (first) {
            // f=32 is RGBA, a=T transmits and displays in one go, C=1 leaves the cursor alone.
            _nya_terminal_push_cstring("a=T,f=32,C=1,i=");
            _nya_terminal_push_number(id);
            _nya_terminal_push_cstring(",s=");
            _nya_terminal_push_number(width);
            _nya_terminal_push_cstring(",v=");
            _nya_terminal_push_number(height);
            _nya_terminal_push_cstring(",");

            first = false;
        }

        _nya_terminal_push_cstring(last ? "m=0;" : "m=1;");
        _nya_terminal_push_base64(&rgba[at], take);
        _nya_terminal_push_cstring("\x1b\\");

        at += take;
    }

    return true;
}

void nya_terminal_image_clear(void) {
    if (!_nya_terminal.open || !_nya_terminal.capabilities.kitty_images || _nya_terminal.image_count == 0) return;

    // a=d,d=A deletes every placement this program made, which is exactly the set this module placed.
    _nya_terminal_push_cstring("\x1b_Ga=d,d=A;\x1b\\");
    _nya_terminal_flush();

    _nya_terminal.image_count = 0;

    // the cells the images covered were never in the grid, so the next present has to write them again.
    nya_terminal_invalidate();
}
