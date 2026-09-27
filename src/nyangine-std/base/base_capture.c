#include "nyangine-std/base/base_capture.h"

#ifdef NYA_TESTING

#include "nyangine-std/base/base_assert.h"
#include "nyangine-std/base/base_memory.h"

void nya_capture_record(NYA_Capture* capture, u32 session, u32 lane, const u8* data, u64 size) {
    nya_assert(capture != nullptr);
    nya_assert(data != nullptr || size == 0);

    NYA_CaptureRecord* record = &capture->records[capture->taken % NYA_CAPTURE_RECORDS];

    u64 kept = size < NYA_CAPTURE_RECORD_MAX_BYTES ? size : NYA_CAPTURE_RECORD_MAX_BYTES;
    if (kept > 0) nya_memcpy(record->bytes, data, kept);

    record->size    = (u32)kept;
    record->session = session;
    record->lane    = lane;

    capture->taken++;
}

void nya_capture_clear(NYA_Capture* capture) {
    nya_assert(capture != nullptr);

    capture->taken = 0;
}

u64 nya_capture_count(const NYA_Capture* capture) {
    nya_assert(capture != nullptr);

    return capture->taken < NYA_CAPTURE_RECORDS ? capture->taken : NYA_CAPTURE_RECORDS;
}

const NYA_CaptureRecord* nya_capture_at(const NYA_Capture* capture, u64 index) {
    nya_assert(capture != nullptr);
    nya_assert(index < nya_capture_count(capture), "record %llu of %llu held", (unsigned long long)index, (unsigned long long)nya_capture_count(capture));

    u64 oldest = capture->taken - nya_capture_count(capture);

    const NYA_CaptureRecord* record = &capture->records[(oldest + index) % NYA_CAPTURE_RECORDS];
    nya_assert(record->size <= NYA_CAPTURE_RECORD_MAX_BYTES);

    return record;
}

#endif // NYA_TESTING
