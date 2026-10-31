#include "shatter/meter/meter_bus.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace shatter::meter {
namespace {
constexpr float kFloorDbfs = -120.0F;

std::uint64_t pack_reading(float peak, float rms) noexcept
{
    return (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(peak)) << 32U) |
        std::bit_cast<std::uint32_t>(rms);
}
}

void PeakRmsMeter::observe(const float* samples, std::size_t count) noexcept
{
    if (samples == nullptr) return;
    for (std::size_t index = 0; index < count; ++index) observe_sample(samples[index]);
}

void PeakRmsMeter::observe_ramped(const float* samples, std::size_t frames, std::size_t channels,
                                  float gain, float gain_increment) noexcept
{
    if (samples == nullptr) return;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel)
            observe_sample(samples[frame * channels + channel] * gain);
        gain += gain_increment;
    }
}

void PeakRmsMeter::observe_sample(float value) noexcept
{
    const float sample = std::isfinite(value) ? value : 0.0F;
    window_peak_ = std::max(window_peak_, std::abs(sample));
    window_square_sum_ += static_cast<double>(sample) * sample;
    ++window_samples_;
    if (window_samples_ == samples_per_window) {
        published_.store(pack_reading(window_peak_,
            static_cast<float>(std::sqrt(window_square_sum_ / window_samples_))),
            std::memory_order_release);
        window_peak_ = 0.0F;
        window_square_sum_ = 0.0;
        window_samples_ = 0;
    }
}

Reading PeakRmsMeter::reading() const noexcept
{
    const auto packed = published_.load(std::memory_order_acquire);
    const auto peak_bits = static_cast<std::uint32_t>(packed >> 32U);
    const auto rms_bits = static_cast<std::uint32_t>(packed);
    return {std::bit_cast<float>(peak_bits), std::bit_cast<float>(rms_bits)};
}

void PeakRmsMeter::reset() noexcept
{
    window_peak_ = 0.0F;
    window_square_sum_ = 0.0;
    window_samples_ = 0;
    published_.store(pack_reading(0.0F, 0.0F), std::memory_order_release);
}

float to_dbfs(float linear) noexcept
{
    if (!(linear > 0.0F) || !std::isfinite(linear)) return kFloorDbfs;
    return std::max(kFloorDbfs, 20.0F * std::log10(linear));
}

} // namespace shatter::meter
