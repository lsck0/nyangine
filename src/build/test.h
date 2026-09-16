/**
 * @file test.h
 * */
#pragma once

#include "nyangine/nyangine.h"

/*
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 * FUNCTIONS AND MACROS
 * ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
 */

/**
 * Builds and runs the tests, optionally filtered.
 * */
void test_runner(NYA_ArgCommand* command);

/**
 * The same, with the tests compiled under source based coverage instrumentation.
 * */
void coverage_runner(NYA_ArgCommand* command);
