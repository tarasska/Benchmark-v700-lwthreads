#include "latency.h"

#include <cassert>
#include <iostream>
#include <random>
#include <thread>

using namespace microbench;

int main() {
    LatencyHistogram empty;
    assert(std::isnan(empty.mean()));
    assert(std::isnan(empty.percentile(99)));

    // Compare merged worker histograms with exact sorted samples, including
    // uneven worker populations (averaging worker percentiles would be wrong).
    std::vector<uint64_t> samples;
    std::mt19937_64 random(17);
    long double sum = 0;
    for (size_t i = 0; i < 100000; ++i) {
        const uint64_t value = LATENCY_MIN_NS + random() % (LATENCY_MAX_NS - LATENCY_MIN_NS);
        samples.push_back(value);
        sum += value;
    }
    LatencyHistogram first, second;
    first.prepare();
    second.prepare();
    std::thread a([&] { for (size_t i = 0; i < 1000; ++i) first.record(samples[i]); });
    std::thread b([&] { for (size_t i = 1000; i < samples.size(); ++i) second.record(samples[i]); });
    a.join();
    b.join();
    LatencyHistogram merged;
    merged.merge(first);
    merged.merge(second);
    merged.merge(empty);
    assert(merged.count() == samples.size());
    assert(std::abs(merged.mean() - static_cast<double>(sum / samples.size())) < 0.001);
    std::sort(samples.begin(), samples.end());
    for (unsigned percent : {95u, 99u}) {
        const auto exact = samples[samples.size() * percent / 100 - 1];
        assert(merged.percentile(percent) >= exact);
        assert(merged.percentile(percent) <= exact * LATENCY_BUCKET_RATIO + 1);
    }

    // Exact bounds, underflow and overflow. Overflow must not bias the mean
    // or silently become a percentile of one second.
    LatencyHistogram edges;
    edges.prepare();
    edges.record(0);
    edges.record(LATENCY_MIN_NS);
    assert(edges.below_min() == 1);
    assert(edges.percentile(99) == LATENCY_MIN_NS);
    edges.record(LATENCY_MAX_NS);
    assert(edges.percentile(99) == LATENCY_MAX_NS);
    edges.record(2 * LATENCY_MAX_NS);
    assert(edges.above_max() == 1);
    assert(std::isnan(edges.percentile(95)));
    assert(edges.mean() == (3.0 * LATENCY_MAX_NS + LATENCY_MIN_NS) / 4);

    // Nearest-rank boundary: exactly one percent overflowing leaves p99 finite.
    LatencyHistogram tail;
    tail.prepare();
    for (int i = 0; i < 99; ++i) tail.record(100);
    tail.record(LATENCY_MAX_NS + 1);
    assert(tail.percentile(99) >= 100 && tail.percentile(99) <= 102);
    tail.record(LATENCY_MAX_NS + 1);
    assert(std::isnan(tail.percentile(99)));

    // Every bucket upper boundary must belong to that bucket, not the next.
    for (const auto bound : latency_bounds()) {
        LatencyHistogram single;
        single.prepare();
        single.record(bound);
        assert(single.percentile(95) == bound);
    }
    std::cout << "Latency tests passed; " << latency_bounds().size()
              << " buckets, " << latency_bounds().size() * sizeof(uint64_t)
              << " bytes per histogram\n";
}
