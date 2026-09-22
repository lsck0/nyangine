#pragma once

#include "nyangine/platform/host/host.h"
#include "nyangine/platform/ipc/ipc.h"
#include "nyangine/platform/signals/signals.h"
#include "nyangine/platform/terminal/terminal.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * HOST
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Hardware threads available to this process, or 1 when that cannot be determined.
 * */
NYA_API u32 nya_platform_processor_count(void) __attr_no_discard;
