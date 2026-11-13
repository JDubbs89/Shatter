#pragma once

#include <memory>

namespace shatter::mixer { class IControl; }
namespace shatter::backend {
std::unique_ptr<mixer::IControl> create_pipewire_engine();
std::unique_ptr<mixer::IControl> create_pulseaudio_engine();
std::unique_ptr<mixer::IControl> create_wasapi_engine();
std::unique_ptr<mixer::IControl> create_unavailable_engine();
} // namespace shatter::backend
