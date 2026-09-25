#pragma once

#include "nyangine-std/platform/host/host.h"
#include "nyangine-std/platform/ipc/ipc.h"
#include "nyangine-std/platform/signals/signals.h"
#include "nyangine-std/platform/terminal/terminal.h"
#include "nyangine-std/platform/web/web.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * HOST
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Hardware threads available to this process, or 1 when that cannot be determined.
 * */
NYA_API u32 nya_platform_processor_count(void) __attr_no_discard;
