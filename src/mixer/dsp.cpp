#include "mixer/dsp.hpp"

namespace shatter::mixer::dsp {

void scale_ramp(float* samples, std::size_t frames, std::size_t channels,
                GainRamp gain_ramp) noexcept
{
    float gain = gain_ramp.start_gain;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            samples[frame * channels + channel] *= gain;
        }
        gain += gain_ramp.gain_increment;
    }
}

void accumulate_ramp(float* output, const float* input, std::size_t frames,
                     std::size_t channels, GainRamp gain_ramp) noexcept
{
    float gain = gain_ramp.start_gain;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto sample = frame * channels + channel;
            output[sample] += input[sample] * gain;
        }
        gain += gain_ramp.gain_increment;
    }
}

void accumulate_with_gain_ramps(float* output, const float* input, std::size_t frames,
                                std::size_t channels, GainRamp app_gain_ramp,
                                GainRamp send_gain_ramp) noexcept
{
    float app_gain = app_gain_ramp.start_gain;
    float send_gain = send_gain_ramp.start_gain;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float gain = app_gain * send_gain;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto sample = frame * channels + channel;
            output[sample] += input[sample] * gain;
        }
        app_gain += app_gain_ramp.gain_increment;
        send_gain += send_gain_ramp.gain_increment;
    }
}

void apply_fade(float* samples, std::size_t frames, std::size_t channels,
                float& gain, std::uint32_t& remaining_frames,
                std::uint32_t total_frames) noexcept
{
    const float fade_step = total_frames == 0 ? 0.0F : 1.0F / static_cast<float>(total_frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float frame_gain = remaining_frames == 0 ? 0.0F : gain;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            samples[frame * channels + channel] *= frame_gain;
        }
        if (remaining_frames > 0) {
            gain = gain > fade_step ? gain - fade_step : 0.0F;
            --remaining_frames;
        }
    }
}

} // namespace shatter::mixer::dsp
