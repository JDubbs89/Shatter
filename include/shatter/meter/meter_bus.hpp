#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace shatter::meter {

struct Reading {
    float peak = 0.0F;
    float rms = 0.0F;
};

// Single-writer meter for one audio source. observe() is bounded by the input
// buffer size and performs no allocation or locking. Readers only see the last
// completed 100 ms window.
class PeakRmsMeter {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "real-time meters require lock-free 64-bit atomics");

public:
    static constexpr std::size_t samples_per_window = 4800 * 2;

    void observe(const float* interleaved_samples, std::size_t sample_count) noexcept;
    void observe_ramped(const float* interleaved_samples, std::size_t frames, std::size_t channels,
                        float start_gain, float gain_increment) noexcept;
    Reading reading() const noexcept;
    // Call only while the meter's audio callback cannot be running.
    void reset() noexcept;

private:
    float window_peak_ = 0.0F;
    double window_square_sum_ = 0.0;
    std::size_t window_samples_ = 0;
    std::atomic<std::uint64_t> published_{0};

    void observe_sample(float sample) noexcept;
};

float to_dbfs(float linear) noexcept;

} // namespace shatter::meter
