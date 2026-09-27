/**
 * @file base_capture.h
 *
 * A recording tap, in test builds only: what a boundary really received, for a simulation to replay.
 *
 * The last NYA_CAPTURE_RECORDS inputs, bytes and all, replayed dropped, corrupted, cut short or spliced into
 * another session. A shipping build has no tap and pays nothing for one.
 *
 * ```c
 * nya_capture_record(&tap, session, lane, data, size);          // at the boundary, as bytes arrive
 *
 * for (u64 i = 0; i < nya_capture_count(&tap); i++) {            // in the simulation, oldest first
 *     const NYA_CaptureRecord* record = nya_capture_at(&tap, i);
 * }
 * ```
 *
 * `session` and `lane` are the tap's own numbering: a loopback pair and which end sent, an HTTP
 * connection. Splicing across sessions is what the first is for.
 * */
#pragma once

#ifdef NYA_TESTING

#include "nyangine-std/base/base_attributes.h"
#include "nyangine-std/base/base_types.h"

// CONSTANTS

enum {
    /**
     * Bytes one record keeps. A simulated session's snapshots and requests are a few hundred bytes; a
     * longer input is kept cut, and replaying a cut one is only a truncation the simulation also makes.
     * */
    NYA_CAPTURE_RECORD_MAX_BYTES = 1024,

    /** Records one tap keeps before the oldest is overwritten. 64 KiB a tap, in test builds alone. */
    NYA_CAPTURE_RECORDS = 64,
};

// TYPES

typedef struct NYA_CaptureRecord NYA_CaptureRecord;
typedef struct NYA_Capture       NYA_Capture;

struct NYA_CaptureRecord {
    u8  bytes[NYA_CAPTURE_RECORD_MAX_BYTES];
    u32 size;
    u32 session;
    u32 lane;
};

/** Zero is an empty tap. Written by one thread, the one the boundary receives on. */
struct NYA_Capture {
    NYA_CaptureRecord records[NYA_CAPTURE_RECORDS];

    /** Records ever taken; the ring holds the last NYA_CAPTURE_RECORDS of them. */
    u64 taken;
};

// FUNCTIONS

/** Keeps `data`, cut to NYA_CAPTURE_RECORD_MAX_BYTES, over the oldest record once the ring is full. */
NYA_API void nya_capture_record(NYA_Capture* capture, u32 session, u32 lane, const u8* data, u64 size);

/** Forgets every record, so a run that starts here replays only what it captured itself. */
NYA_API void nya_capture_clear(NYA_Capture* capture);

/** Records held, at most NYA_CAPTURE_RECORDS. */
NYA_API u64 nya_capture_count(const NYA_Capture* capture) __attr_no_discard;

/** The `index`th record held, oldest first. */
NYA_API const NYA_CaptureRecord* nya_capture_at(const NYA_Capture* capture, u64 index) __attr_no_discard;

#endif // NYA_TESTING
