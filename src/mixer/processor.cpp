#include "mixer/processor.hpp"
#include "mixer/dsp.hpp"

#include <algorithm>
#include <array>

namespace shatter::mixer {
namespace {
constexpr auto kSampleRate = sample_rate;
constexpr auto kChannels = channels;
constexpr auto kShutdownFadeFrames = sample_rate / 4;
} // namespace

Processor::OutputGenerations Processor::output_generations() const noexcept
{
    OutputGenerations generations{};
    for (std::size_t index = 0; index < state_.outputs.size(); ++index)
        generations[index] = state_.outputs[index]->input_generation.load(std::memory_order_acquire);
    return generations;
}

void Processor::process_playback(PlaybackStream& stream, const float* samples, std::size_t frames,
                                  const OutputGenerations& output_generations,
                                  RuntimeProfiler::Scope& profile_scope) noexcept
{
    const float target = stream.muted.load(std::memory_order_relaxed)
        ? 0.0F : stream.target_gain.load(std::memory_order_relaxed);
    const float increment = frames == 0 ? 0.0F
        : (target - stream.smoothed_gain) / static_cast<float>(frames);
    stream.meter.observe_ramped(samples, frames, kChannels, stream.smoothed_gain, increment);
    std::size_t written = frames;
    const auto app_mask = state_.apps[stream.channel_index].output_mask.load(std::memory_order_acquire);
    const auto stream_mask = stream.output_mask.load(std::memory_order_acquire);
    const auto mask = app_mask | stream_mask;
    for (std::size_t output_index = 0; output_index < state_.outputs.size(); ++output_index) {
        if ((mask & (1U << output_index)) == 0) continue;
        OutputChannel& output = *state_.outputs[output_index];
        RealtimeAccessGate::Guard output_guard(output.input_gate);
        if (!output_guard) continue;
        if (output.input_generation.load(std::memory_order_acquire) != output_generations[output_index])
            continue;
        const auto current_app_mask = state_.apps[stream.channel_index].output_mask.load(std::memory_order_acquire);
        const auto current_stream_mask = stream.output_mask.load(std::memory_order_acquire);
        if (((current_app_mask | current_stream_mask) & (1U << output_index)) == 0) continue;
        if (output.linked.load(std::memory_order_acquire)) {
            if ((current_app_mask & (1U << output_index)) != 0) {
                written = std::min(written, stream.audio[output_index].write_with_gain_ramp(
                    samples, frames, stream.smoothed_gain, increment));
            }
            if ((current_stream_mask & (1U << output_index)) != 0) {
                written = std::min(written, stream.send_audio[output_index].write(samples, frames));
            }
        }
    }
    profile_scope.set_frames(frames, kSampleRate);
    if (mask != 0 && written < frames) {
        const auto dropped = frames - written;
        profile_scope.record_input_overrun(dropped);
        state_.xruns.record_input_overrun(dropped);
    }
    stream.smoothed_gain = target;
}

void Processor::process_capture(CaptureChannel& capture_channel, const float* samples, std::size_t frames,
                                 const OutputGenerations& output_generations,
                                 RuntimeProfiler::Scope& profile_scope) noexcept
{
    const float target = capture_channel.muted.load(std::memory_order_relaxed)
        ? 0.0F : capture_channel.target_gain.load(std::memory_order_relaxed);
    const float increment = frames == 0 ? 0.0F
        : (target - capture_channel.smoothed_gain) / static_cast<float>(frames);
    capture_channel.meter.observe_ramped(samples, frames, kChannels,
        capture_channel.smoothed_gain, increment);
    const auto mask = capture_channel.output_mask.load(std::memory_order_acquire);
    std::uint64_t dropped_frames = 0;
    for (std::size_t output_index = 0; output_index < state_.outputs.size(); ++output_index) {
        if ((mask & (1U << output_index)) == 0) continue;
        OutputChannel& output = *state_.outputs[output_index];
        RealtimeAccessGate::Guard output_guard(output.input_gate);
        if (!output_guard) continue;
        if (output.input_generation.load(std::memory_order_acquire) == output_generations[output_index] &&
            (capture_channel.output_mask.load(std::memory_order_acquire) & (1U << output_index)) != 0 &&
            output.linked.load(std::memory_order_acquire)) {
            const auto written = capture_channel.audio[output_index].write_with_gain_ramp(
                samples, frames, capture_channel.smoothed_gain, increment);
            dropped_frames += frames - written;
        }
    }
    if (dropped_frames != 0) {
        profile_scope.record_input_overrun(dropped_frames);
        state_.xruns.record_input_overrun(dropped_frames);
    }
    capture_channel.smoothed_gain = target;
    profile_scope.set_frames(frames, kSampleRate);
}

void Processor::process_output(std::size_t output_index, float* samples, std::size_t frames,
                                RuntimeProfiler::Scope& profile_scope) noexcept
{
    OutputChannel& output = *state_.outputs[output_index];
    const std::uint32_t output_bit = 1U << output_index;
    std::fill(samples, samples + frames * kChannels, 0.0F);
    std::array<float, 1024 * kChannels> channel_mix{};
    std::uint64_t playback_underrun_frames = 0;
    std::uint64_t latest_ring_frames = 0;
    const auto channel_count = state_.allocated_channels.load(std::memory_order_acquire);
    const auto stream_count = state_.allocated_streams.load(std::memory_order_acquire);
    for (std::size_t offset = 0; offset < frames; offset += 1024) {
        const auto block_frames = std::min<std::size_t>(1024, frames - offset);
        const auto output_offset = offset * kChannels;
        for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
            AppChannel& channel = state_.apps[channel_index];
            const bool assigned = (channel.output_mask.load(std::memory_order_acquire) & output_bit) != 0;
            std::fill(channel_mix.begin(), channel_mix.begin() + block_frames * kChannels, 0.0F);
            for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
                PlaybackStream& stream = *state_.streams[stream_index];
                if (!stream.active.load(std::memory_order_acquire) || stream.channel_index != channel_index) continue;
                RealtimeAccessGate::Guard stream_guard(stream.mixer_gate);
                if (!stream_guard || !stream.active.load(std::memory_order_acquire)) continue;
                auto& ring = stream.audio[output_index];
                const auto occupancy = ring.available_frames();
                latest_ring_frames = std::max(latest_ring_frames,
                    static_cast<std::uint64_t>(occupancy));
                profile_scope.record_ring_occupancy(occupancy);
                profile_scope.record_app_stream_ring_occupancy(occupancy);
                const auto consumed = ring.accumulate(channel_mix.data(), block_frames, 1.0F);
                profile_scope.record_unfilled_mix_frames(block_frames - consumed);
                if (assigned) playback_underrun_frames += block_frames - consumed;
            }
            const float target = channel.muted.load(std::memory_order_relaxed)
                ? 0.0F : channel.target_gain.load(std::memory_order_relaxed);
            float gain = channel.smoothed_gain[output_index];
            const float increment = block_frames == 0 ? 0.0F
                : (target - gain) / static_cast<float>(block_frames);
            if (assigned) {
                channel.meters[output_index].observe_ramped(channel_mix.data(), block_frames,
                    kChannels, gain, increment);
                dsp::accumulate_ramp(samples + output_offset, channel_mix.data(),
                                     block_frames, kChannels, {gain, increment});
            }
            channel.smoothed_gain[output_index] = target;

            for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
                PlaybackStream& stream = *state_.streams[stream_index];
                if (!stream.active.load(std::memory_order_acquire) || stream.channel_index != channel_index ||
                    (stream.output_mask.load(std::memory_order_acquire) & output_bit) == 0) continue;
                RealtimeAccessGate::Guard stream_guard(stream.mixer_gate);
                if (!stream_guard) continue;
                auto& ring = stream.send_audio[output_index];
                profile_scope.record_app_stream_ring_occupancy(ring.available_frames());
                const auto consumed = ring.read(channel_mix.data(), block_frames);
                profile_scope.record_unfilled_mix_frames(block_frames - consumed);
                playback_underrun_frames += block_frames - consumed;
                const float stream_target = stream.muted.load(std::memory_order_relaxed)
                    ? 0.0F : stream.target_gain.load(std::memory_order_relaxed);
                const float stream_gain = stream.smoothed_send_gain[output_index];
                const float stream_increment = block_frames == 0 ? 0.0F
                    : (stream_target - stream_gain) / static_cast<float>(block_frames);
                dsp::accumulate_with_gain_ramps(samples + output_offset, channel_mix.data(),
                    block_frames, kChannels, {gain, increment},
                    {stream_gain, stream_increment});
                stream.smoothed_send_gain[output_index] = stream_target;
            }
        }

        for (auto* capture : state_.captures) {
            auto& capture_channel = *capture;
            RealtimeAccessGate::Guard capture_guard(capture_channel.mixer_gate);
            if (!capture_guard || !capture_channel.in_use) continue;
            std::fill(channel_mix.begin(), channel_mix.begin() + block_frames * kChannels, 0.0F);
            auto& ring = capture_channel.audio[output_index];
            const auto consumed = ring.read(channel_mix.data(), block_frames);
            profile_scope.record_ring_occupancy(ring.available_frames());
            profile_scope.record_unfilled_mix_frames(block_frames - consumed);
            if ((capture_channel.output_mask.load(std::memory_order_acquire) & output_bit) != 0)
                dsp::accumulate_ramp(samples + output_offset, channel_mix.data(),
                                     block_frames, kChannels, {1.0F, 0.0F});
        }
    }

    const float output_target = output.muted.load(std::memory_order_relaxed)
        ? 0.0F : output.target_gain.load(std::memory_order_relaxed);
    const float output_gain = output.smoothed_gain;
    const float output_increment = frames == 0 ? 0.0F
        : (output_target - output_gain) / static_cast<float>(frames);
    dsp::scale_ramp(samples, frames, kChannels, {output_gain, output_increment});
    output.smoothed_gain = output_target;

    if (state_.shutdown_fade_requested.load(std::memory_order_acquire)) {
        if (output.shutdown_fade_frames_remaining == 0 && output.shutdown_fade_gain > 0.0F)
            output.shutdown_fade_frames_remaining = kShutdownFadeFrames;
        dsp::apply_fade(samples, frames, kChannels, output.shutdown_fade_gain,
                        output.shutdown_fade_frames_remaining, kShutdownFadeFrames);
    }
    output.meter.observe(samples, frames * kChannels);
    state_.latest_input_ring_frames[output_index].store(latest_ring_frames, std::memory_order_relaxed);
    if (playback_underrun_frames != 0) {
        state_.xruns.record_playback_underrun(playback_underrun_frames);
    }

}

} // namespace shatter::mixer
