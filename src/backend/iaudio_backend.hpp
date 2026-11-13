#pragma once

#include "backend/endpoint.hpp"

#include <string>
#include <vector>

namespace shatter::backend {

// Non-realtime native discovery and lifecycle only. Mixer controls and sample
// buffers are deliberately absent. Concrete adapters additionally supply the
// statically dispatched operations required by mixer::AudioEngine when they
// support custom processing.
class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;
    virtual bool available() const noexcept = 0;
    // Replace endpoints only on success; leave it untouched on failure.
    virtual bool enumerate_endpoints(std::vector<Endpoint>& endpoints, std::string& error) const = 0;
};

} // namespace shatter::backend
