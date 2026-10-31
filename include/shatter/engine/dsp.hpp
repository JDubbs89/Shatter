#pragma once

#include <cstddef>
#include <cstdint>

namespace shatter::engine::dsp {

struct GainRamp {
    float start_gain = 1.0F;
    float gain_increment = 0.0F;
};

void scale_ramp(float* samples, std::size_t frames, std::size_t channels,
                GainRamp gain_ramp) noexcept;

void accumulate_ramp(float* output, const float* input, std::size_t frames,
                     std::size_t channels, GainRamp gain_ramp) noexcept;

void accumulate_with_gain_ramps(float* output, const float* input, std::size_t frames,
                                std::size_t channels, GainRamp app_gain_ramp,
                                GainRamp send_gain_ramp) noexcept;

void apply_fade(float* samples, std::size_t frames, std::size_t channels,
                float& gain, std::uint32_t& remaining_frames,
                std::uint32_t total_frames) noexcept;

} // namespace shatter::engine::dsp
