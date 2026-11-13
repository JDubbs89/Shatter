#include "backend/pipewire/pipewire_backend.hpp"
#include "mixer/engine.hpp"

namespace shatter::mixer {

std::unique_ptr<IControl> create_engine()
{
    return std::make_unique<Engine<backend::PipeWireBackend>>();
}

} // namespace shatter::mixer
