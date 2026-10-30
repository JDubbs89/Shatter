#include "shatter/engine/pipewire_engine.hpp"

#include <pipewire/stream.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace shatter::engine {
namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kShutdownFadeFrames = kSampleRate / 4;
constexpr std::uint32_t kMixerClosed = 1U << 31U;
constexpr std::uint32_t kMixerUserMask = kMixerClosed - 1U;

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
                std::uint32_t users = output.input_users.load(std::memory_order_acquire);
                bool output_acquired = false;
                while ((users & kMixerClosed) == 0) {
                    if (output.input_users.compare_exchange_weak(users, users + 1,
                            std::memory_order_acquire, std::memory_order_relaxed)) {
                        output_acquired = true;
                        break;
                    }
                }
                if (!output_acquired) continue;
                if (output.linked.load(std::memory_order_acquire)) {
                    if ((app_mask & (1U << output_index)) != 0) {
                        written = std::min(written, stream.audio[output_index].write_with_gain_ramp(
                            samples, frames, stream.smoothed_gain, increment));
                    }
                    if ((stream_mask & (1U << output_index)) != 0) {
                        written = std::min(written, stream.send_audio[output_index].write(samples, frames));
                    }
                }
                output.input_users.fetch_sub(1, std::memory_order_release);
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
                std::uint32_t users = output.input_users.load(std::memory_order_acquire);
                bool output_acquired = false;
                while ((users & kMixerClosed) == 0) {
                    if (output.input_users.compare_exchange_weak(users, users + 1,
                            std::memory_order_acquire, std::memory_order_relaxed)) {
                        output_acquired = true;
                        break;
                    }
                }
                if (!output_acquired) continue;
                if (output.linked.load(std::memory_order_acquire))
                    capture_channel_.audio[output_index].write_with_gain_ramp(samples, frames,
                        capture_channel_.smoothed_gain, increment);
                output.input_users.fetch_sub(1, std::memory_order_release);
            }
            capture_channel_.smoothed_gain = target;
            profile_scope.set_frames(frames, kSampleRate);
        }
    }
    pw_stream_queue_buffer(capture_channel_.source_stream, buffer);
}

void PipeWireEngine::output_process(OutputChannel& output)
{
    RuntimeProfiler::Scope profile_scope(profiler_, RuntimeProfiler::Callback::output);
    pw_buffer* buffer = pw_stream_dequeue_buffer(output.stream);
    if (buffer == nullptr) return;

    std::uint32_t users = output.mixer_users.load(std::memory_order_acquire);
    bool acquired = false;
    while ((users & kMixerClosed) == 0) {
        if (output.mixer_users.compare_exchange_weak(users, users + 1,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            acquired = true;
            break;
        }
    }
    if (!acquired) {
        pw_stream_queue_buffer(output.stream, buffer);
        return;
    }

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
                        std::uint32_t stream_users = stream.mixer_users.load(std::memory_order_acquire);
                        bool stream_acquired = false;
                        while ((stream_users & kMixerClosed) == 0) {
                            if (stream.mixer_users.compare_exchange_weak(stream_users, stream_users + 1,
                                    std::memory_order_acquire, std::memory_order_relaxed)) {
                                stream_acquired = true;
                                break;
                            }
                        }
                        if (!stream_acquired || !stream.active.load(std::memory_order_acquire)) {
                            if (stream_acquired) stream.mixer_users.fetch_sub(1, std::memory_order_release);
                            continue;
                        }
                        auto& ring = stream.audio[output_index];
                        const auto occupancy = ring.available_frames();
                        profile_scope.record_ring_occupancy(occupancy);
                        profile_scope.record_app_stream_ring_occupancy(occupancy);
                        const auto consumed = ring.accumulate(channel_mix.data(), block_frames, 1.0F);
                        profile_scope.record_unfilled_mix_frames(block_frames - consumed);
                        stream.mixer_users.fetch_sub(1, std::memory_order_release);
                    }
                    const float target = channel.muted.load(std::memory_order_relaxed)
                        ? 0.0F : channel.target_gain.load(std::memory_order_relaxed);
                    float gain = channel.smoothed_gain[output_index];
                    const float increment = block_frames == 0 ? 0.0F
                        : (target - gain) / static_cast<float>(block_frames);
                    if (assigned) {
                        float app_gain = gain;
                        for (std::size_t frame = 0; frame < block_frames; ++frame) {
                            samples[output_offset + frame * kChannels] += channel_mix[frame * kChannels] * app_gain;
                            samples[output_offset + frame * kChannels + 1] += channel_mix[frame * kChannels + 1] * app_gain;
                            app_gain += increment;
                        }
                    }
                    channel.smoothed_gain[output_index] = target;

                    for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
                        PlaybackStream& stream = playback_streams_[stream_index];
                        if (!stream.active.load(std::memory_order_acquire) || stream.channel_index != channel_index ||
                            (stream.output_mask.load(std::memory_order_acquire) & output_bit) == 0) continue;
                        std::uint32_t stream_users = stream.mixer_users.load(std::memory_order_acquire);
                        bool stream_acquired = false;
                        while ((stream_users & kMixerClosed) == 0) {
                            if (stream.mixer_users.compare_exchange_weak(stream_users, stream_users + 1,
                                    std::memory_order_acquire, std::memory_order_relaxed)) {
                                stream_acquired = true;
                                break;
                            }
                        }
                        if (!stream_acquired) continue;
                        auto& ring = stream.send_audio[output_index];
                        profile_scope.record_app_stream_ring_occupancy(ring.available_frames());
                        const auto consumed = ring.read(channel_mix.data(), block_frames);
                        profile_scope.record_unfilled_mix_frames(block_frames - consumed);
                        const float stream_target = stream.muted.load(std::memory_order_relaxed)
                            ? 0.0F : stream.target_gain.load(std::memory_order_relaxed);
                        float stream_gain = stream.smoothed_send_gain[output_index];
                        const float stream_increment = block_frames == 0 ? 0.0F
                            : (stream_target - stream_gain) / static_cast<float>(block_frames);
                        float app_gain = gain;
                        for (std::size_t frame = 0; frame < block_frames; ++frame) {
                            const float combined_gain = app_gain * stream_gain;
                            samples[output_offset + frame * kChannels] += channel_mix[frame * kChannels] * combined_gain;
                            samples[output_offset + frame * kChannels + 1] += channel_mix[frame * kChannels + 1] * combined_gain;
                            app_gain += increment;
                            stream_gain += stream_increment;
                        }
                        stream.smoothed_send_gain[output_index] = stream_target;
                        stream.mixer_users.fetch_sub(1, std::memory_order_release);
                    }
                }

                std::fill(channel_mix.begin(), channel_mix.begin() + block_frames * kChannels, 0.0F);
                std::uint32_t capture_users = capture_channel_.mixer_users.load(std::memory_order_acquire);
                bool capture_acquired = false;
                while ((capture_users & kMixerClosed) == 0) {
                    if (capture_channel_.mixer_users.compare_exchange_weak(capture_users, capture_users + 1,
                            std::memory_order_acquire, std::memory_order_relaxed)) {
                        capture_acquired = true;
                        break;
                    }
                }
                if (capture_acquired) {
                    auto& ring = capture_channel_.audio[output_index];
                    const auto consumed = ring.read(channel_mix.data(), block_frames);
                    profile_scope.record_ring_occupancy(ring.available_frames());
                    profile_scope.record_unfilled_mix_frames(block_frames - consumed);
                    if ((capture_channel_.output_mask.load(std::memory_order_acquire) & output_bit) != 0) {
                        for (std::size_t sample = 0; sample < block_frames * kChannels; ++sample)
                            samples[output_offset + sample] += channel_mix[sample];
                    }
                    capture_channel_.mixer_users.fetch_sub(1, std::memory_order_release);
                }
            }

            if (shutdown_fade_requested_.load(std::memory_order_acquire)) {
                if (output.shutdown_fade_frames_remaining == 0 && output.shutdown_fade_gain > 0.0F)
                    output.shutdown_fade_frames_remaining = kShutdownFadeFrames;
                const float fade_step = 1.0F / static_cast<float>(kShutdownFadeFrames);
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    const float gain = output.shutdown_fade_frames_remaining == 0
                        ? 0.0F : output.shutdown_fade_gain;
                    samples[frame * kChannels] *= gain;
                    samples[frame * kChannels + 1] *= gain;
                    if (output.shutdown_fade_frames_remaining > 0) {
                        output.shutdown_fade_gain = std::max(0.0F, output.shutdown_fade_gain - fade_step);
                        --output.shutdown_fade_frames_remaining;
                    }
                }
            }
            data.chunk->offset = 0;
            data.chunk->stride = sizeof(float) * kChannels;
            data.chunk->size = frames * sizeof(float) * kChannels;
        }
    }
    output.mixer_users.fetch_sub(1, std::memory_order_release);
    pw_stream_queue_buffer(output.stream, buffer);
}


} // namespace shatter::engine
