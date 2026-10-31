#include "pipewire_engine.hpp"
#include "shatter/engine/dsp.hpp"

#include <pipewire/stream.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace shatter::engine {
namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kShutdownFadeFrames = kSampleRate / 4;

} // namespace

void PipeWireEngine::on_output_process(void* data)
{
    auto& output = *static_cast<OutputChannel*>(data);
    output.engine->output_process(output);
}

void PipeWireEngine::on_stream_input_process(void* data)
{
    auto& stream = *static_cast<PlaybackStream*>(data);
    stream.engine->stream_input_process(stream);
}


void PipeWireEngine::on_capture_source_process(void* data)
{
    static_cast<PipeWireEngine*>(data)->capture_source_process();
}


void PipeWireEngine::stream_input_process(PlaybackStream& stream)
{
    RealtimeAccessGate::Guard process_guard(stream.process_gate);
    if (!process_guard) return;
    std::array<std::uint32_t, max_output_channels> output_generations{};
    for (std::size_t index = 0; index < output_generations.size(); ++index)
        output_generations[index] = output_channels_[index].input_generation.load(std::memory_order_acquire);
    RuntimeProfiler::Scope profile_scope(profiler_, RuntimeProfiler::Callback::input);
    pw_buffer* buffer = pw_stream_dequeue_buffer(stream.sink);
    if (buffer == nullptr) {
        return;
    }
    if (buffer->buffer != nullptr && buffer->buffer->n_datas > 0) {
        spa_data& data = buffer->buffer->datas[0];
        if (data.data != nullptr && data.chunk != nullptr) {
            const auto stride = data.chunk->stride > 0 ? static_cast<std::size_t>(data.chunk->stride)
                                                       : sizeof(float) * kChannels;
            const auto frames = static_cast<std::size_t>(data.chunk->size) / stride;
            const auto* samples = reinterpret_cast<const float*>(
                static_cast<const std::uint8_t*>(data.data) + data.chunk->offset);
            const float target = stream.muted.load(std::memory_order_relaxed)
                ? 0.0F : stream.target_gain.load(std::memory_order_relaxed);
            const float increment = frames == 0 ? 0.0F
                : (target - stream.smoothed_gain) / static_cast<float>(frames);
            std::size_t written = frames;
            const auto app_mask = app_channels_[stream.channel_index].output_mask.load(std::memory_order_acquire);
            const auto stream_mask = stream.output_mask.load(std::memory_order_acquire);
            const auto mask = app_mask | stream_mask;
            for (std::size_t output_index = 0; output_index < output_channels_.size(); ++output_index) {
                if ((mask & (1U << output_index)) == 0) continue;
                OutputChannel& output = output_channels_[output_index];
                RealtimeAccessGate::Guard output_guard(output.input_gate);
                if (!output_guard) continue;
                if (output.input_generation.load(std::memory_order_acquire) != output_generations[output_index])
                    continue;
                const auto current_app_mask = app_channels_[stream.channel_index].output_mask.load(std::memory_order_acquire);
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
            if (mask != 0) profile_scope.record_input_overrun(frames - written);
            stream.smoothed_gain = target;
        }
    }
    pw_stream_queue_buffer(stream.sink, buffer);
}

void PipeWireEngine::capture_source_process()
{
    RealtimeAccessGate::Guard process_guard(capture_channel_.process_gate);
    if (!process_guard) return;
    std::array<std::uint32_t, max_output_channels> output_generations{};
    for (std::size_t index = 0; index < output_generations.size(); ++index)
        output_generations[index] = output_channels_[index].input_generation.load(std::memory_order_acquire);
    RuntimeProfiler::Scope profile_scope(profiler_, RuntimeProfiler::Callback::input);
    pw_buffer* buffer = pw_stream_dequeue_buffer(capture_channel_.source_stream);
    if (buffer == nullptr) return;
    if (buffer->buffer != nullptr && buffer->buffer->n_datas > 0) {
        spa_data& data = buffer->buffer->datas[0];
        if (data.data != nullptr && data.chunk != nullptr) {
            const auto stride = data.chunk->stride > 0 ? static_cast<std::size_t>(data.chunk->stride)
                                                       : sizeof(float) * kChannels;
            const auto frames = static_cast<std::size_t>(data.chunk->size) / stride;
            const auto* samples = reinterpret_cast<const float*>(
                static_cast<const std::uint8_t*>(data.data) + data.chunk->offset);
            const float target = capture_channel_.muted.load(std::memory_order_relaxed)
                ? 0.0F : capture_channel_.target_gain.load(std::memory_order_relaxed);
            const float increment = frames == 0 ? 0.0F
                : (target - capture_channel_.smoothed_gain) / static_cast<float>(frames);
            const auto mask = capture_channel_.output_mask.load(std::memory_order_acquire);
            for (std::size_t output_index = 0; output_index < output_channels_.size(); ++output_index) {
                if ((mask & (1U << output_index)) == 0) continue;
                OutputChannel& output = output_channels_[output_index];
                RealtimeAccessGate::Guard output_guard(output.input_gate);
                if (!output_guard) continue;
                if (output.input_generation.load(std::memory_order_acquire) == output_generations[output_index] &&
                    (capture_channel_.output_mask.load(std::memory_order_acquire) & (1U << output_index)) != 0 &&
                    output.linked.load(std::memory_order_acquire))
                    capture_channel_.audio[output_index].write_with_gain_ramp(samples, frames,
                        capture_channel_.smoothed_gain, increment);
            }
            capture_channel_.smoothed_gain = target;
            profile_scope.set_frames(frames, kSampleRate);
        }
    }
    pw_stream_queue_buffer(capture_channel_.source_stream, buffer);
}

void PipeWireEngine::output_process(OutputChannel& output)
{
    RealtimeAccessGate::Guard process_guard(output.process_gate);
    if (!process_guard) return;
    RuntimeProfiler::Scope profile_scope(profiler_, RuntimeProfiler::Callback::output);
    pw_buffer* buffer = pw_stream_dequeue_buffer(output.stream);
    if (buffer == nullptr) return;

    if (buffer->buffer != nullptr && buffer->buffer->n_datas > 0) {
        spa_data& data = buffer->buffer->datas[0];
        if (data.data != nullptr && data.chunk != nullptr) {
            const std::size_t output_index = static_cast<std::size_t>(&output - output_channels_.data());
            const std::uint32_t output_bit = 1U << output_index;
            const auto capacity_frames = static_cast<std::size_t>(data.maxsize) / (sizeof(float) * kChannels);
            const auto requested_frames = buffer->requested == 0
                ? capacity_frames : static_cast<std::size_t>(buffer->requested);
            const auto frames = std::min(capacity_frames, requested_frames);
            profile_scope.set_frames(frames, kSampleRate);
            if (profiler_.active()) {
                pw_time timing{};
                if (pw_stream_get_time_n(output.stream, &timing, sizeof(timing)) == 0 && timing.rate.denom != 0) {
                    const auto delay_ns = timing.delay > 0
                        ? static_cast<std::uint64_t>(timing.delay) * timing.rate.num * 1'000'000'000ULL /
                            timing.rate.denom : 0;
                    profile_scope.record_pipewire_time(delay_ns, timing.queued / (sizeof(float) * kChannels));
                }
            }

            auto* samples = reinterpret_cast<float*>(data.data);
            std::fill(samples, samples + frames * kChannels, 0.0F);
            std::array<float, 1024 * kChannels> channel_mix{};
            const auto channel_count = allocated_channels_.load(std::memory_order_acquire);
            const auto stream_count = allocated_streams_.load(std::memory_order_acquire);
            for (std::size_t offset = 0; offset < frames; offset += 1024) {
                const auto block_frames = std::min<std::size_t>(1024, frames - offset);
                const auto output_offset = offset * kChannels;
                for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
                    AppChannel& channel = app_channels_[channel_index];
                    const bool assigned = (channel.output_mask.load(std::memory_order_acquire) & output_bit) != 0;
                    std::fill(channel_mix.begin(), channel_mix.begin() + block_frames * kChannels, 0.0F);
                    for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
                        PlaybackStream& stream = playback_streams_[stream_index];
                        if (!stream.active.load(std::memory_order_acquire) || stream.channel_index != channel_index) continue;
                        RealtimeAccessGate::Guard stream_guard(stream.mixer_gate);
                        if (!stream_guard || !stream.active.load(std::memory_order_acquire)) continue;
                        auto& ring = stream.audio[output_index];
                        const auto occupancy = ring.available_frames();
                        profile_scope.record_ring_occupancy(occupancy);
                        profile_scope.record_app_stream_ring_occupancy(occupancy);
                        const auto consumed = ring.accumulate(channel_mix.data(), block_frames, 1.0F);
                        profile_scope.record_unfilled_mix_frames(block_frames - consumed);
                    }
                    const float target = channel.muted.load(std::memory_order_relaxed)
                        ? 0.0F : channel.target_gain.load(std::memory_order_relaxed);
                    float gain = channel.smoothed_gain[output_index];
                    const float increment = block_frames == 0 ? 0.0F
                        : (target - gain) / static_cast<float>(block_frames);
                    if (assigned) {
                        dsp::accumulate_ramp(samples + output_offset, channel_mix.data(),
                                             block_frames, kChannels, {gain, increment});
                    }
                    channel.smoothed_gain[output_index] = target;

                    for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
                        PlaybackStream& stream = playback_streams_[stream_index];
                        if (!stream.active.load(std::memory_order_acquire) || stream.channel_index != channel_index ||
                            (stream.output_mask.load(std::memory_order_acquire) & output_bit) == 0) continue;
                        RealtimeAccessGate::Guard stream_guard(stream.mixer_gate);
                        if (!stream_guard) continue;
                        auto& ring = stream.send_audio[output_index];
                        profile_scope.record_app_stream_ring_occupancy(ring.available_frames());
                        const auto consumed = ring.read(channel_mix.data(), block_frames);
                        profile_scope.record_unfilled_mix_frames(block_frames - consumed);
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

                std::fill(channel_mix.begin(), channel_mix.begin() + block_frames * kChannels, 0.0F);
                RealtimeAccessGate::Guard capture_guard(capture_channel_.mixer_gate);
                if (capture_guard) {
                    auto& ring = capture_channel_.audio[output_index];
                    const auto consumed = ring.read(channel_mix.data(), block_frames);
                    profile_scope.record_ring_occupancy(ring.available_frames());
                    profile_scope.record_unfilled_mix_frames(block_frames - consumed);
                    if ((capture_channel_.output_mask.load(std::memory_order_acquire) & output_bit) != 0) {
                        dsp::accumulate_ramp(samples + output_offset, channel_mix.data(),
                                             block_frames, kChannels, {1.0F, 0.0F});
                    }
                }
            }

            const float output_target = output.muted.load(std::memory_order_relaxed)
                ? 0.0F : output.target_gain.load(std::memory_order_relaxed);
            const float output_gain = output.smoothed_gain;
            const float output_increment = frames == 0 ? 0.0F
                : (output_target - output_gain) / static_cast<float>(frames);
            dsp::scale_ramp(samples, frames, kChannels, {output_gain, output_increment});
            output.smoothed_gain = output_target;

            if (shutdown_fade_requested_.load(std::memory_order_acquire)) {
                if (output.shutdown_fade_frames_remaining == 0 && output.shutdown_fade_gain > 0.0F)
                    output.shutdown_fade_frames_remaining = kShutdownFadeFrames;
                dsp::apply_fade(samples, frames, kChannels, output.shutdown_fade_gain,
                                output.shutdown_fade_frames_remaining, kShutdownFadeFrames);
            }
            data.chunk->offset = 0;
            data.chunk->stride = sizeof(float) * kChannels;
            data.chunk->size = frames * sizeof(float) * kChannels;
        }
    }
    pw_stream_queue_buffer(output.stream, buffer);
}


} // namespace shatter::engine
