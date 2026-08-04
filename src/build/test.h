/**
 * @file test.h
 *
 * The test command: finds every test, builds it, runs it.
 *
 * A command handler rather than a set of NYA_BuildRules, because which tests exist is only known by
 * walking tests/ and the rules have to be built from what is found. Everything it produces is still
 * an ordinary rule, so a test compile is cached and reported like any other target.
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
 *
 * With no arguments it runs everything under tests/. With arguments it runs the tests whose path
 * contains any of them, so `./build run test string` picks up test_string and test_string_edges
 * alike.
 *
 * Stops at the first failure: the runner throws, which leaves the build reporting the rule that
 * failed rather than burying it under the tests that came after.
 * */
void test_runner(NYA_ArgCommand* command);
