/**
 * @file examples/scope_table/main.c
 *
 * nya_scope on its own: a headless loop of frames, each a few nested units of work that allocate from arenas, one
 * of them on a worker thread, and the per scope table they leave behind.
 *
 * ```
 * ./build run example scope_table
 * ```
 *
 * It prints the open stack once from inside the deepest scope, which is the block a crash report carries, then the
 * table: per frame calls, self and total time, the bytes each scope allocated itself, and the most it ever had
 * live. The frame count is the one argument, `./scope_table.example 600`.
 * */
#include "nyangine-core/nyangine.h"

#include "nyangine-core/nyangine.c"

// What nya_scope_watch() below expands to, written by src/nyangine-build/pp/watch.c from the @watch annotation.
#include "genyarated/watches/examples_scope_table_main_c.h"

/* THE WORK */

/** Frames run when no count is given: two seconds at 60 Hz, enough to fill the table's window twice. */
#define FRAMES_DEFAULT 120

#define RECORDS_PER_FRAME  512
#define SIMULATION_STEPS   4
#define REPORT_LINES       64
#define WORKER_BLOCK_BYTES 8192
#define WORKER_BLOCKS      16

typedef struct {
    u32 id;
    f32 position;
    f32 velocity;
} Record;

/** Parses this frame's input into records on the frame arena. */
static Record* input_load(NYA_Arena* frame_arena, u32 frame) {
    nya_scope_function();

    Record* records = nya_arena_alloc(frame_arena, sizeof(Record) * RECORDS_PER_FRAME);
    for (u32 i = 0; i < RECORDS_PER_FRAME; i++) records[i] = (Record){ .id = i, .position = (f32)(i + frame), .velocity = 1.0F };

    return records;
}

/** One integration step. Scoped separately, so the table shows four calls a frame under the simulation. */
static void simulation_step(Record* records, f32 delta_s) {
    nya_scope("simulation_step");

    for (u32 i = 0; i < RECORDS_PER_FRAME; i++) {
        records[i].velocity -= 9.81F * delta_s;
        records[i].position += records[i].velocity * delta_s;
    }
}

// @watch
static f32 simulation_run(Record* records, u32 frame, b8 print_stack) {
    f32 delta_s = 1.0F / 60.0F;
    nya_scope_watch(simulation_run);

    for (u32 step = 0; step < SIMULATION_STEPS; step++) simulation_step(records, delta_s / SIMULATION_STEPS);

    // the same stack a crash report prints, read from the inside.
    if (print_stack) {
        printf("Open scopes in frame %u, outermost first:\n", frame);
        for (u32 i = 0; i < nya_scope_depth(); i++) {
            NYA_ScopeView scope = nya_scope_at(i);
            printf("  %-16s %s:%u  open %.3f ms, allocated " FMTu64 " B\n", scope.name, scope.file, scope.line, nya_time_ns_to_ms(scope.elapsed_ns),
                   scope.allocated_bytes);
        }
        printf("  and %u watched locals registered alongside\n\n", nya_watch_count());
    }

    return records[0].position;
}

/** Formats a summary, which is what allocates the most per frame: one string per line. */
static u64 report_build(NYA_Arena* frame_arena, const Record* records) {
    nya_scope_function();

    u64 length = 0;
    for (u32 line = 0; line < REPORT_LINES; line++) {
        NYA_String* text  = nya_string_sprintf(frame_arena, "record %u at %.2f moving %.2f", records[line].id, (f64)records[line].position,
                                               (f64)records[line].velocity);
        length           += text->length;
    }

    return length;
}

/** A worker's share: its own arena, filled and dropped, charged to its own thread's scope. */
static void worker_compress(void* data) {
    nya_scope("worker_compress");

    u64* checksum = data;

    NYA_Arena* arena = nya_arena_create(.name = "scope_table_worker", .region_size = nya_kibyte_to_byte(256));
    defer      nya_arena_destroy(arena);

    for (u32 block = 0; block < WORKER_BLOCKS; block++) {
        u8* bytes = nya_arena_alloc(arena, WORKER_BLOCK_BYTES);
        nya_memset(bytes, (s32)block, WORKER_BLOCK_BYTES);
        *checksum += bytes[block];
    }
}

/* THE TABLE */

static void table_print(void) {
    NYA_ScopeStats rows[NYA_SCOPE_SITE_MAX];
    u32            count = nya_scope_stats(rows, NYA_SCOPE_SITE_MAX, NYA_SCOPE_SORT_SELF);

    printf("%-18s %6s %9s %9s %10s %10s %10s\n", "scope", "calls", "self ms", "total ms", "alloc B", "peak B", "high B");
    for (u32 i = 0; i < count; i++) {
        const NYA_ScopeStats* row = &rows[i];
        printf("%-18s %6.1f %9.3f %9.3f %10" PRIu64 " %10" PRIu64 " %10" PRIu64 "\n", row->name, (f64)row->calls, row->self_ms, row->total_ms,
               row->allocated_self_bytes, row->peak_bytes, row->high_water_bytes);
    }
}

s32 main(s32 argc, NYA_CString* argv) {
    u32 frames = FRAMES_DEFAULT;
    if (argc > 1) frames = (u32)strtoul(argv[1], nullptr, 10);
    if (frames == 0) frames = FRAMES_DEFAULT;

    NYA_Arena* frame_arena = nya_arena_create(.name = "scope_table_frame");
    NYA_Arena* thread_arena = nya_arena_create(.name = "scope_table_threads");
    defer      nya_arena_destroy(thread_arena);
    defer      nya_arena_destroy(frame_arena);

    u64 checksum = 0;
    f32 position = 0.0F;
    u64 length   = 0;

    for (u32 frame = 0; frame < frames; frame++) {
        // before the frame's own scope opens, as the app loop does, so the frame's scopes close into this frame.
        nya_scope_frame_end();

        {
            nya_scope("frame");

            NYA_Thread* worker = nullptr;
            NYA_EXPECT(nya_thread_spawn(thread_arena, worker_compress, &checksum, "scope_table_worker", &worker));

            Record* records = input_load(frame_arena, frame);
            position        = simulation_run(records, frame, frame == 0);
            length         += report_build(frame_arena, records);

            nya_thread_join(worker);
        }

        nya_arena_free_all(frame_arena);
    }

    nya_scope_frame_end();

    printf("%u frames, position %.2f, %" PRIu64 " report bytes, checksum %" PRIu64 "\n\n", frames, (f64)position, length, checksum);
    table_print();

    nya_assert(nya_scope_depth() == 0, "every scope closed with its block");
    return 0;
}
