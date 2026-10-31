#include "shatter/engine/audio_backend.hpp"

namespace shatter::engine {
namespace {

class UnavailableAudioBackend final : public AudioBackend {
public:
    bool start(std::string& error) override { error = unavailable_message(); return false; }
    void stop() override {}
#define SHATTER_UNAVAILABLE_OPERATION(signature) bool signature override { error = unavailable_message(); return false; }
    SHATTER_UNAVAILABLE_OPERATION(set_app_gain_db(std::string_view, float, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(set_stream_gain_db(std::string_view, float, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(set_app_muted(std::string_view, bool, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(set_stream_muted(std::string_view, bool, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(rename_app(std::string_view, std::string, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(rename_stream(std::string_view, std::string, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(set_capture_source(std::string_view, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(set_capture_gain_db(float, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(set_capture_muted(bool, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(set_fader_db(std::string_view, std::string_view, float, bool, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(adjust_fader_db(std::string_view, std::string_view, float, bool, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(set_capture_speaker_send(bool, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(set_output_gain_db(std::string_view, float, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(set_output_muted(std::string_view, bool, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(make_output(std::string, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(delete_output(std::string_view, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(assign_output_destination(std::string_view, std::string_view, bool, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(assign_app_output(std::string_view, std::string_view, bool, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(assign_input_output(std::string_view, std::string_view, std::string_view, bool,
                                                       std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(load_config(std::string_view, std::string& error))
    SHATTER_UNAVAILABLE_OPERATION(save_config(std::string_view, std::string& error))
#undef SHATTER_UNAVAILABLE_OPERATION

    bool load_default_config(std::string&, std::string& error) override
    {
        error = unavailable_message();
        return false;
    }
    std::string list_inputs(std::size_t) const override { return unavailable_message() + "\n"; }
    std::string list_outputs(std::size_t) const override { return unavailable_message() + "\n"; }
    std::string list_meters(std::string_view) const override { return unavailable_message() + "\n"; }
    std::string fader_status() const override { return unavailable_message() + "\n"; }
    bool profile_start(std::string& error) override { error = unavailable_message(); return false; }
    std::string profile_stop(std::string& error) override { error = unavailable_message(); return {}; }
    bool available() const noexcept override { return false; }
    void begin_shutdown_fade() noexcept override {}
    void pause_streams_for_shutdown() noexcept override {}

private:
    static std::string unavailable_message()
    {
        return "no audio backend is configured for this build; select a supported backend in CMake";
    }
};

} // namespace

std::unique_ptr<AudioBackend> create_audio_backend()
{
    return std::make_unique<UnavailableAudioBackend>();
}

} // namespace shatter::engine
