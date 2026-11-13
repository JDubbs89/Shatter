#pragma once

#include "mixer/processor.hpp"

#include <concepts>
#include <cstddef>

namespace shatter::mixer {

// A truthy buffer owns a native acquisition even if samples() is null. Such a
// buffer must still be released/submitted. A false buffer owns nothing. Buffers
// borrow storage; accessors and adapter operations must not allocate or block.
template<class Buffer>
concept InputAudioBuffer = requires(const Buffer& buffer) {
    { static_cast<bool>(buffer) } noexcept -> std::same_as<bool>;
    { buffer.samples() } noexcept -> std::same_as<const float*>;
    { buffer.frames() } noexcept -> std::same_as<std::size_t>;
};

template<class Buffer>
concept OutputAudioBuffer = requires(const Buffer& buffer) {
    { static_cast<bool>(buffer) } noexcept -> std::same_as<bool>;
    { buffer.samples() } noexcept -> std::same_as<float*>;
    { buffer.frames() } noexcept -> std::same_as<std::size_t>;
};

template<class Backend, class Input>
concept InputBackend = requires(Backend& backend, Input& input,
                                decltype(backend.acquire_input(input))& buffer) {
    { backend.acquire_input(input) } noexcept -> InputAudioBuffer;
    { backend.release_input(input, buffer) } noexcept -> std::same_as<void>;
};

template<class Backend, class Output>
concept OutputBackend = requires(Backend& backend, Output& output,
                                 decltype(backend.acquire_output(output))& buffer) {
    { backend.acquire_output(output) } noexcept -> OutputAudioBuffer;
    { backend.submit_output(output, buffer) } noexcept -> std::same_as<void>;
    { backend.output_index(output) } noexcept -> std::same_as<std::size_t>;
};

// Static processing seam. This class never stores an IAudioBackend pointer.
// Backend operations resolve against the concrete type, independently of its
// virtual discovery/lifecycle interface. Input and output scheduling stay with
// the native adapter; there is no assumed synchronous read/DSP/write cycle.
template<class Backend>
class AudioEngine {
public:
    AudioEngine(Backend& backend, Processor& processor, RuntimeProfiler& profiler) noexcept
        : backend_(backend), processor_(processor), profiler_(profiler) {}

    template<class Input>
        requires InputBackend<Backend, Input> && std::convertible_to<Input&, PlaybackStream&>
    void process_playback(Input& input) noexcept
    {
        RealtimeAccessGate::Guard guard(input.process_gate);
        if (!guard) return;
        const auto generations = processor_.output_generations();
        RuntimeProfiler::Scope profile(profiler_, RuntimeProfiler::Callback::input);
        auto buffer = backend_.acquire_input(input);
        if (!buffer) return;
        if (buffer.samples() != nullptr)
            processor_.process_playback(input, buffer.samples(), buffer.frames(), generations, profile);
        backend_.release_input(input, buffer);
    }

    template<class Input>
        requires InputBackend<Backend, Input> && std::convertible_to<Input&, CaptureChannel&>
    void process_capture(Input& input) noexcept
    {
        RealtimeAccessGate::Guard guard(input.process_gate);
        if (!guard) return;
        const auto generations = processor_.output_generations();
        RuntimeProfiler::Scope profile(profiler_, RuntimeProfiler::Callback::input);
        auto buffer = backend_.acquire_input(input);
        if (!buffer) return;
        if (buffer.samples() != nullptr)
            processor_.process_capture(input, buffer.samples(), buffer.frames(), generations, profile);
        backend_.release_input(input, buffer);
    }

    template<class Output>
        requires OutputBackend<Backend, Output> && std::convertible_to<Output&, OutputChannel&>
    void process_output(Output& output) noexcept
    {
        RealtimeAccessGate::Guard guard(output.process_gate);
        if (!guard) return;
        RuntimeProfiler::Scope profile(profiler_, RuntimeProfiler::Callback::output);
        auto buffer = backend_.acquire_output(output);
        if (!buffer) return;
        if (buffer.samples() != nullptr) {
            profile.set_frames(buffer.frames(), sample_rate);
            if constexpr (requires { backend_.observe_output_timing(output, buffer, profile); }) {
                static_assert(noexcept(backend_.observe_output_timing(output, buffer, profile)));
                backend_.observe_output_timing(output, buffer, profile);
            }
            processor_.process_output(backend_.output_index(output), buffer.samples(), buffer.frames(), profile);
        }
        backend_.submit_output(output, buffer);
    }

private:
    Backend& backend_;
    Processor& processor_;
    RuntimeProfiler& profiler_;
};

} // namespace shatter::mixer
