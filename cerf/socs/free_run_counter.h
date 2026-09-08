#pragma once

#include <atomic>
#include <cstdint>

namespace cerf_free_run_counter {

constexpr uint64_t Gcd(uint64_t a, uint64_t b) {
    return b == 0u ? a : Gcd(b, a % b);
}

constexpr uint64_t kNsPerSec = 1000000000ull;

struct TickScale {
    uint64_t ns_per_unit = 1u;
    uint64_t tk_per_unit = 1u;

    constexpr TickScale() = default;
    constexpr explicit TickScale(uint32_t hz)
        : ns_per_unit(kNsPerSec / Gcd(kNsPerSec, hz)),
          tk_per_unit(hz / Gcd(kNsPerSec, hz)) {}

    constexpr uint32_t NsToTicks(int64_t ns) const {
        return static_cast<uint32_t>(
            static_cast<uint64_t>(ns) * tk_per_unit / ns_per_unit);
    }
    constexpr int64_t TicksToNs(uint32_t ticks) const {
        return static_cast<int64_t>(
            static_cast<uint64_t>(ticks) * ns_per_unit / tk_per_unit);
    }
    constexpr int64_t WrapNs() const {
        return static_cast<int64_t>(4294967296ull * ns_per_unit / tk_per_unit);
    }
    constexpr int64_t NextMatchNs(uint32_t match, uint32_t count_now,
                                  int64_t now_ns) const {
        const uint32_t ticks = match - count_now;
        return now_ns + (ticks != 0u ? TicksToNs(ticks) : WrapNs());
    }
};

class FreeRunCounter {
public:
    void Set(int64_t now_ns, uint32_t count) {
        seq_.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        anchor_ns_.store(now_ns, std::memory_order_relaxed);
        anchor_cnt_.store(count, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        seq_.fetch_add(1, std::memory_order_relaxed);
    }

    uint32_t At(int64_t now_ns, const TickScale& scale) const {
        for (;;) {
            const uint32_t s0 = seq_.load(std::memory_order_acquire);
            if ((s0 & 1u) != 0u) continue;
            const int64_t  ns  = anchor_ns_.load(std::memory_order_relaxed);
            const uint32_t cnt = anchor_cnt_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) != s0) continue;
            return cnt + scale.NsToTicks(now_ns - ns);
        }
    }

private:
    std::atomic<uint32_t> seq_{0};
    std::atomic<int64_t>  anchor_ns_{0};
    std::atomic<uint32_t> anchor_cnt_{0};
};

}  // namespace cerf_free_run_counter
