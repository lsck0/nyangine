/**
 * The terminal input decoder, fed whatever a terminal — or something pretending to be one — writes.
 *
 * `nya_terminal_input_decode` turns a byte stream into keys, mouse reports and resizes: escape
 * sequences, CSI parameters, SS3 function keys, SGR mouse packets and UTF-8 runs, all parsed from
 * bytes the program does not control. A paste is arbitrary bytes; a hostile or buggy terminal can
 * emit a half-finished or overlong sequence; so this is a parser over untrusted input like any other.
 *
 * The decoder's own docstring names it as what the fuzzer drives. The oracle is that it never reads or
 * writes out of bounds, never consumes more than it was given, never reports more inputs than the
 * capacity allows, and only ever tags an input with a kind the enum defines. Leftover bytes are an
 * incomplete sequence and the expected answer, not a finding.
 *
 * The same bytes are decoded three ways — mid-stream, final, and into a single slot — because
 * `is_final` is the only thing that tells a lone ESC from the start of an arrow key and a small output
 * window is where an off-by-one would write past the end. All three have to stay in bounds.
 **/

// clang-format off
// the engine defines the feature test macros this build needs, so it comes before any libc header.
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"
// clang-format on

#define FUZZ_TARGET "terminal"

/** Checks one batch of decoded inputs against everything the decoder promises about them. */
static void fuzz_check_decode(const NYA_TerminalInput* out, u32 count, u32 capacity, u64 consumed, u64 size) {
    nya_assert(count <= capacity, "the decoder filled %u of %u slots", count, capacity);
    nya_assert(consumed <= size, "the decoder consumed %llu of %llu bytes", (unsigned long long)consumed, (unsigned long long)size);

    for (u32 i = 0; i < count; i++) {
        nya_assert(out[i].kind < NYA_TERMINAL_INPUT_KIND_COUNT, "a decoded input carried a kind the enum does not define");
        nya_assert(out[i].key < NYA_TERMINAL_KEY_COUNT, "a decoded key was outside the table");
        nya_assert(out[i].button < NYA_TERMINAL_MOUSE_BUTTON_COUNT, "a decoded mouse report named a button that does not exist");
        nya_assert(out[i].wheel >= -1 && out[i].wheel <= 1, "a wheel step other than up, down or none");
    }
}

static void fuzz_once(const u8* data, u64 size) {
    NYA_TerminalInput out[NYA_TERMINAL_INPUT_MAX] = { 0 };

    // mid-stream: a trailing partial sequence is kept for the next read rather than decoded now.
    u64 consumed = 0;
    u32 count    = nya_terminal_input_decode(data, size, false, out, nya_carray_length(out), &consumed);
    fuzz_check_decode(out, count, nya_carray_length(out), consumed, size);

    // final: no more bytes are coming, so a lone ESC is a key and every complete sequence is flushed. The final read consumes at least what the mid-stream one did, since nothing is held back for a next read.
    u64 final_consumed = 0;
    u32 final_count    = nya_terminal_input_decode(data, size, true, out, nya_carray_length(out), &final_consumed);
    fuzz_check_decode(out, final_count, nya_carray_length(out), final_consumed, size);

    // a smaller output window must never make the decoder write past it: the same bytes into one slot.
    NYA_TerminalInput one          = { 0 };
    u64               one_consumed = 0;
    u32               one_count    = nya_terminal_input_decode(data, size, true, &one, 1, &one_consumed);
    fuzz_check_decode(&one, one_count, 1, one_consumed, size);
}

#include "tests/fuzz/fuzz.h"
