#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace shatter::backend {

// Command-facing contract for the selected native audio adapter. It contains no
// native SDK types. Implementations resolve selectors and coordinate endpoint
// lifecycle; adapters that process samples use mixer::Processor. Unsupported
// operations retain their existing backend-specific errors.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;
    virtual bool set_app_gain_db(std::string_view channel_id, float decibels, std::string& error) = 0;
    virtual bool set_stream_gain_db(std::string_view stream_selector, float decibels, std::string& error) = 0;
    virtual bool set_app_muted(std::string_view channel_id, bool muted, std::string& error) = 0;
    virtual bool set_stream_muted(std::string_view stream_selector, bool muted, std::string& error) = 0;
    virtual bool rename_app(std::string_view current_name, std::string new_name, std::string& error) = 0;
    virtual bool rename_stream(std::string_view current_name, std::string new_name, std::string& error) = 0;
    virtual bool set_capture_source(std::string_view source_selector, std::string& error) = 0;
    virtual bool set_capture_gain_db(float decibels, std::string& error) = 0;
    virtual bool set_capture_muted(std::string_view capture_selector, bool muted, std::string& error) = 0;
    virtual bool set_fader_db(std::string_view scope, std::string_view selector, float decibels,
                              bool minimum, std::string& error) = 0;
    virtual bool adjust_fader_db(std::string_view scope, std::string_view selector, float delta_db,
                                 bool minimum, std::string& error) = 0;
    virtual bool set_capture_speaker_send(bool enabled, std::string& error) = 0;
    virtual bool set_output_gain_db(std::string_view output_selector, float decibels,
                                    std::string& error) = 0;
    virtual bool set_output_muted(std::string_view output_selector, bool muted,
                                  std::string& error) = 0;
    virtual bool make_output(std::string name, bool capture_source, std::string& error) = 0;
    virtual bool delete_output(std::string_view selector, std::string& error) = 0;
    virtual bool assign_output_destination(std::string_view output_selector,
                                           std::string_view destination_selector,
                                           bool assigned, std::string& error) = 0;
    virtual bool assign_app_output(std::string_view app_selector, std::string_view output_selector,
                                   bool assigned, std::string& error) = 0;
    virtual bool assign_input_output(std::string_view scope, std::string_view input_selector,
                                     std::string_view output_selector, bool assigned,
                                     std::string& error) = 0;
    virtual bool load_config(std::string_view name, std::string& error) = 0;
    virtual bool save_config(std::string_view name, std::string& error) = 0;
    virtual bool load_default_config(std::string& name, std::string& error) = 0;
    virtual std::string list_inputs(std::size_t terminal_width = 100) const = 0;
    virtual std::string list_outputs(std::size_t terminal_width = 100) const = 0;
    virtual std::string list_meters(std::string_view selector = {}) const = 0;
    virtual std::string fader_status() const = 0;
    virtual std::string health_status() const = 0;
    virtual bool profile_start(std::string& error) = 0;
    virtual std::string profile_stop(std::string& error) = 0;
    virtual bool available() const noexcept = 0;
    virtual void begin_shutdown_fade() noexcept = 0;
    virtual void pause_streams_for_shutdown() noexcept = 0;
};

std::unique_ptr<AudioBackend> create_audio_backend();

} // namespace shatter::backend
