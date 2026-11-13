#include "backend/audio_backend.hpp"

#include "backend/pipewire/pipewire_backend.hpp"

#include <memory>

namespace shatter::backend {

std::unique_ptr<AudioBackend> create_audio_backend()
{
    return std::make_unique<PipeWireBackend>();
}

} // namespace shatter::backend
