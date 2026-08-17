#pragma once

#include "nyangine/platform/clock/clock.h"
#include "nyangine/platform/command/command.h"
#include "nyangine/platform/filesystem/filesystem.h"
#include "nyangine/platform/signals/signals.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * HOST
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Hardware threads available to this process, or 1 when that cannot be determined.
 *
 * The count of things that can run at once, not of physical cores — it is what a "how many jobs
 * should I start" question actually wants. Never returns 0, so it can be divided by and used as a
 * loop bound without a guard at every call site.
 * */
NYA_API u32 nya_platform_processor_count(void) __attr_no_discard;
