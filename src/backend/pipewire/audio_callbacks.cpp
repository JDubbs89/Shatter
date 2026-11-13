#include "backend/pipewire/pipewire_backend.hpp"

#include <pipewire/stream.h>

#include <algorithm>
#include <cstdint>

namespace shatter::backend {
namespace {

constexpr auto kSampleRate = mixer::sample_rate;
constexpr auto kChannels = mixer::channels;

} // namespace

void PipeWireBackend::on_output_process(void* data)
{
    auto& output = *static_cast<OutputChannel*>(data);
    output.backend->output_process(output);
}

void PipeWireBackend::on_stream_input_process(void* data)
{
    auto& stream = *static_cast<PlaybackStream*>(data);
    stream.backend->stream_input_process(stream);
}

void PipeWireBackend::on_capture_source_process(void* data)
{
    auto& channel = *static_cast<CaptureChannel*>(data);
    channel.backend->capture_source_process(channel);
}

void PipeWireBackend::stream_input_process(PlaybackStream& stream)
{
    RealtimeAccessGate::Guard process_guard(stream.process_gate);
    if (!process_guard) return;
    const auto output_generations = processor_.output_generations();
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
            processor_.process_playback(stream, samples, frames, output_generations, profile_scope);
        }
    }
    pw_stream_queue_buffer(stream.sink, buffer);
}

void PipeWireBackend::capture_source_process(CaptureChannel& capture_channel)
{
    RealtimeAccessGate::Guard process_guard(capture_channel.process_gate);
    if (!process_guard) return;
    const auto output_generations = processor_.output_generations();
    RuntimeProfiler::Scope profile_scope(profiler_, RuntimeProfiler::Callback::input);
    pw_buffer* buffer = pw_stream_dequeue_buffer(capture_channel.source_stream);
    if (buffer == nullptr) return;
    if (buffer->buffer != nullptr && buffer->buffer->n_datas > 0) {
        spa_data& data = buffer->buffer->datas[0];
        if (data.data != nullptr && data.chunk != nullptr) {
            const auto stride = data.chunk->stride > 0 ? static_cast<std::size_t>(data.chunk->stride)
                                                       : sizeof(float) * kChannels;
            const auto frames = static_cast<std::size_t>(data.chunk->size) / stride;
            const auto* samples = reinterpret_cast<const float*>(
                static_cast<const std::uint8_t*>(data.data) + data.chunk->offset);
            processor_.process_capture(capture_channel, samples, frames, output_generations, profile_scope);
        }
    }
    pw_stream_queue_buffer(capture_channel.source_stream, buffer);
}

void PipeWireBackend::output_process(OutputChannel& output)
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
            processor_.process_output(output_index, samples, frames, profile_scope);
            data.chunk->offset = 0;
            data.chunk->stride = sizeof(float) * kChannels;
            data.chunk->size = frames * sizeof(float) * kChannels;
        }
    }
    pw_stream_queue_buffer(output.stream, buffer);
}

} // namespace shatter::backend
