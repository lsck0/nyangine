/**
 * @file testing.h
 *
 * The test facilities, compiled only under NYA_TESTING so a shipping build carries none of them.
 *
 * base_test.h is separate and stays in base: crash catching and soft assertions are what an assertion
 * needs, not what a test harness needs, and base is where assertions live.
 * */
#pragma once

#ifdef NYA_TESTING
#include "nyangine/testing/testing_property.h"
#include "nyangine/testing/testing_simulation.h"
/**/
#include "nyangine/testing/testing_actions.h"
#include "nyangine/testing/testing_session.h"
/**/
#include "nyangine/testing/testing_agent.h"
#endif
