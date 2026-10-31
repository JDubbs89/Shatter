#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace shatter::engine {

// Protects fixed-lifetime state shared with a real-time callback. Callback-side
// entry is allocation-free and never waits; close/wait/reopen belong to control
// threads and must bracket stream teardown and slot reuse.
class RealtimeAccessGate {
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "audio callback access gates require lock-free 32-bit atomics");
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
                  "audio callback controls require lock-free float and boolean atomics");

public:
    class Guard {
    public:
        explicit Guard(RealtimeAccessGate& gate) noexcept
            : gate_(gate), entered_(gate_.try_enter()) {}
        ~Guard() { if (entered_) gate_.leave(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        [[nodiscard]] explicit operator bool() const noexcept { return entered_; }

    private:
        RealtimeAccessGate& gate_;
        bool entered_;
    };

    [[nodiscard]] bool try_enter() noexcept
    {
        const std::uint32_t state = state_.load(std::memory_order_acquire);
        std::uint32_t observed = state;
        for (unsigned attempt = 0; attempt < max_entry_attempts; ++attempt) {
            if ((observed & closed_bit) != 0 || (observed & user_mask) == user_mask) return false;
            const auto desired = observed + 1;
            if (state_.compare_exchange_strong(observed, desired,
                    std::memory_order_acquire, std::memory_order_relaxed)) return true;
        }
        // Under sustained contention a callback skips this optional work rather
        // than spinning without a bound.
        return false;
    }

    void close() noexcept { state_.fetch_or(closed_bit, std::memory_order_acq_rel); }
    void wait_for_quiescence() const noexcept
    {
        while ((state_.load(std::memory_order_acquire) & user_mask) != 0) std::this_thread::yield();
    }
    void reopen() noexcept { state_.store(0, std::memory_order_release); }

private:
    static constexpr std::uint32_t closed_bit = 1U << 31U;
    static constexpr std::uint32_t user_mask = closed_bit - 1U;
    static constexpr unsigned max_entry_attempts = 3;

    void leave() noexcept { state_.fetch_sub(1, std::memory_order_release); }

    std::atomic<std::uint32_t> state_{0};
};

} // namespace shatter::engine
