#pragma once

#include "backend/iaudio_backend.hpp"
#include "mixer/audio_engine.hpp"
#include "mixer/channel_controls.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace shatter::mixer::test_detail {

// An adapter with virtual discovery/lifecycle and concrete audio operations.
// Fixed buffers and counters keep its callback methods allocation-free.
class MemoryBackend final : public backend::IAudioBackend {
public:
    struct InputBuffer {
        bool acquired;
        const float* data;
        explicit operator bool() const noexcept { return acquired; }
        const float* samples() const noexcept { return data; }
        std::size_t frames() const noexcept { return 4; }
    };
    struct OutputBuffer {
        bool acquired;
        float* data;
        explicit operator bool() const noexcept { return acquired; }
        float* samples() const noexcept { return data; }
        std::size_t frames() const noexcept { return 4; }
    };

    std::array<AppChannel, 1> apps;
    std::array<PlaybackStream, 2> streams;
    std::array<CaptureChannel, 1> captures;
    std::array<OutputChannel, 2> outputs;
    std::array<PlaybackStream*, 2> stream_views{&streams[0], &streams[1]};
    std::array<CaptureChannel*, 1> capture_views{&captures[0]};
    std::array<OutputChannel*, 2> output_views{&outputs[0], &outputs[1]};
    std::atomic<std::size_t> allocated_channels{1};
    std::atomic<std::size_t> allocated_streams{1};
    RuntimeProfiler profiler;
    XrunCounters xruns;
    std::array<std::atomic<std::uint64_t>, 2> ring_frames{};
    std::atomic<bool> fade_requested{false};
    std::mutex mutex;
    Processor processor{processing_state()};
    AudioEngine<MemoryBackend> audio{*this, processor, profiler};

    std::array<float, 8> input{1, 1, 1, 1, 1, 1, 1, 1};
    std::array<float, 8> output{};
    bool input_available = true;
    bool input_valid = true;
    bool output_available = true;
    bool output_valid = true;
    bool reuse_output_on_acquire = false;
    bool running = false;
    int starts = 0;
    int input_acquisitions = 0;
    int input_releases = 0;
    int output_acquisitions = 0;
    int output_submissions = 0;

    MemoryBackend()
    {
        apps[0].allocated = true;
        apps[0].name = "Music";
        apps[0].id = "app-music";
        apps[0].output_mask.store(3);
        apps[0].smoothed_gain.fill(1.0F);
        streams[0].in_use = true;
        streams[0].active.store(true);
        streams[0].name = "Playback";
        streams[0].selector_id = "stream-1";
        for (auto& stream : streams) stream.smoothed_send_gain.fill(1.0F);
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            outputs[i].in_use = true;
            outputs[i].linked.store(true);
            outputs[i].name = "Output " + std::to_string(i);
            outputs[i].identity = "output-" + std::to_string(i);
        }
    }

    ProcessingState processing_state() noexcept
    {
        return {apps, stream_views, output_views, capture_views, allocated_channels,
                allocated_streams, profiler, xruns, ring_frames, fade_requested};
    }
    ChannelControls channel_controls() noexcept { return {processing_state(), mutex}; }
    static std::string unsupported_operation() { return "unsupported test operation"; }

    bool start(std::string&) override { ++starts; running = true; return true; }
    void stop() override { running = false; }
    bool available() const noexcept override { return running; }
    bool enumerate_endpoints(std::vector<backend::Endpoint>& endpoints, std::string& error) const override
    {
        if (!running) { error = "not running"; return false; }
        endpoints = {{"memory-output", "memory-output", "Memory output",
                      backend::EndpointKind::playback_device, true, {{sample_rate, channels}}}};
        error.clear();
        return true;
    }

    InputBuffer acquire_input(PlaybackStream&) noexcept
    {
        ++input_acquisitions;
        if (reuse_output_on_acquire) {
            outputs[0].input_generation.fetch_add(1);
            reuse_output_on_acquire = false;
        }
        return {input_available, input_valid ? input.data() : nullptr};
    }
    InputBuffer acquire_input(CaptureChannel&) noexcept
    {
        ++input_acquisitions;
        return {input_available, input_valid ? input.data() : nullptr};
    }
    void release_input(PlaybackStream&, InputBuffer) noexcept { ++input_releases; }
    void release_input(CaptureChannel&, InputBuffer) noexcept { ++input_releases; }
    OutputBuffer acquire_output(OutputChannel&) noexcept
    {
        ++output_acquisitions;
        return {output_available, output_valid ? output.data() : nullptr};
    }
    void submit_output(OutputChannel&, OutputBuffer) noexcept { ++output_submissions; }
    std::size_t output_index(const OutputChannel& channel) const noexcept
    {
        return static_cast<std::size_t>(&channel - outputs.data());
    }
};

// A discovery-only adapter has no gain methods, buffers, or processing stubs.
class DiscoveryBackend final : public backend::IAudioBackend {
public:
    bool start(std::string&) override { return true; }
    void stop() override {}
    bool available() const noexcept override { return true; }
    bool enumerate_endpoints(std::vector<backend::Endpoint>& result, std::string& error) const override
    {
        result.clear();
        error.clear();
        return true;
    }
    static std::string unsupported_operation() { return "discovery only"; }
};

} // namespace shatter::mixer::test_detail
