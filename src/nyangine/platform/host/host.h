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

/** Longest system name written, terminator included. Long enough for a distribution's PRETTY_NAME. */
#define NYA_HOST_DISTRIBUTION_NAME_MAX 96

/** Longest kernel name written, terminator included. `uname`'s fields are 65 bytes each by POSIX. */
#define NYA_HOST_KERNEL_NAME_MAX 144

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

/**
 * Writes which system this is, null terminated and truncated to `capacity`. Always writes.
 *
 * On Linux the distribution's own `PRETTY_NAME` from `/etc/os-release`, because "Linux" in a bug report
 * is not an answer — a Wayland compositor bug on Arch and a glibc version on Debian oldstable are
 * different reports. On Windows the build. Falls back to the platform's name when nothing says.
 * */
NYA_API void nya_host_distribution_name(OUT u8* buffer, u32 capacity);

/**
 * Writes the kernel, null terminated and truncated to `capacity`. Always writes.
 *
 * On Linux `uname`'s sysname and release, so "Linux 6.2.6-arch2-1" — the number a driver or a syscall
 * bug is reported against. On Windows there is no kernel version separate from the build that
 * nya_host_distribution_name already prints, and this says so rather than repeating it.
 * */
NYA_API void nya_host_kernel_name(OUT u8* buffer, u32 capacity);

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * ENVIRONMENT
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Sets `name` to `value` in this process's environment, replacing what was there. False when the OS
 * refuses, as it does for a name holding `=`. Children started afterwards inherit it.
 *
 * Not thread safe on any OS: another thread reading the environment at the same time may see it torn.
 * Meant for tests and start up, before threads exist.
 * */
NYA_API b8 nya_host_environment_add(NYA_ConstCString name, NYA_ConstCString value) __attr_no_discard;

/** Removes `name` from this process's environment. Removing a name that is not set succeeds. */
NYA_API b8 nya_host_environment_remove(NYA_ConstCString name) __attr_no_discard;
