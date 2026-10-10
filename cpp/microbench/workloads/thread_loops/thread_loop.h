//
// Created by Ravil Galiev on 21.07.2023.
//
#pragma once

#include "workloads/stop_condition/stop_condition.h"
#include "globals_t.h"
#include "latency.h"
#include "periodic_yield.h"
#if BENCH_YIELD_EVERY > 0
#include <nasl/yield.hpp>
#endif

namespace microbench::workload {

using K = int64_t;

class ThreadLoop {
protected:
    K garbage = 0;
    VALUE_TYPE NO_VALUE;
    PeriodicYield<> externalYield;

public:
    size_t threadId;
    globals_t* g;
    StopCondition* stopCondition;

    // Owned by the logical worker, including fibers that migrate between OS threads.
    bool measureLatency = false;
    LatencyHistograms latency;

    template <typename Operation>
    auto measure_operation(LatencyOperation kind, Operation&& operation) {
        if constexpr (!LATENCY_ENABLED) return operation();
        if (!measureLatency) return operation();
        const auto start = std::chrono::steady_clock::now();
        auto result = operation();
        const auto end = std::chrono::steady_clock::now();
        latency[static_cast<size_t>(kind)].record(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
        return result;
    }

    // Called after adapter timing and operation accounting, in every phase.
    void after_operation() {
#if BENCH_YIELD_EVERY > 0
        externalYield.after_operation([] { nasl::core::yield(); });
#endif
    }

    ThreadLoop() = default;

    virtual void run() = 0;

    virtual void step() = 0;

    virtual ~ThreadLoop() = default;
};

}  // namespace microbench::workload