/**
 * @file bench.h
 *
 * The bench command: finds every benchmark, builds it *without* sanitizers, runs it.
 *
 * Separate from the test command for one reason that matters: a test is compiled at -O0 under four
 * sanitizers, and a benchmark compiled that way measures the sanitizers. See FLAGS_BENCH.
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Builds and runs the benchmarks under bench/, optionally filtered by substring.
 *
 * Serial, always: two benchmarks running at once contend for cache and cores and both report numbers
 * that mean nothing. This is the opposite of the test runner's reasoning, which parallelises the
 * compile precisely because the result does not depend on timing.
 * */
void bench_runner(NYA_ArgCommand* command);
