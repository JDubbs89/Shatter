#include "shatter/engine/audio_backend.hpp"

#include "shatter/engine/pipewire_engine.hpp"

#include <memory>

namespace shatter::engine {

std::unique_ptr<AudioBackend> create_audio_backend()
{
    return std::make_unique<PipeWireEngine>();
}

} // namespace shatter::engine
