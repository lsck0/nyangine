/**
 * @file http_next_line.c
 *
 * A bounded proof that the HTTP head's line splitter never reads past the buffer it was handed, and
 * never reports a line that reaches outside it, whatever bytes a peer puts on the wire.
 *
 * The function under proof is `_nya_http_next_line` from src/nyangine/http/http_message.c: the one
 * place the request head is cut into lines. `nya_http_request_parse` calls it for the request line and
 * every header, and the chunked-body decoder calls it for every chunk-size line and trailer, so if a
 * hostile stream could drive its cursor off the end, the whole request parser could be. It reads fully
 * untrusted input — the first bytes of every connection the server accepts.
 *
 * The body below is a verbatim copy of the engine's function and its result enum, lifted here rather
 * than #included because the real translation unit pulls in the whole HTTP stack and a twenty-kilobyte
 * request struct, none of which CBMC needs to reason about the two-byte CRLF scan. It is kept
 * byte-for-byte in step with the source.
 *
 * What CBMC checks here:
 *   - --bounds-check / --pointer-check: every `data[index]` and `data[index + 1]` read is inside the
 *     allocated buffer, for every input the harness admits. This is the out-of-bounds-read proof.
 *   - the functional invariant: when a line is found the cursor lands no further than `size` and only
 *     moves forward, and the reported line slice — `[start, start + *out_length)` — lies wholly inside
 *     the buffer. A caller downstream is never handed an offset or a slice past the end.
 *
 * Run by `./build verify`. Small bounds keep it to a fraction of a second.
 */

#include <stdint.h>
#include <stdlib.h>

typedef _Bool    b8;
typedef uint8_t  u8;
typedef uint64_t u64;

#define nullptr ((void*)0)

/* The engine tags out-parameters with an OUT marker for the reader; it is not semantically meaningful. */
#define OUT

/* mirrored from src/nyangine/http/http_message.c */

typedef enum {
    _NYA_HTTP_LINE_INCOMPLETE = 0,
    _NYA_HTTP_LINE_FOUND,
    _NYA_HTTP_LINE_TOO_LONG,
} _NYA_HttpLine;

static _NYA_HttpLine _nya_http_next_line(const u8* data, u64 size, u64 line_max, u64* cursor, const char** out_line, u64* out_length) {
    u64 start = *cursor;

    if (start >= size) return _NYA_HTTP_LINE_INCOMPLETE;

    u64 limit = size - start < line_max ? size : start + line_max;

    for (u64 index = start; index + 1 < limit; index++) {
        if (data[index] != '\r' || data[index + 1] != '\n') continue;

        *out_line   = (const char*)(data + start);
        *out_length = index - start;
        *cursor     = index + 2;

        return _NYA_HTTP_LINE_FOUND;
    }

    if (size - start >= line_max) return _NYA_HTTP_LINE_TOO_LONG;

    return _NYA_HTTP_LINE_INCOMPLETE;
}

/* the harness */

/* Small enough for --unwinding-assertions to prove the scan loop terminates (it steps at most once per
 * byte), big enough to admit an empty buffer, a bare CRLF, and a run of bytes with the CRLF anywhere. */
#define NEXT_LINE_MAX_SIZE 8u

extern u64 nondet_size(void);
extern u64 nondet_start(void);
extern u64 nondet_line_max(void);

int main(void) {
    u64 size = nondet_size();
    __CPROVER_assume(size <= NEXT_LINE_MAX_SIZE);

    /* An object of exactly `size` nondeterministic bytes: a read past it is a real out-of-bounds
     * access CBMC will report, which is what makes the bounds proof meaningful. */
    u8* data = malloc(size);
    __CPROVER_assume(size == 0 || data != nullptr);

    /* The cursor may sit anywhere from the start to one past the last byte, as it does mid-parse when
     * one line has been consumed and the next is sought. */
    u64 start = nondet_start();
    __CPROVER_assume(start <= size);

    /* The real callers pass a fixed positive cap (the max head, chunk-line or trailer size). Bounded
     * here so the `start + line_max` limit is reachable alongside the `size` limit. */
    u64 line_max = nondet_line_max();
    __CPROVER_assume(line_max >= 1 && line_max <= NEXT_LINE_MAX_SIZE);

    u64         cursor      = start;
    const char* out_line    = nullptr;
    u64         out_length  = 0;

    _NYA_HttpLine result = _nya_http_next_line(data, size, line_max, &cursor, &out_line, &out_length);

    if (result == _NYA_HTTP_LINE_FOUND) {
        /* The cursor advanced over the CRLF but never past the buffer: a downstream read starting from
         * it stays in bounds. */
        __CPROVER_assert(cursor > start, "a found line advances the cursor");
        __CPROVER_assert(cursor <= size, "the cursor never leaves the buffer");

        /* The reported line lies wholly inside the buffer: its first byte is at `start` and its last is
         * before the CRLF, so start + length is strictly inside `size`. */
        __CPROVER_assert(start + out_length < size, "the reported line stays inside the buffer");
    } else {
        /* On any non-found outcome the cursor is left untouched, so it is still in bounds. */
        __CPROVER_assert(cursor == start, "a line that was not found leaves the cursor where it was");
    }

    return 0;
}
