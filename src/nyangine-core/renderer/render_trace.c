/**
 * @file render_trace.c
 *
 * GPU time by trace feature, from fences. See debug_trace.h for what the numbers mean.
 *
 * While tracing, a frame acquires its swapchain on one command buffer and encodes into others. Whenever the innermost
 * trace feature has changed at a point with no pass open, the command buffer so far is submitted with a fence and a
 * new one takes its place. The swapchain's command buffer is submitted last, empty, so it presents after everything
 * drawn into its image. A thread waits on the fences in submission order and stamps each as it completes; the frame
 * after reads the stamps back.
 * */
#include "nyangine-core/nyangine.h"

#include "nyangine-core/renderer/render_internal.h"

#if NYA_TRACE_ENABLED

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * STATE
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Groups submitted and not yet read back. A frame makes a dozen or so; past this a group goes untimed. Power of two. */
#define _NYA_RENDER_TRACE_GROUPS 64

typedef struct {
    SDL_GPUFence* fence;
    u64           submitted_ns;

    /** Written by the waiter before it counts the group as waited. */
    u64 done_ns;

    NYA_TraceFeature feature;
} _NYA_RenderTraceGroup;

typedef struct {
    _NYA_RenderTraceGroup groups[_NYA_RENDER_TRACE_GROUPS];

    /** Groups pushed, by the render thread. A group is complete before it is counted. */
    atomic u32 submitted;

    /** Groups whose fence completed, by the waiter. */
    atomic u32 waited;

    /** Groups read back and released, by the render thread. */
    u32 drained;
    u64 previous_done_ns;

    SDL_Thread*    waiter;
    SDL_Semaphore* wake;
    atomic b8      exiting;
} _NYA_RenderTrace;

NYA_INTERNAL _NYA_RenderTrace _nya_render_trace = { 0 };

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API DECLARATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/** Starts the waiter the first time a frame is traced. False when the thread could not start. */
NYA_INTERNAL b8 _nya_render_trace_waiter_ensure(void);

/** The waiter: stamps each fence as it completes, in the order they were submitted. */
NYA_INTERNAL s32 SDLCALL _nya_render_trace_wait(void* user_data);

/** Submits `commands` as a group charged to `feature`. Submitted without a fence when the waiter is too far behind. */
NYA_INTERNAL void _nya_render_trace_submit(SDL_GPUCommandBuffer* commands, NYA_TraceFeature feature);

/** Charges every group the waiter has stamped and releases its fence. */
NYA_INTERNAL void _nya_render_trace_drain(void);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * INTERNAL API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

SDL_GPUCommandBuffer* _nya_render_trace_begin(NYA_Window* window, SDL_GPUCommandBuffer* swapchain_commands) {
    nya_assert(window != nullptr);
    nya_assert(swapchain_commands != nullptr);

    NYA_RenderSystemWindow* render = &window->render_system;
    render->trace_present          = nullptr;

    _nya_render_trace_drain();

    if (!nya_trace_active() || !_nya_render_trace_waiter_ensure()) return swapchain_commands;

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(nya_app_get()->render_system.gpu_device);
    if (commands == nullptr) return swapchain_commands;

    render->trace_present = swapchain_commands;
    render->trace_feature = nya_trace_feature_current();

    return commands;
}

void _nya_render_trace_mark(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;

    if (render->trace_present == nullptr || render->render_commands == nullptr) return;

    NYA_TraceFeature feature = nya_trace_feature_current();
    if (feature == render->trace_feature) return;

    nya_assert(render->render_pass == nullptr, "a trace group can only end with no pass open");

    SDL_GPUCommandBuffer* next = SDL_AcquireGPUCommandBuffer(nya_app_get()->render_system.gpu_device);
    if (next == nullptr) return;

    _nya_render_trace_submit(render->render_commands, render->trace_feature);

    render->render_commands = next;
    render->trace_feature   = feature;
}

void _nya_render_trace_end(NYA_Window* window) {
    NYA_RenderSystemWindow* render = &window->render_system;

    if (render->trace_present == nullptr) {
        SDL_SubmitGPUCommandBuffer(render->render_commands);
        return;
    }

    _nya_render_trace_submit(render->render_commands, render->trace_feature);

    // empty, so it only presents, and after the groups that drew into its image.
    SDL_SubmitGPUCommandBuffer(render->trace_present);
    render->trace_present = nullptr;
}

void _nya_render_trace_shutdown(void) {
    if (_nya_render_trace.waiter == nullptr) return;

    // the device is idle by now, so the waiter finishes every fence before it sees the flag.
    atomic_store(&_nya_render_trace.exiting, true);
    SDL_SignalSemaphore(_nya_render_trace.wake);
    SDL_WaitThread(_nya_render_trace.waiter, nullptr);

    _nya_render_trace_drain();

    SDL_DestroySemaphore(_nya_render_trace.wake);
    _nya_render_trace = (_NYA_RenderTrace){ 0 };
}

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * PRIVATE API IMPLEMENTATION
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

b8 _nya_render_trace_waiter_ensure(void) {
    if (_nya_render_trace.waiter != nullptr) return true;

    _nya_render_trace.wake = SDL_CreateSemaphore(0);
    if (_nya_render_trace.wake == nullptr) return false;

    _nya_render_trace.waiter = SDL_CreateThread(_nya_render_trace_wait, "GPU Trace", nullptr);

    if (_nya_render_trace.waiter == nullptr) {
        nya_log_warn("Could not start the GPU trace thread, so GPU time is not measured: %s", SDL_GetError());
        SDL_DestroySemaphore(_nya_render_trace.wake);
        _nya_render_trace.wake = nullptr;
        return false;
    }

    return true;
}

s32 _nya_render_trace_wait(void* user_data) {
    nya_unused(user_data);

    SDL_GPUDevice* device = nya_app_get()->render_system.gpu_device;

    for (;;) {
        SDL_WaitSemaphore(_nya_render_trace.wake);

        u32 waited = atomic_load(&_nya_render_trace.waited);

        while (waited != atomic_load(&_nya_render_trace.submitted)) {
            _NYA_RenderTraceGroup* group = &_nya_render_trace.groups[waited & (_NYA_RENDER_TRACE_GROUPS - 1)];

            // a lost device returns at once, and the group's time is simply short.
            (void)SDL_WaitForGPUFences(device, true, &group->fence, 1);

            group->done_ns = nya_clock_get_monotonic_ns();

            waited++;
            atomic_store(&_nya_render_trace.waited, waited);
        }

        if (atomic_load(&_nya_render_trace.exiting)) return 0;
    }
}

void _nya_render_trace_submit(SDL_GPUCommandBuffer* commands, NYA_TraceFeature feature) {
    u32 submitted = atomic_load(&_nya_render_trace.submitted);

    if (submitted - _nya_render_trace.drained >= _NYA_RENDER_TRACE_GROUPS) {
        SDL_SubmitGPUCommandBuffer(commands);
        return;
    }

    u64           submitted_ns = nya_clock_get_monotonic_ns();
    SDL_GPUFence* fence        = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);

    if (fence == nullptr) return;

    _nya_render_trace.groups[submitted & (_NYA_RENDER_TRACE_GROUPS - 1)] = (_NYA_RenderTraceGroup){
        .fence        = fence,
        .submitted_ns = submitted_ns,
        .feature      = feature,
    };

    atomic_store(&_nya_render_trace.submitted, submitted + 1);
    SDL_SignalSemaphore(_nya_render_trace.wake);
}

void _nya_render_trace_drain(void) {
    SDL_GPUDevice* device = nya_app_get()->render_system.gpu_device;
    u32            waited = atomic_load(&_nya_render_trace.waited);

    for (; _nya_render_trace.drained != waited; _nya_render_trace.drained++) {
        const _NYA_RenderTraceGroup* group = &_nya_render_trace.groups[_nya_render_trace.drained & (_NYA_RENDER_TRACE_GROUPS - 1)];

        u64 elapsed_ns = nya_trace_gpu_elapsed(group->submitted_ns, _nya_render_trace.previous_done_ns, group->done_ns);
        nya_trace_gpu(group->feature, group->done_ns - elapsed_ns, group->done_ns);

        _nya_render_trace.previous_done_ns = group->done_ns;
        SDL_ReleaseGPUFence(device, group->fence);
    }
}

#endif // NYA_TRACE_ENABLED
