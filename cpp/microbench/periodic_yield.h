#pragma once

#include <cstdint>

#ifndef BENCH_YIELD_EVERY
#define BENCH_YIELD_EVERY 0
#endif
static_assert(BENCH_YIELD_EVERY >= 0 && BENCH_YIELD_EVERY <= 2147483647,
              "BENCH_YIELD_EVERY must be between 0 and 2147483647");

namespace microbench {

// One counter per logical worker, not per OS thread. No modulo on the hot path.
template <uint32_t Interval = BENCH_YIELD_EVERY>
class PeriodicYield {
    uint32_t completed_ = 0;

public:
    template <typename Yield>
    void after_operation(Yield&& yield) {
        if constexpr (Interval > 0) {
            if (++completed_ == Interval) {
                completed_ = 0;
                yield();
            }
        }
    }
};

} // namespace microbench
