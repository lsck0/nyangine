#include "SDL3/SDL_clipboard.h"
#include "SDL3/SDL_events.h"
#include "SDL3/SDL_gpu.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_keycode.h"
#include "SDL3/SDL_messagebox.h"
#include "SDL3/SDL_mouse.h"
#include "SDL3/SDL_pixels.h"
#include "SDL3/SDL_platform.h"
#include "SDL3/SDL_properties.h"
#include "SDL3/SDL_render.h"
#include "SDL3/SDL_video.h"

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * The composed report, and the line index the window scrolls through.
 *
 * Static rather than local for the reason base_logging.c gives about its own report buffer: this is about
 * 80 KiB and the fault path runs on an alternate signal stack where that much is not free. Safe because
 * the crash latch and the reentrancy guard together mean one thread reaches here, exactly once.
 * */
NYA_INTERNAL u8 _nya_crash_report_buffer[NYA_CRASH_REPORT_MAX_BYTES] = { 0 };

/** One line of the report, pointing into whatever buffer nya_crash_window_show was handed. */
typedef struct {
    NYA_ConstCString start;
    u32              length;
} _NYA_CrashReportLine;

NYA_INTERNAL _NYA_CrashReportLine _nya_crash_report_lines[NYA_CRASH_REPORT_LINE_MAX] = { 0 };
NYA_INTERNAL u32                  _nya_crash_report_line_count                       = 0;

/** Whether the observer is in the funnel's list, so deinit takes out exactly what init put in. */
NYA_INTERNAL b8 _nya_crash_reporter_registered = false;

NYA_INTERNAL void _nya_crash_reporter_observe(const NYA_CrashInfo* info, void* user_data);

/**
 * snprintf into `buffer` at `*length`, advancing it. Stops at `capacity` rather than overrunning, and
 * once it has stopped every later call is a no-op, so a caller never has to check between appends.
 * */
NYA_INTERNAL void _nya_crash_append(OUT u8* buffer, u32 capacity, OUT u32* length, NYA_ConstCString format, ...) __attr_fmt_printf(4, 5);

/** Splits `report` on newlines into _nya_crash_report_lines. Lines past the index are dropped. */
NYA_INTERNAL void _nya_crash_report_index(NYA_ConstCString report);

/** Bytes as a human number: `15.9 GiB`. */
NYA_INTERNAL void _nya_crash_format_bytes(u64 bytes, OUT u8* buffer, u32 capacity);

/** Writes `length` bytes to `path`, creating or truncating it. Raw descriptors; no allocator, no stdio. */
NYA_INTERNAL NYA_Error _nya_crash_file_write(NYA_ConstCString path, const u8* data, u32 length) __attr_no_discard;

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * LIFETIME
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

NYA_Error nya_crash_reporter_init(void) {
    if (_nya_crash_reporter_registered) return NYA_OK;

    // The line index is a fixed capacity array like any other, so it says how full it got. Its counter
    // only moves on the crash path, which is the run where the number is worth having. Guarded, because
    // a registration is for the life of the process and init/deinit/init is a legal sequence.
    static b8 ceiling_registered = false;
    if (!ceiling_registered) {
        nya_ceiling_register("crash_report_lines", NYA_CRASH_REPORT_LINE_MAX, &_nya_crash_report_line_count);
        ceiling_registered = true;
    }

    NYA_TRY(nya_crash_observer_add(_nya_crash_reporter_observe, nullptr));
    _nya_crash_reporter_registered = true;

    return NYA_OK;
}

void nya_crash_reporter_deinit(void) {
    if (!_nya_crash_reporter_registered) return;

    (void)nya_crash_observer_remove(_nya_crash_reporter_observe, nullptr);
    _nya_crash_reporter_registered = false;
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE REPORT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_crash_append(OUT u8* buffer, u32 capacity, OUT u32* length, NYA_ConstCString format, ...) {
    nya_assert(buffer != nullptr);
    nya_assert(length != nullptr);

    // One byte is always held back for the terminator, which is also what makes a full buffer inert.
    if (*length + 1 >= capacity) return;

    va_list args;
    va_start(args, format);
    const s32 written = vsnprintf((char*)&buffer[*length], capacity - *length, format, args);
    va_end(args);

    if (written <= 0) return;

    const u32 room  = capacity - *length;
    *length        += (u32)written < room ? (u32)written : room - 1;
}

void _nya_crash_format_bytes(u64 bytes, OUT u8* buffer, u32 capacity) {
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 0);

    // GiB and MiB only: everything reported in bytes here is RAM or VRAM, so it is megabytes at the very
    // least, and a third unit would only show up on a machine that cannot run the engine anyway.
    const u64 MEBIBYTE = 1024ULL * 1024ULL;
    const u64 GIBIBYTE = MEBIBYTE * 1024ULL;

    if (bytes >= GIBIBYTE) {
        (void)snprintf((char*)buffer, capacity, "%.1f GiB", (f64)bytes / (f64)GIBIBYTE);
    } else {
        (void)snprintf((char*)buffer, capacity, "%.0f MiB", (f64)bytes / (f64)MEBIBYTE);
    }
}

/** The build block: which binary this is. */
NYA_INTERNAL void _nya_crash_append_build(OUT u8* buffer, u32 capacity, OUT u32* length) {
    /*
     * The build time is the executable's own modification time, which is when the linker wrote it.
     *
     * Not a -DNYA_BUILD_TIMESTAMP: that flag differs on every invocation, so it would miss the compiler
     * cache on every build of every artifact, while the file's own timestamp is exactly as accurate. See
     * hook_add_build_info_flag, which injects the commit hash beside it for the inverse reason.
     */
    u8 built[NYA_CLOCK_FORMAT_MAX_LENGTH] = "unknown";

    NYA_Arena arena = nya_arena_create_on_stack(.name = "crash_build_info");
    defer     nya_arena_destroy_on_stack(&arena);

    NYA_String* executable = nullptr;
    if (nya_filesystem_executable_path(&arena, &executable).ok) {
        u64 modified_ms = 0;
        if (nya_filesystem_last_modified(nya_string_to_cstring(&arena, executable), &modified_ms).ok) {
            (void)nya_clock_format_utc(modified_ms / 1'000ULL, NYA_CLOCK_FORMAT_READABLE, built, (u32)sizeof(built));
        }
    }

    _nya_crash_append(buffer, capacity, length, "\nBuild\n");
    _nya_crash_append(buffer, capacity, length, "  version   %s\n", NYA_VERSION);
    _nya_crash_append(buffer, capacity, length, "  commit    %s\n", NYA_BUILD_COMMIT);
    _nya_crash_append(buffer, capacity, length, "  kind      %s%s\n", NYA_EXECUTION_MODE_NAME_MAP[NYA_EXECUTION_MODE_CURRENT],
                      NYA_HEADLESS_ENABLED ? ", headless" : "");
    _nya_crash_append(buffer, capacity, length, "  built     %s\n", (NYA_ConstCString)built);
}

/** The platform block: which machine this is. */
NYA_INTERNAL void _nya_crash_append_platform(OUT u8* buffer, u32 capacity, OUT u32* length) {
    u8 cpu[NYA_HOST_CPU_NAME_MAX] = { 0 };
    nya_host_cpu_name(cpu, (u32)sizeof(cpu));

    u8  amount[32] = { 0 };
    u64 bytes      = 0;

    _nya_crash_append(buffer, capacity, length, "\nPlatform\n");
    _nya_crash_append(buffer, capacity, length, "  os        %s\n", SDL_GetPlatform());
    _nya_crash_append(buffer, capacity, length, "  cpu       %s (%u threads)\n", (NYA_ConstCString)cpu, nya_platform_processor_count());

    if (nya_host_memory_total_bytes(&bytes)) {
        _nya_crash_format_bytes(bytes, amount, (u32)sizeof(amount));
        _nya_crash_append(buffer, capacity, length, "  ram       %s\n", (NYA_ConstCString)amount);
    }

    if (nya_host_gpu_memory_total_bytes(&bytes)) {
        _nya_crash_format_bytes(bytes, amount, (u32)sizeof(amount));
        _nya_crash_append(buffer, capacity, length, "  vram      %s\n", (NYA_ConstCString)amount);
    }

    /*
     * The device is read off the app instance rather than through nya_app_get, which asserts when the app
     * is not initialised: a crash during bring-up would then crash again inside the reporter.
     */
    SDL_GPUDevice* device = _NYA_APP_INSTANCE.initialized ? _NYA_APP_INSTANCE.render_system.gpu_device : nullptr;
    if (device == nullptr) {
        _nya_crash_append(buffer, capacity, length, "  gpu       no device (crashed before or after the renderer)\n");
        return;
    }

    const SDL_PropertiesID properties = SDL_GetGPUDeviceProperties(device);

    _nya_crash_append(buffer, capacity, length, "  gpu       %s\n", SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown"));
    _nya_crash_append(buffer, capacity, length, "  driver    %s %s, %s backend\n",
                      SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, "unknown"),
                      SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, ""), SDL_GetGPUDeviceDriver(device));

    // What the engine itself asked the driver for, which is the number a texture leak shows up in. Not the
    // same question as how much the card has, so it is a line of its own.
    u64 gpu_bytes = 0;
    for (u32 kind = 0; kind < NYA_GPU_MEMORY_KIND_COUNT; kind++) gpu_bytes += nya_gpu_memory_bytes((NYA_GPUMemoryKind)kind);

    _nya_crash_format_bytes(gpu_bytes, amount, (u32)sizeof(amount));
    _nya_crash_append(buffer, capacity, length, "  gpu used  %s allocated by the engine\n", (NYA_ConstCString)amount);
}

u32 nya_crash_report_compose(const NYA_CrashInfo* info, OUT u8* buffer, u32 capacity) {
    nya_assert(info != nullptr);
    nya_assert(buffer != nullptr);
    nya_assert(capacity > 1);
    nya_assert(info->source < NYA_CRASH_SOURCE_COUNT);

    u32 length = 0;
    buffer[0]  = '\0';

    u8 now[NYA_CLOCK_FORMAT_MAX_LENGTH] = { 0 };
    (void)nya_clock_format_utc(nya_clock_get_timestamp_s(), NYA_CLOCK_FORMAT_READABLE, now, (u32)sizeof(now));

    _nya_crash_append(buffer, capacity, &length, "%s crash report\n", nya_save_application());
    _nya_crash_append(buffer, capacity, &length, "%s\n", (NYA_ConstCString)now);

    _nya_crash_append(buffer, capacity, &length, "\n%s\n", NYA_CRASH_SOURCE_NAME_MAP[info->source]);
    _nya_crash_append(buffer, capacity, &length, "  %s\n", (NYA_ConstCString)info->message);
    _nya_crash_append(buffer, capacity, &length, "  in %s (%s:%u)\n", info->function, info->file, info->line);

    if (info->source == NYA_CRASH_SOURCE_FAULT) {
        _nya_crash_append(buffer, capacity, &length, "  signal %d at address 0x%llx\n", info->signal, (unsigned long long)info->fault_address);
    }
    if (info->source == NYA_CRASH_SOURCE_ERROR && info->error_kind < NYA_ERROR_COUNT) {
        _nya_crash_append(buffer, capacity, &length, "  error kind %s\n", NYA_ERRORKIND_NAME_MAP[info->error_kind]);
    }

    _nya_crash_append_build(buffer, capacity, &length);
    _nya_crash_append_platform(buffer, capacity, &length);

    _nya_crash_append(buffer, capacity, &length, "\nStack trace\n");
    if (info->backtrace.count == 0) {
        _nya_crash_append(buffer, capacity, &length, "  none captured\n");
    } else if (length + 1 < capacity) {
        length += nya_backtrace_format(&info->backtrace, &buffer[length], capacity - length);
    }

    const u32 logged = nya_log_ring_count();
    _nya_crash_append(buffer, capacity, &length, "\nLog, the last %u lines of at most %u\n", logged, (u32)NYA_LOG_RING_MAX);
    for (u32 i = 0; i < logged; i++) _nya_crash_append(buffer, capacity, &length, "  %s\n", nya_log_ring_at(i));

    /*
     * Deliberately not appended through _nya_crash_append, which is inert once the buffer is full. A
     * report that was cut has to say so, or the missing tail reads as a program that simply stopped.
     */
    if (length + 1 >= capacity) {
        NYA_ConstCString marker        = "\n[report truncated]\n";
        const u32        marker_length = (u32)strlen(marker);

        if (capacity > marker_length + 1) {
            length = capacity - marker_length - 1;
            nya_memcpy(&buffer[length], marker, marker_length);
            length += marker_length;
        }
    }

    buffer[length] = '\0';

    nya_assert(length < capacity, "the crash report overran its buffer: %u of %u bytes", length, capacity);
    return length;
}

NYA_Error _nya_crash_file_write(NYA_ConstCString path, const u8* data, u32 length) {
    nya_assert(path != nullptr);
    nya_assert(data != nullptr);

#if OS_WINDOWS
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nya_error(NYA_ERROR_IO, "could not create '%s'", path);

    DWORD    wrote = 0;
    const b8 ok    = WriteFile(file, data, (DWORD)length, &wrote, nullptr) && wrote == length;
    (void)CloseHandle(file);
#else
    const s32 file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0o644);
    if (file < 0) return nya_error(NYA_ERROR_IO, "could not create '%s': %s", path, strerror(errno));

    // A short write is a failure rather than something to loop on: this is tens of kilobytes to a local
    // file, so a partial write means the filesystem refused, not that a pipe filled up.
    const ssize_t wrote = write(file, data, length);
    const b8      ok    = wrote >= 0 && (u32)wrote == length;
    (void)close(file);
#endif

    if (!ok) return nya_error(NYA_ERROR_IO, "could not write the whole crash report to '%s'", path);

    return NYA_OK;
}

NYA_Error nya_crash_report_submit(NYA_ConstCString report, OUT u8* out_path, u32 path_capacity) {
    nya_assert(report != nullptr);
    nya_assert(out_path != nullptr);
    nya_assert(path_capacity > 0);

    out_path[0] = '\0';

    /*
     * Where the engine is already allowed to write, picked once at startup from the user data directory.
     * Resolving it again here would need an arena and a filesystem probe on the crash path, and would be
     * a second place deciding where the engine's files live.
     */
    NYA_ConstCString directory = nya_log_directory();
    if (directory[0] == '\0') return nya_error(NYA_ERROR_NOT_FOUND, "there is no log directory to write the crash report into");

    u8 stamp[NYA_CLOCK_FORMAT_MAX_LENGTH] = { 0 };
    (void)nya_clock_format_utc(nya_clock_get_timestamp_s(), NYA_CLOCK_FORMAT_FILENAME, stamp, (u32)sizeof(stamp));

    const s32 written = snprintf((char*)out_path, path_capacity, "%s/crash-%s.txt", directory, (const char*)stamp);
    if (written <= 0 || (u32)written >= path_capacity) {
        out_path[0] = '\0';
        return nya_error(NYA_ERROR_INVALID_ARGUMENT, "the crash report path does not fit in %u bytes", path_capacity);
    }

    /*
     * No network call, and no endpoint compiled in anywhere. Sending a crash report somewhere is something
     * done to a player, so it needs a destination they agreed to; until there is one, "send" means putting
     * it where they can find it and telling them where that is. A transport replaces this one call and
     * nothing else: the button above already treats submitting as an operation that reports what it did.
     */
    NYA_TRY(_nya_crash_file_write((NYA_ConstCString)out_path, (const u8*)report, (u32)strlen(report)));

    return NYA_OK;
}

void _nya_crash_report_index(NYA_ConstCString report) {
    nya_assert(report != nullptr);

    _nya_crash_report_line_count = 0;

    NYA_ConstCString start = report;
    for (NYA_ConstCString cursor = report;; cursor++) {
        if (*cursor != '\n' && *cursor != '\0') continue;
        if (_nya_crash_report_line_count >= NYA_CRASH_REPORT_LINE_MAX) break;

        _nya_crash_report_lines[_nya_crash_report_line_count++] = (_NYA_CrashReportLine){ .start = start, .length = (u32)(cursor - start) };

        if (*cursor == '\0') break;
        start = cursor + 1;
    }
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE WINDOW
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Logical size. Wide enough for a 110 column line, tall enough for about forty of them. */
#define _NYA_CRASH_WINDOW_WIDTH  960
#define _NYA_CRASH_WINDOW_HEIGHT 660

#define _NYA_CRASH_PADDING       16
#define _NYA_CRASH_HEADER_HEIGHT 56
#define _NYA_CRASH_LINE_HEIGHT   12
#define _NYA_CRASH_BUTTON_WIDTH  184
#define _NYA_CRASH_BUTTON_HEIGHT 30

/** Lines one wheel notch moves. Three is what a terminal does, and the report reads like one. */
#define _NYA_CRASH_WHEEL_LINES 3

/** How long the loop waits for an event before drawing anyway, so nothing depends on an event arriving. */
#define _NYA_CRASH_POLL_MS 100

/** Largest display scale the window honours. Past 3 the report stops fitting, which defeats the point. */
#define _NYA_CRASH_SCALE_MAX 3.0F

/** The buttons, left to right, in order of consequence: leave, take a copy, hand it over. */
typedef enum {
    _NYA_CRASH_BUTTON_CLOSE,
    _NYA_CRASH_BUTTON_COPY,
    _NYA_CRASH_BUTTON_SEND,

    _NYA_CRASH_BUTTON_COUNT,
} _NYA_CrashButton;

NYA_INTERNAL NYA_ConstCString _NYA_CRASH_BUTTON_LABEL_MAP[_NYA_CRASH_BUTTON_COUNT] = {
    [_NYA_CRASH_BUTTON_CLOSE] = "Close",
    [_NYA_CRASH_BUTTON_COPY]  = "Copy",
    [_NYA_CRASH_BUTTON_SEND]  = "Send to developer",
};

/*
 * Flat colours, one filled rectangle per thing and no borders, which is the look the rest of the engine's
 * debug UI has. Written 0xRRGGBB so the palette reads as a palette; _nya_crash_draw_color splits it.
 */
#define _NYA_CRASH_COLOR_BACKGROUND 0x17171B
#define _NYA_CRASH_COLOR_HEADER     0xC0392B
#define _NYA_CRASH_COLOR_PANE       0x202027
#define _NYA_CRASH_COLOR_BUTTON     0x31313B
#define _NYA_CRASH_COLOR_BUTTON_HOT 0x43434F
#define _NYA_CRASH_COLOR_TEXT       0xD7D7DC
#define _NYA_CRASH_COLOR_DIM        0x8C8C97

NYA_INTERNAL void _nya_crash_draw_color(SDL_Renderer* renderer, u32 color) {
    (void)SDL_SetRenderDrawColor(renderer, (u8)(color >> 16), (u8)(color >> 8), (u8)color, SDL_ALPHA_OPAQUE);
}

NYA_INTERNAL void _nya_crash_draw_rect(SDL_Renderer* renderer, u32 color, SDL_FRect rect) {
    _nya_crash_draw_color(renderer, color);
    (void)SDL_RenderFillRect(renderer, &rect);
}

/** Where button `button` sits, in logical coordinates. One place, so drawing and hit testing agree. */
NYA_INTERNAL SDL_FRect _nya_crash_button_rect(_NYA_CrashButton button) {
    nya_assert(button < _NYA_CRASH_BUTTON_COUNT);

    const f32 step      = (f32)(_NYA_CRASH_BUTTON_WIDTH + _NYA_CRASH_PADDING);
    const f32 remaining = (f32)(_NYA_CRASH_BUTTON_COUNT - (u32)button);

    return (SDL_FRect){
        // Right aligned, so the pointer resting over Send is furthest from Close.
        .x = (f32)(_NYA_CRASH_WINDOW_WIDTH - _NYA_CRASH_PADDING) - (remaining * step) + (f32)_NYA_CRASH_PADDING,
        .y = (f32)(_NYA_CRASH_WINDOW_HEIGHT - _NYA_CRASH_PADDING - _NYA_CRASH_BUTTON_HEIGHT),
        .w = (f32)_NYA_CRASH_BUTTON_WIDTH,
        .h = (f32)_NYA_CRASH_BUTTON_HEIGHT,
    };
}

NYA_INTERNAL b8 _nya_crash_rect_contains(SDL_FRect rect, f32 x, f32 y) {
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

/** How many report lines fit in the pane: the window less the header, the buttons and the gaps around them. */
NYA_INTERNAL u32 _nya_crash_visible_lines(void) {
    const s32 pane = _NYA_CRASH_WINDOW_HEIGHT - _NYA_CRASH_HEADER_HEIGHT - (3 * _NYA_CRASH_PADDING) - _NYA_CRASH_BUTTON_HEIGHT;
    return (u32)(pane / _NYA_CRASH_LINE_HEIGHT);
}

/** Clamps a scroll position so the pane never shows past the end of the report. */
NYA_INTERNAL u32 _nya_crash_scroll_clamp(s64 scroll) {
    const u32 visible = _nya_crash_visible_lines();
    const s64 last    = _nya_crash_report_line_count > visible ? (s64)(_nya_crash_report_line_count - visible) : 0;

    if (scroll < 0) return 0;
    if (scroll > last) return (u32)last;

    return (u32)scroll;
}

NYA_INTERNAL void _nya_crash_window_draw(SDL_Renderer* renderer, const NYA_CrashInfo* info, u32 scroll, NYA_ConstCString status, f32 mouse_x,
                                         f32 mouse_y) {
    _nya_crash_draw_color(renderer, _NYA_CRASH_COLOR_BACKGROUND);
    (void)SDL_RenderClear(renderer);

    /* Header: what happened, in the one line someone will read out loud. */
    _nya_crash_draw_rect(renderer, _NYA_CRASH_COLOR_HEADER,
                         (SDL_FRect){ .x = 0, .y = 0, .w = (f32)_NYA_CRASH_WINDOW_WIDTH, .h = (f32)_NYA_CRASH_HEADER_HEIGHT });

    _nya_crash_draw_color(renderer, _NYA_CRASH_COLOR_TEXT);
    (void)SDL_RenderDebugTextFormat(renderer, (f32)_NYA_CRASH_PADDING, 14.0F, "%s: %s", NYA_CRASH_SOURCE_NAME_MAP[info->source],
                                    (const char*)info->message);
    (void)SDL_RenderDebugTextFormat(renderer, (f32)_NYA_CRASH_PADDING, 32.0F, "in %s (%s:%u)", info->function, info->file, info->line);

    /* Pane: the report, which is also exactly what Copy and Send hand over. */
    const f32 pane_y  = (f32)(_NYA_CRASH_HEADER_HEIGHT + _NYA_CRASH_PADDING);
    const u32 visible = _nya_crash_visible_lines();

    _nya_crash_draw_rect(renderer, _NYA_CRASH_COLOR_PANE,
                         (SDL_FRect){ .x = (f32)_NYA_CRASH_PADDING,
                                      .y = pane_y,
                                      .w = (f32)(_NYA_CRASH_WINDOW_WIDTH - (2 * _NYA_CRASH_PADDING)),
                                      .h = (f32)(visible * _NYA_CRASH_LINE_HEIGHT) });

    _nya_crash_draw_color(renderer, _NYA_CRASH_COLOR_TEXT);

    for (u32 row = 0; row < visible; row++) {
        const u32 index = scroll + row;
        if (index >= _nya_crash_report_line_count) break;

        const _NYA_CrashReportLine line = _nya_crash_report_lines[index];

        // Copied out because SDL_RenderDebugText wants a terminated string and the report is one block.
        char      text[NYA_LOG_RING_LINE_MAX];
        const u32 length = line.length < sizeof(text) - 1 ? line.length : (u32)sizeof(text) - 1;
        nya_memcpy(text, line.start, length);
        text[length] = '\0';

        (void)SDL_RenderDebugText(renderer, (f32)(_NYA_CRASH_PADDING + 6), pane_y + 3.0F + (f32)(row * _NYA_CRASH_LINE_HEIGHT), text);
    }

    /*
     * Status line: where a written report went, or why it could not be written. Above the buttons rather
     * than beside them, because a path is longer than the strip left over next to three of them.
     */
    _nya_crash_draw_color(renderer, _NYA_CRASH_COLOR_DIM);
    (void)SDL_RenderDebugText(renderer, (f32)_NYA_CRASH_PADDING, pane_y + (f32)(visible * _NYA_CRASH_LINE_HEIGHT) + 6.0F, status);

    for (u32 i = 0; i < _NYA_CRASH_BUTTON_COUNT; i++) {
        const SDL_FRect rect = _nya_crash_button_rect((_NYA_CrashButton)i);
        const b8        hot  = _nya_crash_rect_contains(rect, mouse_x, mouse_y);

        _nya_crash_draw_rect(renderer, hot ? _NYA_CRASH_COLOR_BUTTON_HOT : _NYA_CRASH_COLOR_BUTTON, rect);

        // Centred against the 8 pixel cell the debug font is defined in.
        const f32 label_width = (f32)(strlen(_NYA_CRASH_BUTTON_LABEL_MAP[i]) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE);

        _nya_crash_draw_color(renderer, _NYA_CRASH_COLOR_TEXT);
        (void)SDL_RenderDebugText(renderer, rect.x + ((rect.w - label_width) / 2.0F),
                                  rect.y + ((rect.h - (f32)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) / 2.0F), _NYA_CRASH_BUTTON_LABEL_MAP[i]);
    }

    (void)SDL_RenderPresent(renderer);
}

/** Runs a button. True when the window should close. */
NYA_INTERNAL b8 _nya_crash_button_activate(_NYA_CrashButton button, NYA_ConstCString report, OUT u8* status, u32 status_capacity) {
    nya_assert(button < _NYA_CRASH_BUTTON_COUNT);
    nya_assert(report != nullptr);

    switch (button) {
        case _NYA_CRASH_BUTTON_CLOSE: return true;

        case _NYA_CRASH_BUTTON_COPY: {
            if (SDL_SetClipboardText(report)) {
                (void)snprintf((char*)status, status_capacity, "The whole report is on the clipboard.");
            } else {
                (void)snprintf((char*)status, status_capacity, "Could not reach the clipboard: %s", SDL_GetError());
            }
            return false;
        }

        case _NYA_CRASH_BUTTON_SEND: {
            u8              path[NYA_CRASH_REPORT_PATH_MAX] = { 0 };
            const NYA_Error written                         = nya_crash_report_submit(report, path, (u32)sizeof(path));

            if (written.ok) {
                (void)snprintf((char*)status, status_capacity, "Written to %s", (const char*)path);
            } else {
                (void)snprintf((char*)status, status_capacity, "Could not write the report: %s", (NYA_ConstCString)written.message);
            }
            return false;
        }

        default: nya_unreachable();
    }
    static_assert(_NYA_CRASH_BUTTON_COUNT == 3, "Unhandled _NYA_CrashButton enum value.");

    nya_unreachable();
}

/** One event. True when the window should close. */
NYA_INTERNAL b8 _nya_crash_window_handle(const SDL_Event* event, NYA_ConstCString report, OUT u32* scroll, OUT f32* mouse_x, OUT f32* mouse_y,
                                         OUT u8* status, u32 status_capacity) {
    switch (event->type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return true;

        case SDL_EVENT_MOUSE_MOTION:
            *mouse_x = event->motion.x;
            *mouse_y = event->motion.y;
            return false;

        case SDL_EVENT_MOUSE_WHEEL: *scroll = _nya_crash_scroll_clamp((s64)*scroll - ((s64)event->wheel.y * _NYA_CRASH_WHEEL_LINES)); return false;

        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            if (event->button.button != SDL_BUTTON_LEFT) return false;

            for (u32 i = 0; i < _NYA_CRASH_BUTTON_COUNT; i++) {
                if (!_nya_crash_rect_contains(_nya_crash_button_rect((_NYA_CrashButton)i), event->button.x, event->button.y)) continue;
                return _nya_crash_button_activate((_NYA_CrashButton)i, report, status, status_capacity);
            }
            return false;
        }

        case SDL_EVENT_KEY_DOWN: {
            const s64 page = (s64)_nya_crash_visible_lines();

            switch (event->key.key) {
                case SDLK_ESCAPE:   return true;
                case SDLK_UP:       *scroll = _nya_crash_scroll_clamp((s64)*scroll - 1); break;
                case SDLK_DOWN:     *scroll = _nya_crash_scroll_clamp((s64)*scroll + 1); break;
                case SDLK_PAGEUP:   *scroll = _nya_crash_scroll_clamp((s64)*scroll - page); break;
                case SDLK_PAGEDOWN: *scroll = _nya_crash_scroll_clamp((s64)*scroll + page); break;
                case SDLK_HOME:     *scroll = 0; break;
                case SDLK_END:      *scroll = _nya_crash_scroll_clamp((s64)_nya_crash_report_line_count); break;
                default:            break;
            }
            return false;
        }

        default: return false;
    }
}

void nya_crash_window_show(const NYA_CrashInfo* info, NYA_ConstCString report) {
    nya_assert(info != nullptr);
    nya_assert(report != nullptr);

    // Nothing to open a window on: a headless build, a test, or a crash from before SDL came up. The
    // caller has the report either way, so this is a missing nicety rather than a lost report.
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return;

    _nya_crash_report_index(report);

    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;

    // Always on top: the game's own window is very likely still up, frozen on whatever it last drew.
    if (!SDL_CreateWindowAndRenderer(nya_save_application(), _NYA_CRASH_WINDOW_WIDTH, _NYA_CRASH_WINDOW_HEIGHT, SDL_WINDOW_ALWAYS_ON_TOP, &window,
                                     &renderer)) {
        // The last thing guaranteed to be seen. It cannot hold the report, so it points at where one is.
        (void)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, nya_save_application(),
                                       "The game crashed and the crash window could not be opened. The log directory has the details.", nullptr);
        return;
    }

    // The debug font is 8 pixels tall, which is unreadable on a dense display, so the whole layout scales
    // by whatever the display asks for. Clamped, because past 3 the report no longer fits the window.
    const f32 scale = nya_clamp(SDL_GetWindowDisplayScale(window), 1.0F, _NYA_CRASH_SCALE_MAX);
    (void)SDL_SetWindowSize(window, (s32)((f32)_NYA_CRASH_WINDOW_WIDTH * scale), (s32)((f32)_NYA_CRASH_WINDOW_HEIGHT * scale));
    (void)SDL_SetRenderScale(renderer, scale, scale);

    (void)SDL_RaiseWindow(window);

    u32 scroll  = 0;
    f32 mouse_x = -1.0F;
    f32 mouse_y = -1.0F;

    u8 status[NYA_CRASH_REPORT_PATH_MAX + 64] = { 0 };
    (void)snprintf((char*)status, sizeof(status), "Scroll with the wheel, the arrows, or page up and down.");

    b8 done = false;
    while (!done) {
        SDL_Event event;

        // Waits rather than polls, so an open crash window is not a core spinning at 100%. The timeout is
        // what redraws the window when no event arrives, which is what a compositor expects after an
        // expose it did not send us.
        while (!done && SDL_WaitEventTimeout(&event, _NYA_CRASH_POLL_MS)) {
            // Turns window pixels into the logical coordinates the layout above is written in, which is
            // what makes the hit tests agree with the drawing under a display scale.
            (void)SDL_ConvertEventToRenderCoordinates(renderer, &event);

            done = _nya_crash_window_handle(&event, report, &scroll, &mouse_x, &mouse_y, status, (u32)sizeof(status));
        }

        if (done) break;

        _nya_crash_window_draw(renderer, info, scroll, (NYA_ConstCString)status, mouse_x, mouse_y);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * THE OBSERVER
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

void _nya_crash_reporter_observe(const NYA_CrashInfo* info, void* user_data) {
    nya_unused(user_data);
    nya_assert(info != nullptr);

    (void)nya_crash_report_compose(info, _nya_crash_report_buffer, (u32)sizeof(_nya_crash_report_buffer));

    /*
     * A fault gets the file rather than the window, and gets it without being asked: see debug_crash.h for
     * why SDL is not called from a signal handler. The path goes out through write(2), the only output
     * that is safe here.
     */
    if (info->fault_path) {
        u8 path[NYA_CRASH_REPORT_PATH_MAX] = { 0 };
        if (!nya_crash_report_submit((NYA_ConstCString)_nya_crash_report_buffer, path, (u32)sizeof(path)).ok) return;

        u8        line[NYA_CRASH_REPORT_PATH_MAX + 32] = { 0 };
        const s32 written = snprintf((char*)line, sizeof(line), "\nCrash report written to %s\n", (const char*)path);
        if (written > 0) nya_log_write_stderr((NYA_ConstCString)line, (u32)written);

        return;
    }

    nya_crash_window_show(info, (NYA_ConstCString)_nya_crash_report_buffer);
}
