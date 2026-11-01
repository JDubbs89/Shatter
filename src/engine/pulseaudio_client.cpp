#include "pulseaudio_client.hpp"

#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/proplist.h>
#include <pulse/subscribe.h>
#include <pulse/thread-mainloop.h>
#include <pulse/timeval.h>

#include <atomic>
#include <mutex>
#include <utility>

namespace shatter::engine {
namespace {

std::string property(pa_proplist* properties, const char* key)
{
    const char* value = pa_proplist_gets(properties, key);
    return value == nullptr ? std::string{} : std::string(value);
}

struct OperationResult {
    pa_threaded_mainloop* mainloop = nullptr;
    bool complete = false;
    bool succeeded = false;
};

void operation_complete(pa_context*, int success, void* userdata)
{
    auto& result = *static_cast<OperationResult*>(userdata);
    result.complete = true;
    result.succeeded = success != 0;
    pa_threaded_mainloop_signal(result.mainloop, 0);
}

struct StreamQuery {
    pa_threaded_mainloop* mainloop = nullptr;
    pa_context* context = nullptr;
    std::vector<PulsePlaybackStream>* streams = nullptr;
    bool complete = false;
    bool succeeded = true;
    std::string error;
};

void stream_info(pa_context*, const pa_sink_input_info* info, int end_of_list, void* userdata)
{
    auto& query = *static_cast<StreamQuery*>(userdata);
    if (info != nullptr) {
        query.streams->push_back({
            .index = info->index,
            .sink_index = info->sink,
            .application = property(info->proplist, PA_PROP_APPLICATION_NAME),
            .process_binary = property(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY),
        });
    }
    if (end_of_list == 0) return;
    query.complete = true;
    query.succeeded = end_of_list > 0;
    if (!query.succeeded) {
        query.error = pa_strerror(pa_context_errno(query.context));
    }
    pa_threaded_mainloop_signal(query.mainloop, 0);
}

struct SinkQuery {
    pa_threaded_mainloop* mainloop = nullptr;
    pa_context* context = nullptr;
    std::vector<PulsePlaybackSink>* sinks = nullptr;
    bool complete = false;
    bool succeeded = true;
    std::string error;
};

void sink_info(pa_context*, const pa_sink_info* info, int end_of_list, void* userdata)
{
    auto& query = *static_cast<SinkQuery*>(userdata);
    if (info != nullptr) {
        PulsePlaybackSink sink;
        sink.index = info->index;
        if (info->name != nullptr) sink.name = info->name;
        if (info->description != nullptr) sink.description = info->description;
        if (info->latency != PA_USEC_INVALID) sink.current_latency_usec = info->latency;
        query.sinks->push_back(std::move(sink));
    }
    if (end_of_list == 0) return;
    query.complete = true;
    query.succeeded = end_of_list > 0;
    if (!query.succeeded) {
        query.error = pa_strerror(pa_context_errno(query.context));
    }
    pa_threaded_mainloop_signal(query.mainloop, 0);
}

struct ServerQuery {
    pa_threaded_mainloop* mainloop = nullptr;
    pa_context* context = nullptr;
    std::string* default_sink = nullptr;
    bool complete = false;
    bool succeeded = true;
    std::string error;
};

void server_info(pa_context*, const pa_server_info* info, void* userdata)
{
    auto& query = *static_cast<ServerQuery*>(userdata);
    if (info == nullptr || info->default_sink_name == nullptr) {
        query.succeeded = false;
        query.error = info == nullptr
            ? pa_strerror(pa_context_errno(query.context)) : "PulseAudio has no default sink";
    } else {
        *query.default_sink = info->default_sink_name;
    }
    query.complete = true;
    pa_threaded_mainloop_signal(query.mainloop, 0);
}

class LibpulseClient final : public PulseAudioClient {
public:
    ~LibpulseClient() override { stop(); }

    bool start(EventCallback on_server_change, std::string& error) override
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (mainloop_ != nullptr) {
            if (connected_.load(std::memory_order_acquire)) return true;
            stop_locked();
        }

        on_server_change_ = std::move(on_server_change);
        mainloop_ = pa_threaded_mainloop_new();
        if (mainloop_ == nullptr) {
            error = "could not create PulseAudio main loop";
            return false;
        }

        context_ = pa_context_new(pa_threaded_mainloop_get_api(mainloop_), "Shatter");
        if (context_ == nullptr) {
            error = "could not create PulseAudio connection context";
            stop_locked();
            return false;
        }
        pa_context_set_state_callback(context_, &LibpulseClient::context_state_changed, this);
        if (pa_threaded_mainloop_start(mainloop_) < 0) {
            error = "could not start PulseAudio main loop";
            pa_context_unref(context_);
            context_ = nullptr;
            pa_threaded_mainloop_free(mainloop_);
            mainloop_ = nullptr;
            return false;
        }
        mainloop_started_ = true;

        pa_threaded_mainloop_lock(mainloop_);
        if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            error = context_error();
            pa_threaded_mainloop_unlock(mainloop_);
            stop_locked();
            return false;
        }
        while (true) {
            const auto state = pa_context_get_state(context_);
            if (state == PA_CONTEXT_READY) break;
            if (!PA_CONTEXT_IS_GOOD(state)) {
                error = context_error();
                pa_threaded_mainloop_unlock(mainloop_);
                stop_locked();
                return false;
            }
            pa_threaded_mainloop_wait(mainloop_);
        }
        connected_.store(true, std::memory_order_release);
        pa_context_set_subscribe_callback(context_, &LibpulseClient::subscription_changed, this);
        OperationResult subscription{.mainloop = mainloop_};
        const auto mask = static_cast<pa_subscription_mask_t>(
            PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER);
        pa_operation* operation = pa_context_subscribe(context_, mask,
                                                       &operation_complete, &subscription);
        if (operation == nullptr) {
            error = context_error();
            pa_threaded_mainloop_unlock(mainloop_);
            stop_locked();
            return false;
        }
        while (!subscription.complete) pa_threaded_mainloop_wait(mainloop_);
        pa_operation_unref(operation);
        if (!subscription.succeeded) {
            error = context_error();
            pa_threaded_mainloop_unlock(mainloop_);
            stop_locked();
            return false;
        }
        pa_threaded_mainloop_unlock(mainloop_);
        return true;
    }

    void stop() override
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        stop_locked();
    }

    bool connected() const override
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        return connected_.load(std::memory_order_acquire);
    }

    bool playback_streams(std::vector<PulsePlaybackStream>& streams, std::string& error) override
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (!lock_ready(error)) return false;
        StreamQuery query{.mainloop = mainloop_, .context = context_, .streams = &streams,
                          .complete = false, .succeeded = true, .error = {}};
        pa_operation* operation = pa_context_get_sink_input_info_list(context_, &stream_info, &query);
        const bool result = wait_query(operation, query.complete, query.succeeded, query.error, error);
        pa_threaded_mainloop_unlock(mainloop_);
        return result;
    }

    bool sinks(std::vector<PulsePlaybackSink>& sinks, std::string& error) override
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (!lock_ready(error)) return false;
        SinkQuery query{.mainloop = mainloop_, .context = context_, .sinks = &sinks,
                        .complete = false, .succeeded = true, .error = {}};
        pa_operation* operation = pa_context_get_sink_info_list(context_, &sink_info, &query);
        const bool result = wait_query(operation, query.complete, query.succeeded, query.error, error);
        pa_threaded_mainloop_unlock(mainloop_);
        return result;
    }

    bool default_sink(std::string& name, std::string& error) override
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (!lock_ready(error)) return false;
        ServerQuery query{.mainloop = mainloop_, .context = context_, .default_sink = &name,
                          .complete = false, .succeeded = true, .error = {}};
        pa_operation* operation = pa_context_get_server_info(context_, &server_info, &query);
        const bool result = wait_query(operation, query.complete, query.succeeded, query.error, error);
        pa_threaded_mainloop_unlock(mainloop_);
        return result;
    }

    bool move_stream(std::uint32_t stream_index, std::string_view sink_name,
                     std::string& error) override
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (!lock_ready(error)) return false;
        const std::string target(sink_name);
        OperationResult operation_result{.mainloop = mainloop_};
        pa_operation* operation = pa_context_move_sink_input_by_name(
            context_, stream_index, target.c_str(), &operation_complete, &operation_result);
        const bool result = wait_query(operation, operation_result.complete,
                                       operation_result.succeeded, {}, error);
        pa_threaded_mainloop_unlock(mainloop_);
        return result;
    }

private:
    static void context_state_changed(pa_context* context, void* userdata)
    {
        auto& self = *static_cast<LibpulseClient*>(userdata);
        const auto state = pa_context_get_state(context);
        self.connected_.store(state == PA_CONTEXT_READY, std::memory_order_release);
        pa_threaded_mainloop_signal(self.mainloop_, 0);
        if (self.on_server_change_) self.on_server_change_();
    }

    static void subscription_changed(pa_context*, pa_subscription_event_type_t, std::uint32_t,
                                     void* userdata)
    {
        auto& self = *static_cast<LibpulseClient*>(userdata);
        if (self.on_server_change_) self.on_server_change_();
    }

    std::string context_error() const
    {
        return context_ == nullptr ? "PulseAudio connection is unavailable"
                                   : pa_strerror(pa_context_errno(context_));
    }

    bool lock_ready(std::string& error)
    {
        if (mainloop_ == nullptr || context_ == nullptr) {
            error = "PulseAudio connection is unavailable";
            return false;
        }
        pa_threaded_mainloop_lock(mainloop_);
        if (!connected_.load(std::memory_order_acquire) ||
            pa_context_get_state(context_) != PA_CONTEXT_READY) {
            error = context_error();
            pa_threaded_mainloop_unlock(mainloop_);
            return false;
        }
        return true;
    }

    bool wait_query(pa_operation* operation, bool& complete, bool& succeeded,
                    const std::string& query_error, std::string& error)
    {
        if (operation == nullptr) {
            error = context_error();
            return false;
        }
        while (!complete) pa_threaded_mainloop_wait(mainloop_);
        pa_operation_unref(operation);
        if (!succeeded) {
            error = query_error.empty() ? context_error() : query_error;
            return false;
        }
        return true;
    }

    void stop_locked()
    {
        connected_.store(false, std::memory_order_release);
        if (mainloop_ == nullptr) return;
        if (context_ != nullptr) {
            if (mainloop_started_) pa_threaded_mainloop_lock(mainloop_);
            pa_context_set_state_callback(context_, nullptr, nullptr);
            pa_context_set_subscribe_callback(context_, nullptr, nullptr);
            pa_context_disconnect(context_);
            if (mainloop_started_) pa_threaded_mainloop_unlock(mainloop_);
            pa_context_unref(context_);
            context_ = nullptr;
        }
        if (mainloop_started_) {
            pa_threaded_mainloop_stop(mainloop_);
            mainloop_started_ = false;
        }
        pa_threaded_mainloop_free(mainloop_);
        mainloop_ = nullptr;
    }

    mutable std::mutex lifecycle_mutex_;
    pa_threaded_mainloop* mainloop_ = nullptr;
    bool mainloop_started_ = false;
    pa_context* context_ = nullptr;
    std::atomic<bool> connected_{false};
    EventCallback on_server_change_;
};

} // namespace

std::unique_ptr<PulseAudioClient> create_pulse_audio_client()
{
    return std::make_unique<LibpulseClient>();
}

} // namespace shatter::engine
