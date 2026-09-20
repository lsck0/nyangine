/**
 * @file host.h
 *
 * What the machine underneath the process is, for a crash report and the debug overlay to print.
 *
 * ```c
 * u8 cpu[NYA_HOST_CPU_NAME_MAX];
 * nya_host_cpu_name(cpu, sizeof(cpu));
 *
 * u64 ram_bytes = 0;
 * if (nya_host_memory_total_bytes(&ram_bytes)) printf("%s, " FMTu64 " MiB\n", (char*)cpu, ram_bytes / (1024 * 1024));
 * ```
 *
 * Every probe answers from the OS directly, without SDL, because the build tool compiles platform with
 * -DNYA_NO_SDL. None of them allocate: each writes into the caller's buffer or a single u64, so the crash
 * path can ask after the allocator is no longer trustworthy.
 *
 * A probe that cannot answer returns false and leaves the out parameter alone rather than reporting a
 * zero that reads like a real measurement. `nya_host_cpu_name` always writes something, falling back to
 * the architecture name, since "unknown CPU" is still a line worth printing.
 * */
#pragma once

#include "nyangine/base/base_attributes.h"
#include "nyangine/base/base_types.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * CONSTANTS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Longest CPU name, terminator included. The x86 CPUID brand string is 48 bytes by definition and
 * /proc/cpuinfo's `model name` has never been seen longer, so 64 leaves room without a second page.
 * */
#define NYA_HOST_CPU_NAME_MAX 64

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Writes the processor's marketing name, null terminated and truncated to `capacity`. Always writes.
 * */
NYA_API void nya_host_cpu_name(OUT u8* buffer, u32 capacity);

/** Physical RAM installed. False when the OS will not say. */
NYA_API b8 nya_host_memory_total_bytes(OUT u64* out_bytes);

/**
 * Video memory on the display adapter. False when the OS will not say, which is the common case: only
 * the amdgpu and nvidia kernel drivers publish it on Linux, and nothing outside DXGI does on Windows.
 * */
NYA_API b8 nya_host_gpu_memory_total_bytes(OUT u64* out_bytes);
