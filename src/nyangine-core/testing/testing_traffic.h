/**
 * @file testing_traffic.h
 *
 * Captured traffic as simulation faults. A replicate server and client talk over an in-process wire, the HTTP
 * server answers requests over a real socket, and what each of them really received is replayed dropped,
 * duplicated, reordered, delayed, bit flipped, truncated and spliced into another session.
 *
 * ```c
 * nya_simulation_actions_add(run);
 * nya_simulation_traffic_add(run);          // on top of the engine's set; see testing_actions.h
 *
 * u32 failures = nya_simulation_run(run);
 *
 * nya_simulation_traffic_remove();
 * nya_simulation_actions_remove();
 * ```
 *
 * ## The oracle
 *
 * - Whatever a peer sends is refused or handled, never an assertion: an assert a peer can reach is a denial of
 *   service, so the engine's own assertions are the check.
 * - A session only the network touched (drop, duplicate, reorder, delay) converges once the faults stop: the
 *   client holds a replica of every entity the server replicates, where the server has it.
 * - A snapshot cut short on its way to the client moves the client's rejected count.
 * - The HTTP server answers or closes every connection, holds no slot afterwards, and never answers garbage with
 *   a 5xx, which would say the fault was its own.
 *
 * Bytes flipped, cut or spliced into the client's inbox are a hostile server, which the client survives but need
 * not agree with, so such a session is only checked for surviving it.
 *
 * ## Two worlds
 *
 * The server and the client each tick in a world of their own, swapped in around every call. In one world a
 * replica carries the replicated flag, so the server would replicate the client's copies back to it.
 * */
#pragma once

#ifdef NYA_TESTING

#include "nyangine-core/testing/testing_simulation.h"

/** Opens the first session, starts the HTTP server when a port is free, and registers the actions and faults. */
NYA_API void nya_simulation_traffic_add(NYA_SimulationRun* run);

/** Closes the session and stops the HTTP server. A no-op when nothing was added. */
NYA_API void nya_simulation_traffic_remove(void);

#endif // NYA_TESTING
