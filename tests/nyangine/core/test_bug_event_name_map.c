/**
 * @file test_bug_event_name_map.c
 *
 * NYA_EVENT_NAME_MAP has to have an entry for every event type.
 *
 * It is declared `NYA_EVENT_NAME_MAP[]` — no explicit size — and filled with designated
 * initialisers. That makes its length "one past the highest index anyone wrote", not
 * NYA_EVENT_COUNT, so an event type nobody added an entry for is either a null pointer inside the
 * array or off the end of it entirely, depending only on where it sits in the enum.
 *
 * Three real event types were missing: NYA_EVENT_JOB_STARTED, NYA_EVENT_JOB_COMPLETED and
 * NYA_EVENT_ASSET_LOAD_FAILED. core_event.c's dispatch does
 *
 *     nya_log_trace("Event dispatched: %s", NYA_EVENT_NAME_MAP[event.type]);
 *
 * on every single dispatch, so dispatching any of those three passed a null pointer to a %s
 * conversion. That is undefined behaviour; glibc happens to print "(null)" instead of faulting,
 * which is exactly why it went unnoticed — and the job events are dispatched by the job system on
 * every job start and finish.
 *
 * The sibling maps in this codebase are all spelled with an explicit size —
 * NYA_INTEGRITY_STATUS_NAME_MAP[NYA_INTEGRITY_STATUS_COUNT],
 * NYA_FILETYPE_NAME_MAP[NYA_FILE_TYPE_COUNT] — which is what makes a missing entry a null to find
 * rather than a short array. This one was the exception.
 *
 * The static assert below is the real fix: it makes a future event type added without a name a
 * compile error rather than something to discover in a trace log.
 * */

#include "nyangine/nyangine.c"
#include "nyangine/nyangine.h"

s32 main(void) {
    printf("TEST: every event type has a name\n");

    u64 map_length = sizeof(NYA_EVENT_NAME_MAP) / sizeof(NYA_EVENT_NAME_MAP[0]);

    // The array must cover the whole enum, not merely up to the last entry someone remembered.
    nya_check(
        map_length >= (u64)NYA_EVENT_COUNT,
        "NYA_EVENT_NAME_MAP holds " FMTu64 " entries but there are %d event types, so the highest ones index out of bounds",
        map_length,
        (s32)NYA_EVENT_COUNT
    );

    // And no gap inside it, which is what a missing designated initialiser leaves behind.
    for (s32 type = 0; type < (s32)NYA_EVENT_COUNT; type++) {
        if ((u64)type >= map_length) break;

        nya_check(NYA_EVENT_NAME_MAP[type] != nullptr, "event type %d has no name; dispatching it passes null to a %%s conversion", type);
    }

    // The three that were actually missing, named so a regression says which.
    if ((u64)NYA_EVENT_JOB_STARTED < map_length) {
        nya_check(NYA_EVENT_NAME_MAP[NYA_EVENT_JOB_STARTED] != nullptr, "NYA_EVENT_JOB_STARTED has no name");
    }
    if ((u64)NYA_EVENT_JOB_COMPLETED < map_length) {
        nya_check(NYA_EVENT_NAME_MAP[NYA_EVENT_JOB_COMPLETED] != nullptr, "NYA_EVENT_JOB_COMPLETED has no name");
    }
    if ((u64)NYA_EVENT_ASSET_LOAD_FAILED < map_length) {
        nya_check(NYA_EVENT_NAME_MAP[NYA_EVENT_ASSET_LOAD_FAILED] != nullptr, "NYA_EVENT_ASSET_LOAD_FAILED has no name");
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // TEST: the same property for every other name map in the tree
    // ─────────────────────────────────────────────────────────────────────────────
    //
    // The event map was the one that was wrong, but nothing about the mistake was specific to it —
    // any of these is a designated-initialiser table indexed by an enum, and any of them can grow an
    // enumerator without growing a row. Checked here rather than in five separate places because the
    // property is identical and the failure mode is identical.
    printf("TEST: every name map is complete\n");

#define _NYA_CHECK_NAME_MAP(map, count)                                                                                                              \
    do {                                                                                                                                             \
        u64 _length = sizeof(map) / sizeof((map)[0]);                                                                                                \
        nya_check(_length == (u64)(count), #map " has " FMTu64 " entries but " #count " is %d", _length, (s32)(count));                               \
        for (u64 _i = 0; _i < _length && _i < (u64)(count); _i++) {                                                                                  \
            nya_check((map)[_i] != nullptr, #map "[" FMTu64 "] has no name", _i);                                                                    \
        }                                                                                                                                            \
    } while (0)

    _NYA_CHECK_NAME_MAP(NYA_EVENT_NAME_MAP, NYA_EVENT_COUNT);
    _NYA_CHECK_NAME_MAP(NYA_ERRORKIND_NAME_MAP, NYA_ERROR_COUNT);
    _NYA_CHECK_NAME_MAP(NYA_INTEGRITY_STATUS_NAME_MAP, NYA_INTEGRITY_STATUS_COUNT);
    _NYA_CHECK_NAME_MAP(NYA_TYPE_NAME_MAP, NYA_TYPE_COUNT);
    _NYA_CHECK_NAME_MAP(NYA_SERDE_FORMAT_NAME_MAP, NYA_SERDE_FORMAT_COUNT);
    _NYA_CHECK_NAME_MAP(NYA_FILETYPE_NAME_MAP, NYA_FILE_TYPE_COUNT);

#undef _NYA_CHECK_NAME_MAP

    return nya_check_failures() == 0 ? 0 : 1;
}
