#pragma once

#include "backend/pulseaudio/pulseaudio_types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shatter::backend {

struct PulsePlaybackSink {
    std::uint32_t index = 0;
    std::string name;
    std::string description;
    std::optional<std::uint64_t> current_latency_usec;
};

class PulseAudioClient {
public:
    using EventCallback = std::function<void()>;

    virtual ~PulseAudioClient() = default;

    virtual bool start(EventCallback on_server_change, std::string& error) = 0;
    virtual void stop() = 0;
    virtual bool connected() const = 0;
    virtual bool playback_streams(std::vector<PulsePlaybackStream>& streams, std::string& error) = 0;
    virtual bool sinks(std::vector<PulsePlaybackSink>& sinks, std::string& error) = 0;
    virtual bool default_sink(std::string& name, std::string& error) = 0;
    virtual bool move_stream(std::uint32_t stream_index, std::string_view sink_name,
                             std::string& error) = 0;
};

std::unique_ptr<PulseAudioClient> create_pulse_audio_client();

} // namespace shatter::backend
