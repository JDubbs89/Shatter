#include "backend/iaudio_backend.hpp"
#include "mixer/engine.hpp"

namespace shatter::backend {
namespace {

class UnavailableAudioBackend final : public IAudioBackend {
public:
    static std::string unsupported_operation()
    {
        return "no supported audio server is currently available; Shatter will retry automatically";
    }
    bool enumerate_endpoints(std::vector<Endpoint>&, std::string& error) const override
    {
        error = unavailable_message();
        return false;
    }

    bool start(std::string& error) override { error = unavailable_message(); return false; }
    void stop() override {}

    std::string list_inputs(std::size_t) const { return unavailable_message() + "\n"; }
    std::string list_outputs(std::size_t) const { return unavailable_message() + "\n"; }
    std::string list_meters(std::string_view) const { return unavailable_message() + "\n"; }
    std::string fader_status() const { return unavailable_message() + "\n"; }
    std::string health_status() const
    {
        return "Engine health: failed-open\nReason: " + unavailable_message() +
            "\nXruns since engine start: unavailable (no audio backend)\nI/O latency: unavailable\n";
    }
    std::string profile_stop(std::string& error) { error = unavailable_message(); return {}; }
    bool available() const noexcept override { return false; }

private:
    static std::string unavailable_message()
    {
        return "no supported audio server is currently available; Shatter will retry automatically";
    }
};

} // namespace

} // namespace shatter::backend

namespace shatter::backend {
std::unique_ptr<mixer::IControl> create_unavailable_engine()
{
    return std::make_unique<mixer::Engine<UnavailableAudioBackend>>();
}
} // namespace shatter::backend
