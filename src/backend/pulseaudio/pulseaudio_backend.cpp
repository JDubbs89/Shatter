#include "backend/pulseaudio/pulseaudio_client.hpp"

#include "backend/iaudio_backend.hpp"
#include "mixer/engine.hpp"
#include "mixer/health.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace shatter::backend {
namespace {

std::string backend_unsupported_message()
{
    return "PulseAudio currently supports playback stream discovery and routing only";
}

class PulseAudioBackend final : public IAudioBackend {
public:
    static std::string unsupported_operation()
    {
        return "PulseAudio currently supports playback stream discovery and routing only";
    }
    bool enumerate_endpoints(std::vector<Endpoint>& endpoints, std::string& error) const override
    {
        std::vector<PulsePlaybackStream> streams;
        std::vector<PulsePlaybackSink> sinks;
        if (!client_->playback_streams(streams, error) || !client_->sinks(sinks, error)) return false;
        std::string default_name, default_error;
        const bool known_default = client_->default_sink(default_name, default_error);
        std::vector<Endpoint> result;
        for (const auto& sink : sinks) {
            Endpoint endpoint{sink.name, sink.name, sink.description,
                              EndpointKind::playback_device, {}, {}};
            if (known_default) endpoint.is_default = sink.name == default_name;
            result.push_back(std::move(endpoint));
        }
        for (const auto& stream : streams) {
            const auto id = std::to_string(stream.index);
            result.push_back({id, id, stream.application, EndpointKind::playback_stream, {}, {}});
        }
        endpoints = std::move(result);
        error.clear();
        return true;
    }

    PulseAudioBackend()
        : client_(create_pulse_audio_client())
    {
    }

    ~PulseAudioBackend() override { stop(); }

    bool start(std::string& error) override
    {
        if (started_.load(std::memory_order_acquire)) return true;
        if (!client_->start([this] { wake_routing_worker(); }, error)) return false;

        {
            std::lock_guard lock(worker_mutex_);
            stop_worker_ = false;
        }
        routing_worker_ = std::thread([this] { routing_worker_loop(); });
        started_.store(true, std::memory_order_release);
        wake_routing_worker();
        return true;
    }

    void stop() override
    {
        {
            std::lock_guard lock(worker_mutex_);
            stop_worker_ = true;
        }
        worker_condition_.notify_all();
        if (routing_worker_.joinable()) routing_worker_.join();
        client_->stop();
        started_.store(false, std::memory_order_release);
    }

    bool assign_app_output(std::string_view app_selector, std::string_view sink_selector,
                           bool assigned, std::string& error)
    {
        std::vector<PulsePlaybackStream> streams;
        if (!client_->playback_streams(streams, error)) return false;
        const auto matching_streams = std::count_if(streams.begin(), streams.end(), [&](const auto& stream) {
            return matches_pulse_playback_selector(stream, app_selector);
        });
        if (matching_streams == 0) {
            error = "no active PulseAudio playback streams match app selector '" +
                std::string(app_selector) + "'";
            return false;
        }

        std::vector<PulsePlaybackSink> sinks;
        if (!client_->sinks(sinks, error)) return false;

        std::string destination_name;
        if (assigned) {
            std::vector<const PulsePlaybackSink*> matches;
            for (const auto& sink : sinks) {
                if (sink.name == sink_selector || sink.description == sink_selector)
                    matches.push_back(&sink);
            }
            if (matches.empty()) {
                error = "no PulseAudio sink matches selector '" + std::string(sink_selector) + "'";
                return false;
            }
            if (matches.size() > 1) {
                error = "PulseAudio sink selector '" + std::string(sink_selector) + "' is ambiguous";
                return false;
            }
            destination_name = matches.front()->name;
        } else {
            if (!client_->default_sink(destination_name, error)) return false;
            const auto default_sink = std::find_if(sinks.begin(), sinks.end(), [&](const auto& sink) {
                return sink.name == destination_name;
            });
            if (default_sink == sinks.end()) {
                error = "PulseAudio default sink '" + destination_name + "' is not available";
                return false;
            }
        }

        bool succeeded = true;
        for (const auto& stream : streams) {
            if (!matches_pulse_playback_selector(stream, app_selector)) continue;
            const auto destination = std::find_if(sinks.begin(), sinks.end(), [&](const auto& sink) {
                return sink.name == destination_name;
            });
            if (destination == sinks.end()) {
                error = "PulseAudio sink '" + destination_name + "' disappeared during routing";
                return false;
            }
            if (stream.sink_index == destination->index) continue;
            if (!client_->move_stream(stream.index, destination_name, error)) {
                error = "cannot route PulseAudio stream for '" + std::string(app_selector) +
                    "': " + error;
                succeeded = false;
                break;
            }
        }
        if (succeeded && assigned) {
            std::lock_guard lock(routes_mutex_);
            routing_rules_[std::string(app_selector)] = destination_name;
        } else if (!assigned) {
            std::lock_guard lock(routes_mutex_);
            routing_rules_.erase(std::string(app_selector));
        }
        wake_routing_worker();
        return succeeded;
    }

    std::string list_inputs(std::size_t) const
    {
        std::vector<PulsePlaybackStream> streams;
        std::string error;
        if (!client_->playback_streams(streams, error))
            return "could not list PulseAudio playback streams: " + error + "\n";

        std::map<std::string, std::pair<std::string, std::size_t>> applications;
        for (const auto& stream : streams) {
            const std::string selector = stream.process_binary.empty()
                ? stream.application : stream.process_binary;
            auto& application = applications[selector];
            if (application.first.empty())
                application.first = stream.application.empty() ? selector : stream.application;
            ++application.second;
        }

        std::ostringstream output;
        output << "PulseAudio playback streams\n";
        output << "App selector | Application | Active streams\n";
        for (const auto& [selector, application] : applications)
            output << selector << " | " << application.first << " | " << application.second << '\n';
        if (applications.empty()) output << "No app streams discovered\n";
        return output.str();
    }

    std::string list_meters(std::string_view) const
    {
        return "Live peak/RMS meters are unavailable for the PulseAudio backend.\n";
    }

    std::string list_outputs(std::size_t) const
    {
        std::vector<PulsePlaybackSink> sinks;
        std::string error;
        if (!client_->sinks(sinks, error))
            return "could not list PulseAudio sinks: " + error + "\n";
        std::string default_name;
        const bool has_default = client_->default_sink(default_name, error);

        std::ostringstream output;
        output << "PulseAudio playback sinks\n";
        output << "Sink selector | Description | Status\n";
        for (const auto& sink : sinks)
            output << sink.name << " | " << sink.description << " | "
                   << (has_default && sink.name == default_name ? "default" : "available") << '\n';
        if (sinks.empty()) output << "No playback sinks found\n";
        return output.str();
    }

    std::string fader_status() const { return backend_unsupported_message() + "\n"; }
    std::string health_status() const
    {
        std::vector<PulsePlaybackSink> sinks;
        std::string error;
        const bool sinks_read = started_.load(std::memory_order_acquire) &&
            client_->connected() && client_->sinks(sinks, error);
        std::string background_error;
        {
            std::lock_guard lock(routes_mutex_);
            background_error = background_error_;
        }
        const auto state = mixer::health_state(started_.load(std::memory_order_acquire),
                                        sinks_read && !sinks.empty(), 0);
        std::ostringstream report;
        report << std::fixed << std::setprecision(3);
        report << "Engine health: " << mixer::health_state_name(state) << '\n';
        if (!started_.load(std::memory_order_acquire))
            report << "Reason: PulseAudio backend did not open successfully\n";
        else if (!client_->connected())
            report << "Issue: PulseAudio server connection is unavailable\n";
        else if (!sinks_read)
            report << "Issue: could not query PulseAudio sinks: " << error << '\n';
        else if (sinks.empty())
            report << "Issue: no PulseAudio playback sinks are available\n";
        else if (!background_error.empty())
            report << "Issue: " << background_error << '\n';
        else
            report << "Issue: none detected\n";

        report << "Xruns since engine start: unavailable (PulseAudio backend has no Shatter ring buffers)\n";
        if (sinks_read && !sinks.empty()) {
            const PulsePlaybackSink* measured_sink = nullptr;
            for (const auto& sink : sinks) {
                if (sink.current_latency_usec && (measured_sink == nullptr ||
                    *sink.current_latency_usec > *measured_sink->current_latency_usec))
                    measured_sink = &sink;
            }
            if (measured_sink != nullptr) {
                report << "I/O latency: " << *measured_sink->current_latency_usec / 1000.0
                       << " ms (current PulseAudio sink query, sink " << measured_sink->name << ")\n";
            } else {
                report << "I/O latency: unavailable (PulseAudio did not report current sink latency)\n";
            }
        } else {
            report << "I/O latency: unavailable (no playback sink is available to measure)\n";
        }
        return report.str();
    }

    std::string profile_stop(std::string& error) { error = backend_unsupported_message(); return {}; }
    bool available() const noexcept override
    {
        return started_.load(std::memory_order_acquire) && client_->connected();
    }

private:

    void wake_routing_worker()
    {
        worker_condition_.notify_one();
    }

    void routing_worker_loop()
    {
        std::unique_lock lock(worker_mutex_);
        while (!stop_worker_) {
            worker_condition_.wait_for(lock, std::chrono::milliseconds(500));
            if (stop_worker_) break;
            lock.unlock();
            reconcile_routing_rules();
            lock.lock();
        }
    }

    void reconcile_routing_rules()
    {
        if (!client_->connected()) {
            std::string reconnect_error;
            if (!client_->start([this] { wake_routing_worker(); }, reconnect_error)) {
                std::lock_guard lock(routes_mutex_);
                background_error_ = "PulseAudio reconnect failed: " + reconnect_error;
                return;
            }
        }

        std::map<std::string, std::string> rules;
        {
            std::lock_guard lock(routes_mutex_);
            rules = routing_rules_;
        }
        if (rules.empty()) return;

        std::vector<PulsePlaybackStream> streams;
        std::vector<PulsePlaybackSink> sinks;
        std::string error;
        if (!client_->playback_streams(streams, error) || !client_->sinks(sinks, error)) {
            std::lock_guard lock(routes_mutex_);
            background_error_ = "PulseAudio route reconciliation failed: " + error;
            return;
        }

        for (const auto& [selector, sink_name] : rules) {
            const auto destination = std::find_if(sinks.begin(), sinks.end(), [&](const auto& sink) {
                return sink.name == sink_name;
            });
            if (destination == sinks.end()) continue;
            for (const auto& stream : streams) {
                if (!matches_pulse_playback_selector(stream, selector) ||
                    stream.sink_index == destination->index) continue;
                if (!client_->move_stream(stream.index, sink_name, error)) {
                    std::lock_guard lock(routes_mutex_);
                    background_error_ = "PulseAudio route reconciliation failed: " + error;
                    return;
                }
            }
        }
        std::lock_guard lock(routes_mutex_);
        background_error_.clear();
    }

    std::unique_ptr<PulseAudioClient> client_;
    std::atomic<bool> started_{false};
    mutable std::mutex routes_mutex_;
    std::map<std::string, std::string> routing_rules_;
    std::string background_error_;
    std::mutex worker_mutex_;
    std::condition_variable worker_condition_;
    bool stop_worker_ = true;
    std::thread routing_worker_;
};

} // namespace

} // namespace shatter::backend

namespace shatter::backend {
std::unique_ptr<mixer::IControl> create_pulseaudio_engine()
{
    return std::make_unique<mixer::Engine<PulseAudioBackend>>();
}
} // namespace shatter::backend
