#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace microbench {

struct WorkerProgress {
    size_t worker_count;
    size_t zero_progress_workers;
    double zero_progress_percent;
    double jain_fairness;
};

// GSTATS drops trailing zero counters from JSON. Include those workers using
// the configured test-phase count, never the serialized array's length.
inline WorkerProgress summarize_worker_progress(const std::vector<uint64_t>& operations,
                                                size_t worker_count) {
    if (operations.size() > worker_count)
        throw std::invalid_argument("More operation counters than configured workers");

    size_t active = 0;
    long double sum = 0;
    long double sum_squares = 0;
    for (uint64_t count : operations) {
        active += count != 0;
        const long double value = count;
        sum += value;
        sum_squares += value * value;
    }
    const double undefined = std::numeric_limits<double>::quiet_NaN();
    return {worker_count, worker_count - active,
            worker_count ? 100.0 * (worker_count - active) / worker_count : undefined,
            sum_squares > 0 ? std::min(1.0, static_cast<double>(
                sum * sum / (static_cast<long double>(worker_count) * sum_squares))) : undefined};
}

} // namespace microbench
