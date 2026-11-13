#include "backend/pipewire/pipewire_backend.hpp"

#include <pipewire/stream.h>

#include <algorithm>
#include <cstdint>

namespace shatter::backend {
namespace {

PipeWireBackend::InputBuffer acquire_input_buffer(pw_stream* stream) noexcept
{
    PipeWireBackend::InputBuffer buffer{pw_stream_dequeue_buffer(stream)};
    if (!buffer || buffer.native->buffer == nullptr || buffer.native->buffer->n_datas == 0)
        return buffer;
    const spa_data& data = buffer.native->buffer->datas[0];
    if (data.data != nullptr && data.chunk != nullptr) {
        const auto stride = data.chunk->stride > 0 ? static_cast<std::size_t>(data.chunk->stride)
                                                   : sizeof(float) * mixer::channels;
        buffer.frame_count = static_cast<std::size_t>(data.chunk->size) / stride;
        buffer.data = reinterpret_cast<const float*>(
            static_cast<const std::uint8_t*>(data.data) + data.chunk->offset);
    }
    return buffer;
}

} // namespace

PipeWireBackend::InputBuffer PipeWireBackend::acquire_input(PlaybackStream& stream) noexcept
{
    return acquire_input_buffer(stream.sink);
}

PipeWireBackend::InputBuffer PipeWireBackend::acquire_input(CaptureChannel& channel) noexcept
{
    return acquire_input_buffer(channel.source_stream);
}

void PipeWireBackend::release_input(PlaybackStream& stream, InputBuffer buffer) noexcept
{
    pw_stream_queue_buffer(stream.sink, buffer.native);
}

void PipeWireBackend::release_input(CaptureChannel& channel, InputBuffer buffer) noexcept
{
    pw_stream_queue_buffer(channel.source_stream, buffer.native);
}

PipeWireBackend::OutputBuffer PipeWireBackend::acquire_output(OutputChannel& output) noexcept
{
    OutputBuffer buffer{pw_stream_dequeue_buffer(output.stream)};
    if (!buffer || buffer.native->buffer == nullptr || buffer.native->buffer->n_datas == 0)
        return buffer;
    const spa_data& data = buffer.native->buffer->datas[0];
    if (data.data != nullptr && data.chunk != nullptr) {
        const auto capacity = static_cast<std::size_t>(data.maxsize) / (sizeof(float) * mixer::channels);
        const auto requested = buffer.native->requested == 0
            ? capacity : static_cast<std::size_t>(buffer.native->requested);
        buffer.frame_count = std::min(capacity, requested);
        buffer.data = static_cast<float*>(data.data);
    }
    return buffer;
}

void PipeWireBackend::submit_output(OutputChannel& output, OutputBuffer buffer) noexcept
{
    if (buffer.samples() != nullptr) {
        spa_chunk& chunk = *buffer.native->buffer->datas[0].chunk;
        chunk.offset = 0;
        chunk.stride = sizeof(float) * mixer::channels;
        chunk.size = buffer.frames() * sizeof(float) * mixer::channels;
    }
    pw_stream_queue_buffer(output.stream, buffer.native);
}

std::size_t PipeWireBackend::output_index(const OutputChannel& output) const noexcept
{
    return static_cast<std::size_t>(&output - output_channels_.data());
}

void PipeWireBackend::observe_output_timing(OutputChannel& output, const OutputBuffer&,
                                            RuntimeProfiler::Scope& profile) noexcept
{
    if (!profiler_.active()) return;
    pw_time timing{};
    if (pw_stream_get_time_n(output.stream, &timing, sizeof(timing)) == 0 && timing.rate.denom != 0) {
        const auto delay_ns = timing.delay > 0
            ? static_cast<std::uint64_t>(timing.delay) * timing.rate.num * 1'000'000'000ULL /
                timing.rate.denom : 0;
        profile.record_pipewire_time(delay_ns, timing.queued / (sizeof(float) * mixer::channels));
    }
}

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
    audio_engine_.process_playback(stream);
}

void PipeWireBackend::capture_source_process(CaptureChannel& channel)
{
    audio_engine_.process_capture(channel);
}

void PipeWireBackend::output_process(OutputChannel& output)
{
    audio_engine_.process_output(output);
}

} // namespace shatter::backend
