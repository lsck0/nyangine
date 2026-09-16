/**
 * @file pp/stale.h
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Newest modification time across `paths`, or 0 if none of them exist.
 * */
u64 nya_pp_newest(NYA_ConstCString* paths, NYA_ConstCString extension) __attr_no_discard;

/**
 * Whether every file in `outputs` exists and is strictly newer than everything in `inputs`.
 * */
b8 nya_pp_is_current(NYA_ConstCString pass, NYA_ConstCString* inputs, NYA_ConstCString* outputs) __attr_no_discard;
