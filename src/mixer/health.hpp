#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace shatter::mixer {

enum class HealthState { running, degraded, failed_open };

inline HealthState health_state(bool backend_started, bool output_connected,
                                std::uint32_t unrouted_streams) noexcept
{
    if (!backend_started) return HealthState::failed_open;
    if (!output_connected || unrouted_streams != 0) return HealthState::degraded;
    return HealthState::running;
}

struct XrunSnapshot {
    std::uint64_t input_overrun_events = 0;
    std::uint64_t input_overrun_frames = 0;
    std::uint64_t playback_underrun_events = 0;
    std::uint64_t playback_underrun_frames = 0;
};

class XrunCounters {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "real-time xrun counters require lock-free 64-bit atomics");

public:
    void record_input_overrun(std::uint64_t frames) noexcept
    {
        if (frames == 0) return;
        input_overrun_events_.fetch_add(1, std::memory_order_relaxed);
        input_overrun_frames_.fetch_add(frames, std::memory_order_relaxed);
    }

    void record_playback_underrun(std::uint64_t frames) noexcept
    {
        if (frames == 0) return;
        playback_underrun_events_.fetch_add(1, std::memory_order_relaxed);
        playback_underrun_frames_.fetch_add(frames, std::memory_order_relaxed);
    }

    XrunSnapshot snapshot() const noexcept
    {
        return {
            input_overrun_events_.load(std::memory_order_relaxed),
            input_overrun_frames_.load(std::memory_order_relaxed),
            playback_underrun_events_.load(std::memory_order_relaxed),
            playback_underrun_frames_.load(std::memory_order_relaxed),
        };
    }

private:
    std::atomic<std::uint64_t> input_overrun_events_{0};
    std::atomic<std::uint64_t> input_overrun_frames_{0};
    std::atomic<std::uint64_t> playback_underrun_events_{0};
    std::atomic<std::uint64_t> playback_underrun_frames_{0};
};

inline const char* health_state_name(HealthState state) noexcept
{
    switch (state) {
    case HealthState::running: return "running";
    case HealthState::degraded: return "degraded";
    case HealthState::failed_open: return "failed-open";
    }
    return "failed-open";
}

} // namespace shatter::mixer
