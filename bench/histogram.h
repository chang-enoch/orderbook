#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace bench {

using Clock = std::chrono::steady_clock;

// Keeps the compiler from eliding work whose result is otherwise unused.
template <class T>
inline void DoNotOptimize(const T& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

inline void ClobberMemory() { asm volatile("" : : : "memory"); }

// Granularity of the system clock, in nanoseconds: the smallest non-zero gap
// between two back-to-back reads. On Apple Silicon this is ~41.7ns (a 24MHz
// timebase), which is a hard floor for userspace timing -- there is no
// finer-grained counter available without kernel support.
//
// This matters for how the results are read. Individual samples are quantized
// to multiples of this tick, so p50/p90 for the fastest operations are
// resolution-limited (a p50 of one tick means "somewhere below one tick", not
// "41.7ns"). The tails, where operations cost hundreds of nanoseconds, span
// many ticks and are measured properly -- and those are what this project is
// about. The mean stays informative at any percentile because the clock is
// asynchronous to the work, so the quantization error dithers out.
//
// Deliberately not subtracted from samples: the call overhead is smaller than
// one tick, so any estimate of it is itself quantization noise, and subtracting
// a noisy tick from every sample shifts the whole distribution.
inline std::uint64_t ClockTickNs() {
    constexpr int kIters = 200'000;
    std::uint64_t best = ~0ull;
    for (int i = 0; i < kIters; ++i) {
        const auto a = Clock::now();
        const auto b = Clock::now();
        const auto d = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
        if (d > 0 && d < best) best = d;
    }
    return best == ~0ull ? 1 : best;
}

struct Stats {
    std::uint64_t count = 0;
    double        mean  = 0;
    std::uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
};

// Per-op-type latency samples. Storage is preallocated up front; nothing
// allocates, sorts, or prints while a timed region is open.
class Recorder {
public:
    explicit Recorder(std::size_t capacityPerOp) {
        for (auto& v : samples_) v.reserve(capacityPerOp);
    }

    void Record(int op, std::uint64_t ns) { samples_[op].push_back(ns); }

    void Reset() {
        for (auto& v : samples_) v.clear();
        all_.clear();
    }

    // Sorts, exactly once. Every Summarize call after this is a pure read --
    // calling them in any order or more than once gives the same answer.
    void Finalize() {
        for (auto& v : samples_) std::sort(v.begin(), v.end());
        all_.clear();
        std::size_t total = 0;
        for (const auto& v : samples_) total += v.size();
        all_.reserve(total);
        for (const auto& v : samples_) all_.insert(all_.end(), v.begin(), v.end());
        std::sort(all_.begin(), all_.end());
    }

    Stats Summarize(int op) const { return From(samples_[op]); }
    Stats SummarizeAll() const { return From(all_); }

private:
    static Stats From(const std::vector<std::uint64_t>& sorted) {
        Stats s;
        if (sorted.empty()) return s;
        double sum = 0;
        for (auto x : sorted) sum += static_cast<double>(x);
        s.count = sorted.size();
        s.mean  = sum / static_cast<double>(sorted.size());
        s.p50   = Percentile(sorted, 0.50);
        s.p90   = Percentile(sorted, 0.90);
        s.p99   = Percentile(sorted, 0.99);
        s.p999  = Percentile(sorted, 0.999);
        s.p9999 = Percentile(sorted, 0.9999);
        s.max   = sorted.back();
        return s;
    }

    // Nearest-rank on the already-sorted, already-overhead-adjusted vector.
    static std::uint64_t Percentile(const std::vector<std::uint64_t>& sorted,
                                    double q) {
        if (sorted.empty()) return 0;
        std::size_t idx =
            static_cast<std::size_t>(q * static_cast<double>(sorted.size()));
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
    }

    std::vector<std::uint64_t> samples_[3];  // indexed by Op
    std::vector<std::uint64_t> all_;         // merged, built by Finalize()
};

}  // namespace bench
