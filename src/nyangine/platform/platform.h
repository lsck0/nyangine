#pragma once

#include "nyangine/platform/clock/clock.h"
#include "nyangine/platform/command/command.h"
#include "nyangine/platform/filesystem/filesystem.h"
#include "nyangine/platform/memory/memory.h"
#include "nyangine/platform/signals/signals.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * HOST
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Hardware threads available to this process, or 1 when that cannot be determined.
 * */
NYA_API u32 nya_platform_processor_count(void) __attr_no_discard;
