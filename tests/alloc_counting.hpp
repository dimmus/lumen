#pragma once

// Allocation-counting tests replace the global operator new/delete to count what a
// steady frame allocates. Under ThreadSanitizer that replacement cannot exist:
// libclang_rt.tsan_cxx defines the global operators itself, so a second definition is
// a duplicate-symbol link error — and TSan's own per-access bookkeeping would pollute
// any count anyway.
//
// Tests that include this compile their counting operators only when
// LUMEN_TEST_ALLOC_COUNTING is 1 (plain and ASan builds; ASan's runtime tolerates
// user-replaced operators, so the zero-allocation assertions stay real evidence
// there) and print an explicit skip line at those assertions under TSan rather than
// passing them vacuously.
#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
#define LUMEN_TEST_ALLOC_COUNTING 0
#else
#define LUMEN_TEST_ALLOC_COUNTING 1
#endif
