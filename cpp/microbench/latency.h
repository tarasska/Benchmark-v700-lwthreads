#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace microbench {

// Compile-time tuning. Bounds are nanoseconds; buckets grow geometrically.
inline constexpr bool LATENCY_ENABLED = true;
inline constexpr uint64_t LATENCY_MIN_NS = 10;
inline constexpr uint64_t LATENCY_MAX_NS = 1000000000;
inline constexpr double LATENCY_BUCKET_RATIO = 1.01;
static_assert(LATENCY_MIN_NS > 0 && LATENCY_MAX_NS > LATENCY_MIN_NS);
static_assert(LATENCY_BUCKET_RATIO > 1.0);

enum class LatencyOperation { Insert, Remove, Get, Contains, RangeQuery, Push, Pop, Count };
inline constexpr std::array<const char*, 7> LATENCY_OPERATION_NAMES = {
    "insert", "remove", "get", "contains", "range_query", "push", "pop"};

inline const std::vector<uint64_t>& latency_bounds() {
    static const auto bounds = [] {
        std::vector<uint64_t> result{LATENCY_MIN_NS};
        while (result.back() < LATENCY_MAX_NS) {
            const double next = std::ceil(result.back() * LATENCY_BUCKET_RATIO);
            result.push_back(next >= LATENCY_MAX_NS ? LATENCY_MAX_NS
                : std::max(result.back() + 1, static_cast<uint64_t>(next)));
        }
        return result;
    }();
    return bounds;
}

class LatencyHistogram {
    std::vector<uint64_t> buckets_;
    uint64_t count_ = 0;
    uint64_t below_min_ = 0;
    uint64_t above_max_ = 0;
    long double sum_ns_ = 0;

public:
    void prepare() { buckets_.assign(latency_bounds().size(), 0); }

    void record(uint64_t ns) {
        ++count_;
        sum_ns_ += ns; // Never clamp the mean, including out-of-range samples.
        if (ns < LATENCY_MIN_NS) ++below_min_;
        if (ns > LATENCY_MAX_NS) {
            ++above_max_;
            return;
        }
        const auto& bounds = latency_bounds();
        const auto index = std::lower_bound(bounds.begin(), bounds.end(), ns) - bounds.begin();
        ++buckets_[index];
    }

    void merge(const LatencyHistogram& other) {
        if (!other.count_) return;
        if (buckets_.empty()) prepare();
        for (size_t i = 0; i < buckets_.size(); ++i) buckets_[i] += other.buckets_[i];
        count_ += other.count_;
        below_min_ += other.below_min_;
        above_max_ += other.above_max_;
        sum_ns_ += other.sum_ns_;
    }

    uint64_t count() const { return count_; }
    uint64_t below_min() const { return below_min_; }
    uint64_t above_max() const { return above_max_; }
    double mean() const {
        return count_ ? static_cast<double>(sum_ns_ / count_)
                      : std::numeric_limits<double>::quiet_NaN();
    }

    // Nearest-rank percentile, reported as the bucket's upper bound.
    // NaN means no samples or a percentile in the unbounded overflow bucket.
    double percentile(unsigned percent) const {
        if (!count_ || percent == 0 || percent > 100)
            return std::numeric_limits<double>::quiet_NaN();
        const uint64_t rank = (count_ / 100) * percent
            + ((count_ % 100) * percent + 99) / 100;
        uint64_t cumulative = 0;
        for (size_t i = 0; i < buckets_.size(); ++i) {
            cumulative += buckets_[i];
            if (cumulative >= rank) return latency_bounds()[i];
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
};

using LatencyHistograms = std::array<LatencyHistogram,
    static_cast<size_t>(LatencyOperation::Count)>;

} // namespace microbench
