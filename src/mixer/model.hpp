#pragma once

#include "mixer/audio_ring.hpp"
#include "mixer/realtime_access_gate.hpp"
#include "mixer/meter/meter_bus.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace shatter::mixer {

inline constexpr std::size_t max_app_channels = 1024;
inline constexpr std::size_t max_playback_streams = 128;
inline constexpr std::size_t max_output_channels = 8;
inline constexpr std::size_t max_capture_channels = 32;
inline constexpr std::uint32_t sample_rate = 48000;
inline constexpr std::uint32_t channels = 2;

// Portable channel state. Native adapters may extend these records with endpoint
// handles; those handles are never visible to the processor. Records have stable
// addresses for their entire processing lifetime. Control changes use atomics or
// close/wait/reopen the appropriate gates before resetting or reusing a record.

struct AppChannel {
    bool allocated = false;
    std::string identity;
    std::string id;
    std::string name;
    std::atomic<float> target_gain{1.0F};
    std::atomic<bool> muted{false};
    std::atomic<std::uint32_t> output_mask{1};
    std::array<float, max_output_channels> smoothed_gain{};
    std::array<meter::PeakRmsMeter, max_output_channels> meters;
};

struct PlaybackStream {
    bool in_use = false;
    std::atomic<bool> active{false};
    RealtimeAccessGate process_gate;
    RealtimeAccessGate mixer_gate;
    std::size_t channel_index = 0;
    std::string name;
    std::string original_name;
    std::string selector_id;
    std::string application;
    std::atomic<float> target_gain{1.0F};
    std::atomic<bool> muted{false};
    std::atomic<std::uint32_t> output_mask{0};
    float smoothed_gain = 1.0F;
    std::array<float, max_output_channels> smoothed_send_gain{};
    std::array<AudioRingBuffer, max_output_channels> audio;
    std::array<AudioRingBuffer, max_output_channels> send_audio;
    meter::PeakRmsMeter meter;
};

struct PlaybackRouteState {
    std::string name;
    float gain = 1.0F;
    bool muted = false;
    std::uint32_t output_mask = 0;
};

struct CaptureChannel {
    std::atomic<bool> in_use{false};
    std::string name;
    std::string identity;
    std::string selector_id;
    std::atomic<bool> active{false};
    RealtimeAccessGate process_gate;
    RealtimeAccessGate mixer_gate;
    std::atomic<float> target_gain{1.0F};
    std::atomic<bool> muted{false};
    std::atomic<std::uint32_t> output_mask{0};
    float smoothed_gain = 1.0F;
    std::array<AudioRingBuffer, max_output_channels> audio;
    meter::PeakRmsMeter meter;
};

struct OutputChannel {
    bool in_use = false;
    std::atomic<bool> active{false};
    std::atomic<bool> linked{false};
    std::atomic<std::uint32_t> input_generation{0};
    RealtimeAccessGate process_gate;
    RealtimeAccessGate input_gate;
    std::string name;
    bool capture_source = false;
    std::atomic<float> target_gain{1.0F};
    std::atomic<bool> muted{false};
    float smoothed_gain = 1.0F;
    meter::PeakRmsMeter meter;
    float shutdown_fade_gain = 1.0F;
    std::uint32_t shutdown_fade_frames_remaining = 0;
};

} // namespace shatter::mixer
