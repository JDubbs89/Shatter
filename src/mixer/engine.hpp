#pragma once

#include "backend/iaudio_backend.hpp"
#include "mixer/channel_controls.hpp"
#include "mixer/control.hpp"

#include <concepts>
#include <memory>
#include <utility>

namespace shatter::mixer {

template<class Backend>
concept MixingBackend = requires(Backend& backend) {
    { backend.channel_controls() } -> std::same_as<ChannelControls>;
};

// Control owns one engine; the engine owns one concrete native backend. The
// IAudioBackend reference exposed to control points to that same object. Native
// optional features are bound at compile time, not added to IAudioBackend.
// Existing discovery-only adapters do not gain mixer features through this type.
template<std::derived_from<backend::IAudioBackend> Backend>
class Engine final : public IControl {
public:
    Engine() : backend_(std::make_unique<Backend>()) {}
    // The injected backend must be non-null and is owned exclusively by this engine.
    explicit Engine(std::unique_ptr<Backend> backend) : backend_(std::move(backend)) {}

    backend::IAudioBackend& backend() noexcept override { return *backend_; }

    bool start(std::string& error) override
    {
        return backend_->start(error);
    }

    void stop() override
    {
        backend_->stop();
    }

    bool set_app_gain_db(std::string_view channel_id, float decibels, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().set_app_gain_db(channel_id, decibels, error);
        } else {
            return unsupported(error);
        }
    }

    bool set_stream_gain_db(std::string_view stream_selector, float decibels, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().set_stream_gain_db(stream_selector, decibels, error);
        } else {
            return unsupported(error);
        }
    }

    bool set_app_muted(std::string_view channel_id, bool muted, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().set_app_muted(channel_id, muted, error);
        } else {
            return unsupported(error);
        }
    }

    bool set_stream_muted(std::string_view stream_selector, bool muted, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().set_stream_muted(stream_selector, muted, error);
        } else {
            return unsupported(error);
        }
    }

    bool rename_app(std::string_view current_name, std::string new_name, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().rename_app(current_name, std::move(new_name), error);
        } else {
            return unsupported(error);
        }
    }

    bool rename_stream(std::string_view current_name, std::string new_name, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().rename_stream(current_name, std::move(new_name), error);
        } else {
            return unsupported(error);
        }
    }

    bool set_capture_source(std::string_view source_selector, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().set_capture_source(source_selector, error);
        } else {
            return unsupported(error);
        }
    }

    bool set_capture_gain_db(float decibels, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().set_capture_gain_db(decibels, error);
        } else {
            return unsupported(error);
        }
    }

    bool set_capture_muted(std::string_view capture_selector, bool muted, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().set_capture_muted(capture_selector, muted, error);
        } else {
            return unsupported(error);
        }
    }

    bool set_fader_db(std::string_view scope, std::string_view selector, float decibels, bool minimum, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().set_fader_db(scope, selector, decibels, minimum, error);
        } else {
            return unsupported(error);
        }
    }

    bool adjust_fader_db(std::string_view scope, std::string_view selector, float delta_db, bool minimum, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().adjust_fader_db(scope, selector, delta_db, minimum, error);
        } else {
            return unsupported(error);
        }
    }

    bool set_capture_speaker_send(bool enabled, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().set_capture_speaker_send(enabled, error);
        } else {
            return unsupported(error);
        }
    }

    bool set_output_gain_db(std::string_view output_selector, float decibels, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().set_output_gain_db(output_selector, decibels, error);
        } else {
            return unsupported(error);
        }
    }

    bool set_output_muted(std::string_view output_selector, bool muted, std::string& error) override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().set_output_muted(output_selector, muted, error);
        } else {
            return unsupported(error);
        }
    }

    bool make_output(std::string name, bool capture_source, std::string& error) override
    {
        if constexpr (requires(Backend& b) { b.make_output(name, capture_source, error); }) {
            return backend_->make_output(std::move(name), capture_source, error);
        } else {
            return unsupported(error);
        }
    }

    bool delete_output(std::string_view selector, std::string& error) override
    {
        if constexpr (requires(Backend& b) { b.delete_output(selector, error); }) {
            return backend_->delete_output(selector, error);
        } else {
            return unsupported(error);
        }
    }

    bool assign_output_destination(std::string_view output_selector, std::string_view destination_selector, bool assigned, std::string& error) override
    {
        if constexpr (requires(Backend& b) { b.assign_output_destination(output_selector, destination_selector, assigned, error); }) {
            return backend_->assign_output_destination(output_selector, destination_selector, assigned, error);
        } else {
            return unsupported(error);
        }
    }

    bool assign_app_output(std::string_view app_selector, std::string_view output_selector, bool assigned, std::string& error) override
    {
        if constexpr (requires(Backend& b) { b.assign_app_output(app_selector, output_selector, assigned, error); }) {
            return backend_->assign_app_output(app_selector, output_selector, assigned, error);
        } else {
            return unsupported(error);
        }
    }

    bool assign_input_output(std::string_view scope, std::string_view input_selector, std::string_view output_selector, bool assigned, std::string& error) override
    {
        if constexpr (requires(Backend& b) { b.assign_input_output(scope, input_selector, output_selector, assigned, error); }) {
            return backend_->assign_input_output(scope, input_selector, output_selector, assigned, error);
        } else {
            return unsupported(error);
        }
    }

    bool load_config(std::string_view name, std::string& error) override
    {
        if constexpr (requires(Backend& b) { b.load_config(name, error); }) {
            return backend_->load_config(name, error);
        } else {
            return unsupported(error);
        }
    }

    bool save_config(std::string_view name, std::string& error) override
    {
        if constexpr (requires(Backend& b) { b.save_config(name, error); }) {
            return backend_->save_config(name, error);
        } else {
            return unsupported(error);
        }
    }

    bool load_default_config(std::string& name, std::string& error) override
    {
        if constexpr (requires(Backend& b) { b.load_default_config(name, error); }) {
            return backend_->load_default_config(name, error);
        } else {
            return unsupported(error);
        }
    }

    std::string list_inputs(std::size_t terminal_width = 100) const override
    {
        if constexpr (requires(Backend& b) { b.list_inputs(terminal_width); }) {
            return backend_->list_inputs(terminal_width);
        } else {
            return backend_->unsupported_operation() + "\n";
        }
    }

    std::string list_outputs(std::size_t terminal_width = 100) const override
    {
        if constexpr (requires(Backend& b) { b.list_outputs(terminal_width); }) {
            return backend_->list_outputs(terminal_width);
        } else {
            return backend_->unsupported_operation() + "\n";
        }
    }

    std::string list_meters(std::string_view selector = {}) const override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().list_meters(selector);
        } else if constexpr (requires(Backend& b) { b.list_meters(selector); }) {
            return backend_->list_meters(selector);
        } else {
            return backend_->unsupported_operation() + "\n";
        }
    }

    std::string fader_status() const override
    {
        if constexpr (MixingBackend<Backend>) {
            return backend_->channel_controls().fader_status();
        } else if constexpr (requires(Backend& b) { b.fader_status(); }) {
            return backend_->fader_status();
        } else {
            return backend_->unsupported_operation() + "\n";
        }
    }

    std::string health_status() const override
    {
        if constexpr (requires(Backend& b) { b.health_status(); }) {
            return backend_->health_status();
        } else {
            return backend_->unsupported_operation() + "\n";
        }
    }

    bool profile_start(std::string& error) override
    {
        if constexpr (requires(Backend& b) { b.profile_start(error); }) {
            return backend_->profile_start(error);
        } else {
            return unsupported(error);
        }
    }

    std::string profile_stop(std::string& error) override
    {
        if constexpr (requires(Backend& b) { b.profile_stop(error); }) {
            return backend_->profile_stop(error);
        } else {
            error = backend_->unsupported_operation();
            return {};
        }
    }

    bool available() const noexcept override
    {
        return backend_->available();
    }

    void begin_shutdown_fade() noexcept override
    {
        if constexpr (MixingBackend<Backend>) {
            backend_->channel_controls().begin_shutdown_fade();
        }
    }

    void pause_streams_for_shutdown() noexcept override
    {
        if constexpr (requires(Backend& b) { b.pause_streams_for_shutdown(); }) {
            backend_->pause_streams_for_shutdown();
        }
    }

private:
    bool unsupported(std::string& error) const
    {
        error = backend_->unsupported_operation();
        return false;
    }

    std::unique_ptr<Backend> backend_;
};

} // namespace shatter::mixer
