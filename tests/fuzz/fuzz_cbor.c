/**
 * The CBOR reader, fed whatever an authenticator hands the server.
 *
 * WebAuthn's attestation object and its COSE keys are CBOR, so these bytes come from across the network
 * and are an attacker's to choose. The reader is written to be defensive — every read bounds checked
 * before a byte is touched, indefinite-length items refused, skip bounded to NYA_CBOR_MAX_DEPTH so a
 * nested encoding cannot run the stack out — and this is what proves it: arbitrary bytes must fail the
 * read, never walk off the buffer and never recurse without bound.
 *
 * The oracle is the one promise the reader makes about its cursor: no read ever moves `offset` past
 * `size`, and every string a read hands back points inside the buffer it was given. A byte string that
 * says it is longer than the buffer is a refused read and the expected answer, not a finding.
 *
 * Three walks over the same bytes: a `skip` that steps over each top-level value the way the parser
 * does past an entry it does not care about, the map-and-label walk an attestation object drives, and
 * each typed read on a fresh reader so a crafted header for one is exercised as itself.
 **/

// clang-format off
// the engine defines the feature test macros this build needs, so it comes before any libc header.
#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"
// clang-format on

#define FUZZ_TARGET "cbor"

/** The cursor never passes the end of the buffer, whatever the last read did or refused to do. */
static void fuzz_check_cursor(const NYA_CborReader* reader) {
    nya_assert(reader->offset <= reader->size, "a read moved the cursor past the end of the buffer");
}

/** A string a read handed back lies wholly inside the buffer it was pointed at, and is not a wild pointer. */
static void fuzz_check_span(const NYA_CborReader* reader, const u8* bytes, u64 span) {
    if (span == 0) return;
    nya_assert(bytes >= reader->data, "a read returned a pointer before the start of the buffer");
    nya_assert(bytes <= reader->data + reader->size, "a read returned a pointer past the end of the buffer");
    nya_assert(span <= reader->size, "a read returned a span longer than the whole buffer");
    nya_assert((u64)(bytes - reader->data) + span <= reader->size, "a read returned a span that reaches past the buffer");
}

static void fuzz_once(const u8* data, u64 size) {
    // a skip over each top-level value: what walking past an entry the parser does not interpret does. Each
    // item consumes at least one byte, so the buffer bounds the loop even before skip refuses a bad value.
    {
        NYA_CborReader reader = nya_cbor_reader(data, size);

        for (u64 step = 0; step <= size; step++) {
            if (reader.offset >= reader.size) break;
            if (!nya_cbor_skip(&reader)) break;
            fuzz_check_cursor(&reader);
        }
    }

    // the attestation-object walk: a definite-length map, then a label and a skipped value per entry. The
    // count is the authenticator's to claim, so it is capped by the buffer and every read is checked.
    {
        NYA_CborReader reader = nya_cbor_reader(data, size);

        u64 entries = 0;
        if (nya_cbor_read_map(&reader, &entries)) {
            fuzz_check_cursor(&reader);

            for (u64 index = 0; index < entries && index <= size; index++) {
                s64 label = 0;
                if (!nya_cbor_read_int(&reader, &label)) break;
                fuzz_check_cursor(&reader);

                if (!nya_cbor_skip(&reader)) break;
                fuzz_check_cursor(&reader);
            }
        }
    }

    // each typed read on a fresh reader, so a header crafted for one kind is exercised as that kind and the
    // pointer a string read hands back is checked to land inside the buffer.
    {
        NYA_CborReader reader = nya_cbor_reader(data, size);
        u64            value  = 0;
        (void)nya_cbor_read_u64(&reader, &value);
        fuzz_check_cursor(&reader);
    }
    {
        NYA_CborReader reader = nya_cbor_reader(data, size);
        s64            value  = 0;
        (void)nya_cbor_read_int(&reader, &value);
        fuzz_check_cursor(&reader);
    }
    {
        NYA_CborReader reader = nya_cbor_reader(data, size);
        const u8*      bytes  = nullptr;
        u64            span   = 0;
        if (nya_cbor_read_bytes(&reader, &bytes, &span)) fuzz_check_span(&reader, bytes, span);
        fuzz_check_cursor(&reader);
    }
    {
        NYA_CborReader reader = nya_cbor_reader(data, size);
        const u8*      text   = nullptr;
        u64            span   = 0;
        if (nya_cbor_read_text(&reader, &text, &span)) fuzz_check_span(&reader, text, span);
        fuzz_check_cursor(&reader);
    }
    {
        NYA_CborReader reader = nya_cbor_reader(data, size);
        u64            count  = 0;
        (void)nya_cbor_read_array(&reader, &count);
        fuzz_check_cursor(&reader);
    }
}

#include "tests/fuzz/fuzz.h"
