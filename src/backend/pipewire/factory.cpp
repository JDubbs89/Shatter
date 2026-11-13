#include "backend/pipewire/pipewire_backend.hpp"
#include "mixer/engine.hpp"

namespace shatter::backend {
std::unique_ptr<mixer::IControl> create_pipewire_engine()
{
    return std::make_unique<mixer::Engine<PipeWireBackend>>();
}
} // namespace shatter::backend
