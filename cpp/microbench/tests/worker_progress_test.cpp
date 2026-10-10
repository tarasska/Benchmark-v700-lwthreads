#include "worker_progress.h"

#include <cassert>
#include <cmath>
#include <iostream>

using microbench::summarize_worker_progress;

int main() {
    const auto equal = summarize_worker_progress({100, 100, 100, 100}, 4);
    assert(equal.zero_progress_percent == 0);
    assert(equal.jain_fairness == 1);

    // The user's four busy workers out of sixteen, with omitted trailing zeros.
    const auto starving = summarize_worker_progress({100, 100, 100, 100}, 16);
    assert(starving.worker_count == 16);
    assert(starving.zero_progress_workers == 12);
    assert(starving.zero_progress_percent == 75);
    assert(starving.jain_fairness == 0.25);
    const auto padded = summarize_worker_progress(
        {100, 100, 100, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 16);
    assert(padded.jain_fairness == starving.jain_fairness);
    assert(padded.zero_progress_percent == starving.zero_progress_percent);
    const auto interleaved = summarize_worker_progress({0, 100, 0, 100, 0, 100, 0, 100}, 16);
    assert(interleaved.jain_fairness == starving.jain_fairness);

    const auto skewed = summarize_worker_progress({1000000, 1, 1, 1}, 4);
    assert(skewed.zero_progress_percent == 0);
    assert(skewed.jain_fairness > 0.25 && skewed.jain_fairness < 0.251);

    const auto idle = summarize_worker_progress({}, 16);
    assert(idle.zero_progress_workers == 16);
    assert(idle.zero_progress_percent == 100);
    assert(std::isnan(idle.jain_fairness));
    const auto no_workers = summarize_worker_progress({}, 0);
    assert(std::isnan(no_workers.zero_progress_percent));
    assert(std::isnan(no_workers.jain_fairness));

    // Squaring and summing must not overflow 64-bit integer arithmetic.
    const auto large = summarize_worker_progress({UINT64_MAX, UINT64_MAX}, 2);
    assert(large.jain_fairness == 1);
    bool rejected = false;
    try { summarize_worker_progress({1, 2}, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    std::cout << "Worker progress tests passed\n";
}
