// Test entry points, grouped by source file. Registered in src/tests/registry.c.
#ifndef PICOBENCH_TESTS_H
#define PICOBENCH_TESTS_H

#include "core/sequencer.h"

// selftest.c - infrastructure self-checks
test_result_t test_timing_counter(test_ctx_t *ctx);
test_result_t test_clocks_readback(test_ctx_t *ctx);
test_result_t test_cpu_loop_smoke(test_ctx_t *ctx);
test_result_t test_debug_crash(test_ctx_t *ctx);

#endif
