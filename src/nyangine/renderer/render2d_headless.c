/**
 * @file render2d_headless.c
 *
 * The 2D batch, for builds with no renderer.
 *
 * Every public function of render2d.h, doing nothing. Callers do not guard their draw calls, so
 * a headless build needs the symbols to exist — a test that runs a whole frame of game logic still
 * runs the code that would have drawn it.
 *
 * Kept apart from render2d.c because the two used to live in one file behind a single #if, which
 * meant every function in the subsystem was written twice, seventeen hundred lines apart, and a
 * signature could be changed on one side only. Two files with the same public surface still have
 * that hazard; at least now each is readable on its own, and nyangine.c picks one.
 *
 * Anything with no headless meaning returns a zeroed value: nya_render2d_frame_stats reports an empty
 * frame. Nothing asserts — refusing to draw is the correct behaviour here, not an error.
 *
 * ⚠ **Two things are emphatically not stubbed, because stubbing them was a bug.** The camera and text
 * *measurement* are both computed exactly, by the same code the real renderer runs — see
 * render_camera.c and render_text.h. Neither needs a GPU, and answering zero for them meant a headless
 * caller observed a world the real build would never have handed it. Drawing is still a no-op, which
 * is the one thing a build with no device genuinely cannot do.
 * */
#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PUBLIC API IMPLEMENTATION (HEADLESS)
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/*
 * Stubbed as a block, the way renderer.c stubs itself, so a headless build has the whole surface
 * callable and drawing does nothing. A test that exercises a layer's on_render then needs no
 * knowledge of which build it is in.
 */

void nya_render2d_shutdown(void) {
}

void nya_render2d_layer_set(NYA_Window* window, s32 layer) {
    nya_unused(window, layer);
}

s32 nya_render2d_layer(NYA_Window* window) {
    nya_unused(window);

    return 0;
}

void nya_render2d_flush(NYA_Window* window) {
    nya_assert(window != nullptr);
}

void nya_render2d_rect(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, NYA_Color color) {
    nya_unused(window, x, y, width, height, color);
}

void nya_render2d_rect_outline(NYA_Window* window, f32 x, f32 y, f32 width, f32 height, f32 thickness, NYA_Color color) {
    nya_unused(window, x, y, width, height, thickness, color);
}

void nya_render2d_target_size(NYA_Window* window, OUT u32* out_width, OUT u32* out_height) {
    nya_assert(window != nullptr);
    nya_assert(out_width != nullptr && out_height != nullptr);

    // The window itself, since a headless build never binds a render texture to replace it.
    *out_width  = window->screen_width;
    *out_height = window->screen_height;
}

void nya_render2d_rect_rotated(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, NYA_Color color) {
    nya_unused(window, center, size, rotation, color);
}

void nya_render2d_rect_rotated_outline(NYA_Window* window, f32x2 center, f32x2 size, f32 rotation, f32 thickness, NYA_Color color) {
    nya_unused(window, center, size, rotation, thickness, color);
}

void nya_render2d_line(NYA_Window* window, f32x2 from, f32x2 to, f32 thickness, NYA_Color color) {
    nya_unused(window, from, to, thickness, color);
}

void nya_render2d_polyline(NYA_Window* window, const f32x2* points, u32 count, f32 thickness, NYA_Color color) {
    nya_unused(window, points, count, thickness, color);
}

void nya_render2d_triangle(NYA_Window* window, f32x2 a, f32x2 b, f32x2 c, NYA_Color color) {
    nya_unused(window, a, b, c, color);
}

void nya_render2d_circle(NYA_Window* window, f32x2 center, f32 radius, NYA_Color color) {
    nya_unused(window, center, radius, color);
}

/*
 * The camera is the one piece of state a headless build still has to model, because game logic reads
 * it back and converts through it — and every one of these has to agree with render2d.c exactly, or a
 * test observes a camera the real build would never hand it.
 *
 * They did not, twice over. Reset zeroed the struct and get returned it raw, so a headless caller that
 * reset and then read got a zoom of zero where the real build gives one — the very value render2d.c
 * documents refusing to hand back, since it goes straight into a divide on the way to world space.
 * And the two conversions were the identity, so a headless caller that set a camera and asked where a
 * world point landed was told "wherever it already was". This is the drift the note at the top of this
 * file warns about, found twice in the file that warns about it.
 *
 * Fixed by not writing it twice: the arithmetic lives in render_camera.c and both builds call it. What
 * is left below is only what genuinely differs — the real renderer closes a draw range first, and a
 * headless build has no range to close.
 */
void nya_render2d_camera_set(NYA_Window* window, NYA_Camera2DTopDown camera) {
    nya_assert(window != nullptr);

    window->render_system.draw_batch.camera = (NYA_Camera2D){
        .kind        = NYA_CAMERA2D_KIND_TOP_DOWN,
        .as_top_down = nya_camera2d_top_down_sanitized(camera),
    };
}

void nya_render2d_camera_isometric_set(NYA_Window* window, NYA_Camera2DIsometric camera) {
    nya_assert(window != nullptr);

    window->render_system.draw_batch.camera = (NYA_Camera2D){
        .kind         = NYA_CAMERA2D_KIND_ISOMETRIC,
        .as_isometric = nya_camera2d_isometric_sanitized(camera),
    };
}

void nya_render2d_camera_reset(NYA_Window* window) {
    nya_assert(window != nullptr);

    window->render_system.draw_batch.camera = (NYA_Camera2D){ .kind = NYA_CAMERA2D_KIND_NONE };
}

NYA_Camera2D nya_render2d_camera_get(NYA_Window* window) {
    nya_assert(window != nullptr);

    return window->render_system.draw_batch.camera;
}

NYA_Camera2DTopDown nya_render2d_camera_top_down_get(NYA_Window* window) {
    nya_assert(window != nullptr);

    return nya_camera2d_top_down_or_identity(window->render_system.draw_batch.camera);
}

/*
 * The target size, not the batch's, and that is the one real difference from render2d.c: the batch's
 * copy is written when a flush builds a projection, and nothing here ever flushes. Reading the window
 * directly is what nya_render2d_target_size above already does for the same reason.
 */
f32x2 nya_render2d_screen_to_world(NYA_Window* window, f32x2 screen) {
    nya_assert(window != nullptr);

    u32 width = 0, height = 0;
    nya_render2d_target_size(window, &width, &height);

    return nya_camera2d_screen_to_world(&window->render_system.draw_batch.camera, screen, width, height);
}

f32x2 nya_render2d_world_to_screen(NYA_Window* window, f32x2 world) {
    nya_assert(window != nullptr);

    u32 width = 0, height = 0;
    nya_render2d_target_size(window, &width, &height);

    return nya_camera2d_world_to_screen(&window->render_system.draw_batch.camera, world, width, height);
}

void nya_render2d_texture(NYA_Window* window, NYA_ConstCString texture_handle, f32 x, f32 y, NYA_Color tint) {
    nya_unused(window, texture_handle, x, y, tint);
}

void nya_render2d_texture_ex(NYA_Window* window, NYA_ConstCString texture_handle, NYA_Render2DTexture params) {
    nya_unused(window, texture_handle, params);
}

void nya_render2d_texture_rect(
    NYA_Window*      window,
    NYA_ConstCString texture_handle,
    f32              source_x,
    f32              source_y,
    f32              source_width,
    f32              source_height,
    f32              destination_x,
    f32              destination_y,
    f32              destination_width,
    f32              destination_height,
    NYA_Color        tint
) {
    nya_unused(window, texture_handle, source_x, source_y, source_width, source_height);
    nya_unused(destination_x, destination_y, destination_width, destination_height, tint);
}

/*
 * ── Text ──
 *
 * ⚠ **These used to return zero, and that is what item 4.4 was really about.** A headless build had no
 * text path at all, so nothing under tests/ could reach the layout code — which is how a kerning hash
 * whose multiply wrapped got through 152 tests and two clean builds and then aborted the first frame
 * that drew a character. The same drift as the camera, one layer down and with worse consequences.
 *
 * They answer properly now, and not by reimplementing anything: laying text out needs no GPU, so both
 * builds call the same render_text.c. What a headless build genuinely cannot do is *rasterise*, so the
 * draws below are still stubs while every measurement is exact.
 */

/** The current font, mirrored so the measurements that take no font can find one. */
NYA_INTERNAL NYA_ConstCString _nya_render2d_headless_font      = nullptr;
NYA_INTERNAL f32              _nya_render2d_headless_font_size = 0.0F;

void nya_render2d_font_set(NYA_ConstCString font_path, f32 point_size) {
    _nya_render2d_headless_font      = font_path;
    _nya_render2d_headless_font_size = point_size;
}

f32 nya_render2d_font_size_get(void) {
    return _nya_render2d_headless_font_size;
}

NYA_ConstCString nya_render2d_font_get(void) {
    return _nya_render2d_headless_font;
}

void nya_render2d_nine_slice(NYA_Window* window, NYA_ConstCString texture_handle, NYA_NineSlice params) {
    nya_unused(window, texture_handle, params);
}

/**
 * The size the box would occupy, measured exactly. Nothing is drawn.
 *
 * `line_spacing` and `max_lines` are applied here the same way the real layout applies them, so a
 * caller sizing a panel headless gets the number the panel would have.
 * */
f32x2 nya_render2d_text_box_measure(NYA_ConstCString text, NYA_Render2DTextBox params) {
    if (text == nullptr || text[0] == '\0') return f32x2_zero;

    NYA_ConstCString font_path  = params.font_path != nullptr ? params.font_path : _nya_render2d_headless_font;
    f32              point_size = params.point_size > 0.0F ? params.point_size : _nya_render2d_headless_font_size;

    TTF_Font* font = nya_text_font_for(font_path, point_size);
    if (font == nullptr) return f32x2_zero;

    static NYA_TextRun run;

    s32 wrap_width = params.width > 0.0F ? (s32)params.width : 0;
    if (!nya_text_shape(font, text, 0, wrap_width, &run)) return f32x2_zero;

    f32 line_height = nya_text_line_height(font) * (params.line_spacing > 0.0F ? params.line_spacing : 1.0F);

    u32 lines = run.line_count;
    if (params.max_lines > 0 && lines > params.max_lines) lines = params.max_lines;

    f32 widest = 0.0F;
    for (u32 i = 0; i < lines; i++) widest = nya_max(widest, (f32)run.lines[i].width);

    return (f32x2){ widest, (f32)lines * line_height };
}

f32x2 nya_render2d_text_box(NYA_Window* window, NYA_ConstCString text, NYA_Render2DTextBox params) {
    nya_unused(window);

    // The same measurement, since the real one returns the box it drew into and a headless build draws
    // nothing. A layer that positions the next widget under this one is right either way.
    return nya_render2d_text_box_measure(text, params);
}

void nya_render2d_text(NYA_Window* window, NYA_ConstCString text, f32 x, f32 y, NYA_Color color) {
    nya_unused(window, text, x, y, color);
}

void nya_render2d_text_with_font(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, NYA_ConstCString text, f32 x, f32 y, NYA_Color color) {
    nya_unused(window, font_path, point_size, text, x, y, color);
}

f32x2 nya_render2d_text_measure(NYA_ConstCString text) {
    return nya_render2d_text_measure_with_font(_nya_render2d_headless_font, _nya_render2d_headless_font_size, text);
}

f32x2 nya_render2d_text_measure_with_font(NYA_ConstCString font_path, f32 point_size, NYA_ConstCString text) {
    if (text == nullptr) return f32x2_zero;

    return nya_text_measure_font(nya_text_font_for(font_path, point_size), text, 0);
}

f32 nya_render2d_text_width(NYA_ConstCString text) {
    return nya_render2d_text_measure(text)[0];
}

f32 nya_render2d_text_height(NYA_ConstCString text) {
    return nya_render2d_text_measure(text)[1];
}

f32 nya_render2d_font_line_height(void) {
    return nya_text_line_height(nya_text_font_for(_nya_render2d_headless_font, _nya_render2d_headless_font_size));
}

f32 nya_render2d_font_ascent(void) {
    return nya_text_ascent(nya_text_font_for(_nya_render2d_headless_font, _nya_render2d_headless_font_size));
}

f32 nya_render2d_font_descent(void) {
    return nya_text_descent(nya_text_font_for(_nya_render2d_headless_font, _nya_render2d_headless_font_size));
}

f32 nya_render2d_font_height(void) {
    TTF_Font* font = nya_text_font_for(_nya_render2d_headless_font, _nya_render2d_headless_font_size);

    return nya_text_ascent(font) + nya_text_descent(font);
}

void nya_render2d_shader_begin(NYA_Window* window, NYA_ConstCString pipeline_handle) {
    nya_unused(window, pipeline_handle);
}

void nya_render2d_scissor_begin(NYA_Window* window, f32 x, f32 y, f32 width, f32 height) {
    nya_unused(window, x, y, width, height);
}

void nya_render2d_lights_apply(NYA_Window* window, const NYA_Light2D* lights, const f32x2* positions, u32 count, NYA_Color ambient) {
    nya_unused(window, lights, positions, count, ambient);
}

void nya_render2d_scissor_end(NYA_Window* window) {
    nya_unused(window);
}

void nya_render2d_shader_set_uniform(NYA_Window* window, const void* data, u32 size) {
    nya_unused(window, data, size);
}

void nya_render2d_shader_end(NYA_Window* window) {
    nya_unused(window);
}

NYA_RenderTexture nya_render_texture_create(NYA_Window* window, u32 width, u32 height) {
    nya_unused(window);
    return (NYA_RenderTexture){ .texture = nullptr, .width = width, .height = height };
}

void nya_render_texture_destroy(NYA_RenderTexture* render_texture) {
    if (render_texture != nullptr) *render_texture = (NYA_RenderTexture){ 0 };
}

void nya_render_texture_begin(NYA_Window* window, NYA_RenderTexture* render_texture, NYA_Color clear) {
    nya_unused(window, render_texture, clear);
}

void nya_render_texture_end(NYA_Window* window) {
    nya_unused(window);
}

void nya_render2d_render_texture(NYA_Window* window, const NYA_RenderTexture* render_texture, f32 x, f32 y, f32 width, f32 height, NYA_Color tint) {
    nya_unused(window, render_texture, x, y, width, height, tint);
}

u32 nya_render2d_pending_vertex_count(NYA_Window* window) {
    nya_assert(window != nullptr);
    return window->render_system.draw_batch.vertex_count;
}

NYA_Render2DFrameStats nya_render2d_frame_stats(NYA_Window* window) {
    nya_assert(window != nullptr);

    // Nothing is drawn headless, so nothing was counted.
    return (NYA_Render2DFrameStats){ 0 };
}

void nya_render2d_procedural(NYA_Window* window, NYA_ConstCString pipeline_handle, u32 vertex_count, const void* uniform_data, u32 uniform_size) {
    nya_assert(window != nullptr);
    nya_unused(pipeline_handle);
    nya_unused(vertex_count);
    nya_unused(uniform_data);
    nya_unused(uniform_size);
}

void nya_render2d_textf(NYA_Window* window, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, ...) {
    nya_assert(window != nullptr);
    nya_unused(x);
    nya_unused(y);
    nya_unused(color);
    nya_unused(format);
}

void nya_render2d_textf_with_font(NYA_Window* window, NYA_ConstCString font_path, f32 point_size, f32 x, f32 y, NYA_Color color, NYA_ConstCString format, ...) {
    nya_unused(point_size);
    nya_assert(window != nullptr);
    nya_unused(font_path);
    nya_unused(x);
    nya_unused(y);
    nya_unused(color);
    nya_unused(format);
}

NYA_ConstCString nya_render2d_flush_reason_name(NYA_Render2DFlushReason reason) {
    nya_unused(reason);
    return "none";
}
