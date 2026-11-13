#include "backend/selection.hpp"
#include "backend/providers.hpp"
#include "mixer/control.hpp"

namespace shatter::backend {

std::string_view name(Kind kind) noexcept
{
    switch (kind) {
    case Kind::pipewire: return "pipewire";
    case Kind::pulseaudio: return "pulseaudio";
    case Kind::wasapi: return "wasapi";
    default: return "none";
    }
}

std::unique_ptr<mixer::IControl> create_engine(Kind kind)
{
#if defined(_WIN32)
    if (kind == Kind::wasapi) return create_wasapi_engine();
#else
    if (kind == Kind::pipewire) return create_pipewire_engine();
    if (kind == Kind::pulseaudio) return create_pulseaudio_engine();
#endif
    return create_unavailable_engine();
}

} // namespace shatter::backend

namespace shatter::mixer {
std::unique_ptr<IControl> create_engine()
{
    return backend::create_engine(backend::detect_server().kind);
}
} // namespace shatter::mixer
