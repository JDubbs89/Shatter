#pragma once

#include "mixer/model.hpp"
#include "mixer/health.hpp"
#include "mixer/runtime_profiler.hpp"

#include <span>

namespace shatter::mixer {

// Non-owning view bound once before audio starts. All records and spans must
// outlive the processor and its callbacks. Each input has one producer; each
// output has one consumer, using a separate ring per input/output relationship.
// Adapters publish active/linked state and synchronize structural changes using
// the records' gates. Sample buffers are interleaved stereo float at 48 kHz.
struct ProcessingState {
    std::span<AppChannel> apps;
    std::span<PlaybackStream* const> streams;
    std::span<OutputChannel* const> outputs;
    std::span<CaptureChannel* const> captures;
    std::atomic<std::size_t>& allocated_channels;
    std::atomic<std::size_t>& allocated_streams;
    RuntimeProfiler& profiler;
    XrunCounters& xruns;
    std::span<std::atomic<std::uint64_t>> latest_input_ring_frames;
    std::atomic<bool>& shutdown_fade_requested;
};

class Processor {
public:
    using OutputGenerations = std::array<std::uint32_t, max_output_channels>;

    explicit Processor(ProcessingState state) noexcept : state_(state) {}

    // Snapshot before acquiring a native input buffer so slot reuse cannot send
    // an old buffer into a newly created output. Input process_gate must be held.
    OutputGenerations output_generations() const noexcept;
    void process_playback(PlaybackStream& stream, const float* samples, std::size_t frames,
                          const OutputGenerations& generations, RuntimeProfiler::Scope& profile) noexcept;
    void process_capture(CaptureChannel& capture, const float* samples, std::size_t frames,
                         const OutputGenerations& generations, RuntimeProfiler::Scope& profile) noexcept;
    // output_index identifies output in state.outputs; output.process_gate must
    // be held. Buffers contain frames * channels samples. No allocation or locks.
    void process_output(std::size_t output_index, float* samples, std::size_t frames,
                        RuntimeProfiler::Scope& profile) noexcept;

private:
    ProcessingState state_;
};

} // namespace shatter::mixer
