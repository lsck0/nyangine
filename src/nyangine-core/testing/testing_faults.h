/**
 * @file testing_faults.h
 *
 * Simulation scenarios over the os fault interposer (os_fault.h): the engine's own I/O, lied to below the
 * engine, where the traffic set lies to it above. One plan is attached for the whole run, seeded from the
 * run, with the random stream seeded so keys and nonces replay; each action arms the rules it needs and
 * heals them before it returns, so no other action runs under a fault it did not ask for.
 *
 * ```c
 * nya_simulation_actions_add(run);
 * nya_simulation_traffic_add(run);
 * nya_simulation_faults_add(run);           // after traffic: it reuses the HTTP server traffic started
 *
 * u32 failures = nya_simulation_run(run);
 *
 * nya_simulation_faults_remove();
 * nya_simulation_traffic_remove();
 * nya_simulation_actions_remove();
 * ```
 *
 * ## The scenarios and their oracles
 *
 * - UDP: a real UDP transport pair over loopback, encrypted, on the simulated clock, while datagrams are
 *   dropped, doubled, held back and bit flipped. Every reliable message arrives once, in order and intact;
 *   a flipped datagram is never handed up; once the network heals, everything sent is delivered.
 * - HTTP: the real server under short sends and receives, EAGAIN, connection resets and refused accepts.
 *   Without a reset every request gets its whole answer, byte for byte; with one, any answer that arrives
 *   is whole, nothing is a 5xx, and no slot is held afterwards. A slowloris peer dribbling a header is cut
 *   off at the head bound while honest requests keep being answered. A starved exchange is a 503.
 * - Disk: a save written through a full disk, short writes, a failing fsync and a failing rename reads
 *   back as the old save or the new one, never a mix, and leaves no temporary behind; an append-only
 *   journal under the same faults holds every acknowledged line, whole and in order.
 *
 * The UDP pair runs on simulated time only while one of these actions runs, and nothing else touches it,
 * so its clock never mixes with the host's.
 * */
#pragma once

#include "nyangine-std/os/os_fault.h"

// the interposer is Linux's, so the scenarios over it are too.
#if defined(NYA_TESTING) && NYA_OS_FAULT_ENABLED

#include "nyangine-core/testing/testing_simulation.h"

/** Attaches the run's fault plan and registers the scenarios. A scenario whose resource is missing (no HTTP server, no save root) is not registered. */
NYA_API void nya_simulation_faults_add(NYA_SimulationRun* run);

/** Tears the scenarios down and detaches the plan. A no-op when nothing was added. */
NYA_API void nya_simulation_faults_remove(void);

#endif // NYA_TESTING && NYA_OS_FAULT_ENABLED
