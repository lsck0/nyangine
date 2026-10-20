/**
 * bench_scope.c with the measuring on, as in a debug, dev or test build, but at -O2 so the figure is the
 * instrumentation's rather than the unoptimised build's.
 **/

#define NYA_SCOPE_FORCE_MEASURED

#include "bench/bench_scope.c"
