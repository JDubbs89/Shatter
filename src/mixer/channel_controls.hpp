#pragma once

#include "mixer/processor.hpp"

#include <mutex>
#include <string>
#include <string_view>

namespace shatter::mixer {

// Non-realtime mixer controls over the same state used by Processor. Native
// adapters supply stable records and their structural mutex. Methods lock that
// mutex for selector-based operations. Methods suffixed _locked require the
// caller to hold it; bulk atomic updates and shutdown fading do not take it.
class ChannelControls {
public:
    ChannelControls(ProcessingState state, std::mutex& mutex) noexcept
        : state_(state), mutex_(mutex) {}

    bool set_app_gain_db(std::string_view channel_id, float decibels, std::string& error);
    bool set_stream_gain_db(std::string_view stream_selector, float decibels, std::string& error);
    bool set_app_muted(std::string_view channel_id, bool muted, std::string& error);
    bool set_stream_muted(std::string_view stream_selector, bool muted, std::string& error);
    bool rename_app(std::string_view current_name, std::string new_name, std::string& error);
    bool rename_stream(std::string_view current_name, std::string new_name, std::string& error);
    bool set_capture_source(std::string_view source_selector, std::string& error);
    bool set_capture_gain_db(float decibels, std::string& error);
    bool set_capture_muted(std::string_view capture_selector, bool muted, std::string& error);
    bool adjust_fader_db(std::string_view scope, std::string_view selector, float delta_db,
                                 bool minimum, std::string& error);
    bool set_fader_db(std::string_view scope, std::string_view selector, float decibels,
                              bool minimum, std::string& error);
    bool set_output_gain_db(std::string_view output_selector, float decibels,
                                    std::string& error);
    bool set_output_muted(std::string_view output_selector, bool muted,
                                  std::string& error);
    bool set_capture_speaker_send(bool enabled, std::string& error);
    std::string list_meters(std::string_view selector = {}) const;
    std::string fader_status() const;
    void begin_shutdown_fade() noexcept;

    AppChannel* find_channel_locked(std::string_view channel_id);
    AppChannel* resolve_channel_locked(std::string_view selector, std::string& error);
    PlaybackStream* resolve_stream_locked(std::string_view selector, std::string& error);
    CaptureChannel* resolve_capture_channel_locked(std::string_view selector, std::string& error);
    OutputChannel* resolve_output_locked(std::string_view selector, std::string& error);
    bool assign_input_output_locked(std::string_view scope, std::string_view input_selector,
                                    std::string_view output_selector, bool assigned, std::string& error);
    bool assign_app_output_locked(std::string_view app_selector, std::string_view output_selector,
                                  bool assigned, std::string& error);

private:
    enum class FaderUpdateMode { absolute, relative, minimum };
    bool update_fader_db(std::string_view scope, std::string_view selector, float decibels,
                         FaderUpdateMode mode, std::string& error);
    ProcessingState state_;
    std::mutex& mutex_;
};

} // namespace shatter::mixer
