#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shatter::backend {

enum class EndpointKind { playback_device, capture_device, playback_stream, application_capture };

struct AudioFormat {
    std::uint32_t sample_rate = 0;
    std::uint32_t channels = 0;
};

// id is the adapter's listed selector; identity is a native identity, not a
// promise of persistence for transient application streams. Unknown defaults
// and formats are left empty rather than inferred from the mixer configuration.
struct Endpoint {
    std::string id;
    std::string identity;
    std::string name;
    EndpointKind kind = EndpointKind::playback_device;
    std::optional<bool> is_default;
    std::vector<AudioFormat> formats;
};

} // namespace shatter::backend
