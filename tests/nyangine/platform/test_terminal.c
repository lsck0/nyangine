/**
 * The terminal's two pure halves, which are the two that can be wrong without anyone noticing: the
 * escape sequence decoder and the UTF-8 it walks. Both run without a terminal, which is why
 * nya_terminal_input_decode exists as a function over bytes rather than only inside the poll.
 *
 * Everything else in the module needs a real terminal and a real tty, and is verified by running
 * `./build run example tui_dashboard`.
 **/

#include "nyangine/nyangine.h"

#include "nyangine/nyangine.c"

/** Decodes one buffer to completion, as a poll with nothing more coming would. */
static u32 decode(NYA_ConstCString bytes, u64 size, NYA_TerminalInput* out, u32 capacity) {
  u64 consumed = 0;
  u32 count    = nya_terminal_input_decode((const u8*)bytes, size, true, out, capacity, &consumed);

  nya_assert(consumed == size, "a final decode left " FMTu64 " of " FMTu64 " bytes", size - consumed, size);

  return count;
}

/** The same for a literal, whose length is its strlen. */
static u32 decode_cstring(NYA_ConstCString bytes, NYA_TerminalInput* out, u32 capacity) {
  return decode(bytes, strlen(bytes), out, capacity);
}

s32 main(void) {
  NYA_TerminalInput input[NYA_TERMINAL_INPUT_MAX] = { 0 };

  // TEST: plain characters are keys carrying their code point and no named key
  {
    u32 count = decode_cstring("abc", input, nya_carray_length(input));
    nya_assert(count == 3);

    for (u32 i = 0; i < 3; i++) {
      nya_assert(input[i].kind == NYA_TERMINAL_INPUT_KEY);
      nya_assert(input[i].key == NYA_TERMINAL_KEY_NONE);
      nya_assert(input[i].modifiers == NYA_TERMINAL_MODIFIER_NONE);
    }

    nya_assert(input[0].codepoint == 'a' && input[1].codepoint == 'b' && input[2].codepoint == 'c');
  }

  // TEST: the keys that have names of their own
  {
    u32 count = decode("\r\t\x7f", 3, input, nya_carray_length(input));
    nya_assert(count == 3);

    nya_assert(input[0].key == NYA_TERMINAL_KEY_ENTER);
    nya_assert(input[1].key == NYA_TERMINAL_KEY_TAB);
    nya_assert(input[2].key == NYA_TERMINAL_KEY_BACKSPACE);

    // none of them is also a character: a return that typed a '\r' would insert one in a text field.
    for (u32 i = 0; i < 3; i++) nya_assert(input[i].codepoint == 0);
  }

  // TEST: control chords, which are the byte plus 0x60 with ctrl set
  {
    u32 count = decode("\x01\x03", 2, input, nya_carray_length(input));
    nya_assert(count == 2);

    nya_assert(input[0].codepoint == 'a' && input[0].modifiers == NYA_TERMINAL_MODIFIER_CTRL);
    nya_assert(input[1].codepoint == 'c' && input[1].modifiers == NYA_TERMINAL_MODIFIER_CTRL);
  }

  // TEST: a lone escape is the escape key only when nothing more is coming
  {
    u32 count = decode("\x1b", 1, input, nya_carray_length(input));
    nya_assert(count == 1);
    nya_assert(input[0].key == NYA_TERMINAL_KEY_ESCAPE);

    // the same byte, with more still on its way, is the start of a sequence and is held.
    u64 consumed = 0;
    count        = nya_terminal_input_decode((const u8*)"\x1b", 1, false, input, nya_carray_length(input), &consumed);

    nya_assert(count == 0, "a non final escape produced %u inputs", count);
    nya_assert(consumed == 0, "a non final escape consumed " FMTu64 " bytes", consumed);
  }

  // TEST: CSI arrows, with and without a modifier parameter
  {
    u32 count = decode_cstring("\x1b[A\x1b[B\x1b[C\x1b[D", input, nya_carray_length(input));
    nya_assert(count == 4);

    nya_assert(input[0].key == NYA_TERMINAL_KEY_UP);
    nya_assert(input[1].key == NYA_TERMINAL_KEY_DOWN);
    nya_assert(input[2].key == NYA_TERMINAL_KEY_RIGHT);
    nya_assert(input[3].key == NYA_TERMINAL_KEY_LEFT);

    // ESC [ 1 ; 5 C is ctrl+right: the modifier parameter is one past the mask.
    count = decode_cstring("\x1b[1;5C", input, nya_carray_length(input));
    nya_assert(count == 1);
    nya_assert(input[0].key == NYA_TERMINAL_KEY_RIGHT);
    nya_assert(input[0].modifiers == NYA_TERMINAL_MODIFIER_CTRL);

    // 1 + shift(1) + alt(2) + ctrl(4) is 8.
    count = decode_cstring("\x1b[1;8A", input, nya_carray_length(input));
    nya_assert(count == 1);
    nya_assert(input[0].modifiers == (NYA_TERMINAL_MODIFIER_SHIFT | NYA_TERMINAL_MODIFIER_ALT | NYA_TERMINAL_MODIFIER_CTRL));
  }

  // TEST: SS3 function keys, the tilde table, and shift+tab
  {
    u32 count = decode_cstring("\x1bOP\x1bOS", input, nya_carray_length(input));
    nya_assert(count == 2);
    nya_assert(input[0].key == NYA_TERMINAL_KEY_F1 && input[1].key == NYA_TERMINAL_KEY_F4);

    count = decode_cstring("\x1b[3~\x1b[5~\x1b[24~", input, nya_carray_length(input));
    nya_assert(count == 3);
    nya_assert(input[0].key == NYA_TERMINAL_KEY_DELETE);
    nya_assert(input[1].key == NYA_TERMINAL_KEY_PAGE_UP);
    nya_assert(input[2].key == NYA_TERMINAL_KEY_F12);

    count = decode_cstring("\x1b[Z", input, nya_carray_length(input));
    nya_assert(count == 1);
    nya_assert(input[0].key == NYA_TERMINAL_KEY_TAB && input[0].modifiers == NYA_TERMINAL_MODIFIER_SHIFT);
  }

  // TEST: alt is an escape in front of whatever the key would have been alone
  {
    u32 count = decode_cstring("\x1bx", input, nya_carray_length(input));
    nya_assert(count == 1);
    nya_assert(input[0].codepoint == 'x');
    nya_assert(input[0].modifiers == NYA_TERMINAL_MODIFIER_ALT);
  }

  // TEST: SGR mouse reports, which are the only encoding past column 223
  {
    // button 0 pressed at column 33, row 9, one based.
    u32 count = decode_cstring("\x1b[<0;33;9M", input, nya_carray_length(input));
    nya_assert(count == 1);
    nya_assert(input[0].kind == NYA_TERMINAL_INPUT_MOUSE_BUTTON);
    nya_assert(input[0].button == NYA_TERMINAL_MOUSE_BUTTON_LEFT);
    nya_assert(input[0].is_down);
    nya_assert(input[0].column == 32 && input[0].row == 8, "the terminal counts from one and everything above counts from zero");

    // the same with a lowercase final byte is the release.
    count = decode_cstring("\x1b[<0;33;9m", input, nya_carray_length(input));
    nya_assert(count == 1 && !input[0].is_down);

    // bit 5 is motion.
    count = decode_cstring("\x1b[<32;5;5M", input, nya_carray_length(input));
    nya_assert(count == 1 && input[0].kind == NYA_TERMINAL_INPUT_MOUSE_MOVED);

    // bit 6 is the wheel, and the low bits pick the direction.
    count = decode_cstring("\x1b[<64;1;1M\x1b[<65;1;1M", input, nya_carray_length(input));
    nya_assert(count == 2);
    nya_assert(input[0].kind == NYA_TERMINAL_INPUT_MOUSE_WHEEL && input[0].wheel == 1);
    nya_assert(input[1].kind == NYA_TERMINAL_INPUT_MOUSE_WHEEL && input[1].wheel == -1);

    // a right button click with ctrl held: 2 for the button, 16 for the modifier.
    count = decode_cstring("\x1b[<18;2;2M", input, nya_carray_length(input));
    nya_assert(count == 1);
    nya_assert(input[0].button == NYA_TERMINAL_MOUSE_BUTTON_RIGHT);
    nya_assert(input[0].modifiers == NYA_TERMINAL_MODIFIER_CTRL);
  }

  // TEST: a sequence split across two reads decodes once, not twice and not never
  {
    // the first half is held, nothing is consumed, and nothing is reported.
    u64 consumed = 0;
    u32 count    = nya_terminal_input_decode((const u8*)"\x1b[<0;1", 6, false, input, nya_carray_length(input), &consumed);

    nya_assert(count == 0 && consumed == 0, "half a mouse report produced %u inputs", count);

    // joined with the rest, exactly as nya_terminal_poll joins the residue with the next read.
    count = decode_cstring("\x1b[<0;1;1M", input, nya_carray_length(input));
    nya_assert(count == 1 && input[0].kind == NYA_TERMINAL_INPUT_MOUSE_BUTTON);

    // an arrow split between its ESC[ and its final byte behaves the same.
    count = nya_terminal_input_decode((const u8*)"ab\x1b[", 4, false, input, nya_carray_length(input), &consumed);
    nya_assert(count == 2, "the complete keys before an incomplete sequence still decode");
    nya_assert(consumed == 2, "the incomplete sequence is left for the next read");
  }

  // TEST: the decoder never wedges on input a hostile terminal could send
  {
    // a CSI nothing here decodes is consumed and reported as nothing, so the next key still arrives.
    u32 count = decode_cstring("\x1b[?25hq", input, nya_carray_length(input));
    nya_assert(count == 1 && input[0].codepoint == 'q');

    // a parameter longer than any real one is rejected rather than wrapped into a plausible number.
    count = decode_cstring("\x1b[999999999;999999999Aq", input, nya_carray_length(input));
    nya_assert(count == 1 && input[0].codepoint == 'q', "an overlong parameter must be dropped, not wrapped");

    // an escape that never finishes cannot be held forever: past the residue it is thrown away.
    char runaway[NYA_TERMINAL_RESIDUE_MAX + 8];
    nya_memset(runaway, '0', sizeof(runaway));
    runaway[0] = '\x1b';
    runaway[1] = '[';

    u64 consumed = 0;
    count        = nya_terminal_input_decode((const u8*)runaway, sizeof(runaway), false, input, nya_carray_length(input), &consumed);

    nya_assert(count == 0, "a runaway escape reported %u inputs", count);
    nya_assert(consumed > 0, "a runaway escape longer than the residue must be dropped, not held");

    // a zero byte is ctrl+space and not a terminator the loop stops at.
    count = decode("\0z", 2, input, nya_carray_length(input));
    nya_assert(count == 2);
    nya_assert(input[0].codepoint == ' ' && input[0].modifiers == NYA_TERMINAL_MODIFIER_CTRL);
    nya_assert(input[1].codepoint == 'z');
  }

  // TEST: the decoder fills no more than it was given room for
  {
    NYA_TerminalInput two[2] = { 0 };

    u64 consumed = 0;
    u32 count    = nya_terminal_input_decode((const u8*)"abcde", 5, true, two, nya_carray_length(two), &consumed);

    nya_assert(count == 2, "the decoder filled %u of two slots", count);
    nya_assert(consumed == 2, "the bytes it did not decode are left for the next call, not dropped");
  }

  // TEST: UTF-8, including what a terminal must not be able to do with it
  {
    // 'ä' is two bytes, '€' three, and an emoji four.
    u32 count = decode_cstring("ä€🐱", input, nya_carray_length(input));
    nya_assert(count == 3);
    nya_assert(input[0].codepoint == 0xE4);
    nya_assert(input[1].codepoint == 0x20AC);
    nya_assert(input[2].codepoint == 0x1F431);

    u32 codepoint = 0;
    nya_assert(nya_terminal_utf8_decode((const u8*)"A", 1, &codepoint) == 1 && codepoint == 'A');

    // half a character is not a character yet, and says so rather than guessing.
    nya_assert(nya_terminal_utf8_decode((const u8*)"\xE2\x82", 2, &codepoint) == 0);

    // a continuation byte with no lead is one byte of the replacement character, so a stream of them
    // cannot make a caller's loop scan forward.
    nya_assert(nya_terminal_utf8_decode((const u8*)"\x80", 1, &codepoint) == 1 && codepoint == 0xFFFD);
    nya_assert(nya_terminal_utf8_decode((const u8*)"\xC3\x28", 2, &codepoint) == 1 && codepoint == 0xFFFD);
  }

  // TEST: ink packs and clamps
  {
    nya_assert(nya_terminal_ink(0.0F, 0.0F, 0.0F) == 0x000000);
    nya_assert(nya_terminal_ink(1.0F, 1.0F, 1.0F) == 0xFFFFFF);
    nya_assert(nya_terminal_ink(1.0F, 0.0F, 0.0F) == 0xFF0000);
    nya_assert(nya_terminal_ink(0.0F, 0.0F, 1.0F) == 0x0000FF);

    // out of range is clamped rather than wrapped into another colour entirely.
    nya_assert(nya_terminal_ink(2.0F, -1.0F, 0.5F) == 0xFF0080);
  }

  // TEST: every key has a name, and a value that is not one gets a name too
  {
    for (u32 key = 0; key < NYA_TERMINAL_KEY_COUNT; key++) {
      NYA_ConstCString name = nya_terminal_key_name((NYA_TerminalKey)key);

      nya_assert(name != nullptr && name[0] != '\0', "NYA_TerminalKey %u has no name", key);
    }

    nya_assert(nya_string_equals(nya_terminal_key_name(NYA_TERMINAL_KEY_ESCAPE), "escape"));
    nya_assert(nya_string_equals(nya_terminal_key_name(NYA_TERMINAL_KEY_F10), "f10"));
    nya_assert(nya_string_equals(nya_terminal_key_name((NYA_TerminalKey)NYA_TERMINAL_KEY_COUNT), "none"));
  }

  // TEST: the device answers without a terminal instead of crashing
  {
    // a test harness has no tty, so nothing below is open and every call must still be safe. This is
    // the path a program piped into a file takes.
    nya_assert(!nya_terminal_is_open());
    nya_assert(nya_terminal_columns() == 0 && nya_terminal_rows() == 0);
    nya_assert(nya_terminal_capabilities().color_depth == NYA_TERMINAL_COLOR_NONE);

    nya_terminal_clear(0xFFFFFF);
    nya_terminal_cell_set(3, 4, (NYA_TerminalCell){ .codepoint = 'x' });
    nya_assert(nya_terminal_cell_get(3, 4).codepoint == 0);

    nya_terminal_present();
    nya_terminal_invalidate();
    nya_terminal_image_clear();

    const u8 pixel[4] = { 0xFF, 0x00, 0x00, 0xFF };
    nya_assert(!nya_terminal_image_draw(0, 0, pixel, 1, 1), "an image on a closed terminal must draw nothing");

    // close without an open is the idempotent half of the pair, and is what a crash handler calls.
    nya_terminal_close();
  }

  (void)printf("TEST: terminal decoder, UTF-8 and the closed device\n");

  return EXIT_SUCCESS;
}
