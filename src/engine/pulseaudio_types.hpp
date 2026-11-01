#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace shatter::engine {

struct PulsePlaybackStream {
    std::uint32_t index = 0;
    std::uint32_t sink_index = 0;
    std::string application;
    std::string process_binary;
};

inline bool matches_pulse_playback_selector(const PulsePlaybackStream& stream,
                                            std::string_view selector) noexcept
{
    return !selector.empty() && (stream.application == selector || stream.process_binary == selector);
}

} // namespace shatter::engine
