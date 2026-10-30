#include "shatter/engine/pipewire_engine.hpp"

#include <pipewire/keys.h>
#include <pipewire/loop.h>
#include <pipewire/stream.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <sys/resource.h>
#include <utility>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>

namespace shatter::engine {
namespace {

constexpr char kSpeakerStreamName[] = "shatter.speakers";
constexpr char kTargetObjectKey[] = "target.object";
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr float kMinimumGainDb = -60.0F;
constexpr float kMaximumGainDb = 12.0F;
constexpr std::uint32_t kMixerClosed = 1U << 31U;
constexpr std::uint32_t kMixerUserMask = kMixerClosed - 1U;

spa_pod* build_stereo_format(spa_pod_builder* builder)
{
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kSampleRate;
    info.channels = kChannels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    return spa_format_audio_raw_build(builder, SPA_PARAM_EnumFormat, &info);
}

bool stream_is_ready(pw_stream_state state)
{
    return state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING;
}

std::string fit_column(std::string_view value, std::size_t width)
{
    if (value.size() <= width) return std::string(value);
    if (width < 4) return std::string(value.substr(0, width));
    return std::string(value.substr(0, width - 3)) + "...";
}

std::filesystem::path config_path(std::string_view name)
{
    const char* base = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path root = base && *base ? base : std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".config";
    std::string filename(name);
    if (filename.ends_with(".conf")) filename.resize(filename.size() - 5);
    if (filename.empty() || filename == "." || filename == ".." || filename.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos)
        throw std::runtime_error("config name may contain only letters, digits, dot, dash, and underscore");
    return root / "shatter" / (filename + ".conf");
}

void trace_output(const char* stage, const void* request, std::string_view detail = {})
{
    if (std::getenv("SHATTER_TRACE_OUTPUT") == nullptr) return;
    const char* path = std::getenv("SHATTER_TRACE_OUTPUT_FILE");
    if (path == nullptr || *path == '\0') path = "/tmp/shatter-output-trace.log";
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    dprintf(fd, "[output-trace] pid=%ld request=%p stage=%s detail=%.*s\n", static_cast<long>(getpid()),
            request, stage, static_cast<int>(detail.size()), detail.data());
    ::close(fd);
}

float gain_db(float linear)
{
    return linear <= 0.0F ? -60.0F : std::max(-60.0F, 20.0F * std::log10(linear));
}

std::string make_channel_id(std::string_view identity)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : identity) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    std::ostringstream result;
    result << "app-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return result.str();
}

} // namespace

PipeWireEngine::~PipeWireEngine()
{
    stop();
}

bool PipeWireEngine::start(std::string& error)
{
    if (available_) {
        return true;
    }

    OutputChannel& speakers = output_channels_[0];
    speakers.engine = this;
    speakers.in_use = true;
    speakers.name = "speakers";
    speakers.node_name = kSpeakerStreamName;

    pw_init(nullptr, nullptr);
    main_loop_ = pw_main_loop_new(nullptr);
    if (main_loop_ == nullptr) {
        error = "could not create PipeWire main loop";
        cleanup();
        return false;
    }

    context_ = pw_context_new(pw_main_loop_get_loop(main_loop_), nullptr, 0);
    if (context_ == nullptr) {
        error = "could not create PipeWire context";
        cleanup();
        return false;
    }
    core_ = pw_context_connect(context_, nullptr, 0);
    if (core_ == nullptr) {
        error = "could not connect to PipeWire";
        cleanup();
        return false;
    }

    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    registry_events_.version = PW_VERSION_REGISTRY_EVENTS;
    registry_events_.global = &PipeWireEngine::on_global;
    registry_events_.global_remove = &PipeWireEngine::on_global_remove;
    pw_registry_add_listener(registry_, &registry_listener_, &registry_events_, this);

    if (!create_output_stream(speakers, true, error)) {
        if (error.empty()) error = "could not create the speaker output stream";
        cleanup();
        return false;
    }

    loop_thread_ = std::thread(&PipeWireEngine::run_loop, this);
    std::unique_lock lock(state_mutex_);
    const bool ready = state_changed_.wait_for(lock, std::chrono::seconds(5), [this] {
        return !startup_error_.empty() || (output_ready_ && output_linked_.load());
    });
    if (!ready || !startup_error_.empty()) {
        error = !startup_error_.empty() ? startup_error_
               : output_ready_ && !output_linked_.load()
                   ? "speaker playback stream is not linked to a PipeWire sink"
                   : "PipeWire speaker stream did not become ready";
        lock.unlock();
        cleanup();
        return false;
    }

    available_ = true;
    return true;
}

void PipeWireEngine::stop()
{
    if (main_loop_ == nullptr && context_ == nullptr && core_ == nullptr) {
        return;
    }
    cleanup();
}

bool PipeWireEngine::set_app_gain_db(std::string_view channel_id, float decibels, std::string& error)
{
    if (!std::isfinite(decibels) || decibels < kMinimumGainDb || decibels > kMaximumGainDb) {
        error = "gain must be between -60 dB and +12 dB";
        return false;
    }
    std::lock_guard lock(nodes_mutex_);
    AppChannel* channel = resolve_channel_locked(channel_id, error);
    if (channel == nullptr) {
        return false;
    }
    channel->target_gain.store(std::pow(10.0F, decibels / 20.0F), std::memory_order_relaxed);
    return true;
}

bool PipeWireEngine::set_stream_gain_db(std::string_view stream_selector, float decibels, std::string& error)
{
    if (!std::isfinite(decibels) || decibels < kMinimumGainDb || decibels > kMaximumGainDb) {
        error = "gain must be between -60 dB and +12 dB";
        return false;
    }
    std::lock_guard lock(nodes_mutex_);
    PlaybackStream* stream = resolve_stream_locked(stream_selector, error);
    if (stream == nullptr || !stream->active.load(std::memory_order_relaxed)) {
        if (error.empty()) error = "unknown or inactive playback stream: " + std::string(stream_selector);
        return false;
    }
    stream->target_gain.store(std::pow(10.0F, decibels / 20.0F), std::memory_order_relaxed);
    return true;
}

bool PipeWireEngine::set_app_muted(std::string_view channel_id, bool muted, std::string& error)
{
    std::lock_guard lock(nodes_mutex_);
    AppChannel* channel = resolve_channel_locked(channel_id, error);
    if (channel == nullptr) {
        return false;
    }
    channel->muted.store(muted, std::memory_order_relaxed);
    return true;
}

bool PipeWireEngine::set_stream_muted(std::string_view stream_selector, bool muted, std::string& error)
{
    std::lock_guard lock(nodes_mutex_);
    PlaybackStream* stream = resolve_stream_locked(stream_selector, error);
    if (stream == nullptr || !stream->active.load(std::memory_order_relaxed)) {
        if (error.empty()) error = "unknown or inactive playback stream: " + std::string(stream_selector);
        return false;
    }
    stream->muted.store(muted, std::memory_order_relaxed);
    return true;
}

bool PipeWireEngine::rename_app(std::string_view current_name, std::string new_name, std::string& error)
{
    if (new_name.empty()) {
        error = "new app name cannot be empty";
        return false;
    }
    std::lock_guard lock(nodes_mutex_);
    AppChannel* channel = resolve_channel_locked(current_name, error);
    if (channel == nullptr) return false;
    for (std::size_t index = 0; index < allocated_channels_.load(std::memory_order_relaxed); ++index) {
        if (&app_channels_[index] != channel && app_channels_[index].name == new_name) {
            error = "another app channel already uses the name: " + new_name;
            return false;
        }
    }
    channel->name = std::move(new_name);
    return true;
}

bool PipeWireEngine::rename_stream(std::string_view current_name, std::string new_name, std::string& error)
{
    if (new_name.empty()) {
        error = "new stream name cannot be empty";
        return false;
    }
    std::lock_guard lock(nodes_mutex_);
    PlaybackStream* stream = resolve_stream_locked(current_name, error);
    if (stream == nullptr) return false;
    for (std::size_t index = 0; index < allocated_streams_.load(std::memory_order_relaxed); ++index) {
        if (&playback_streams_[index] != stream && playback_streams_[index].in_use &&
            playback_streams_[index].name == new_name) {
            error = "another playback stream already uses the name: " + new_name;
            return false;
        }
    }
    stream->name = std::move(new_name);
    return true;
}

bool PipeWireEngine::set_capture_source(std::uint32_t source_node_id, std::string& error)
{
    if (main_loop_ == nullptr) {
        error = "PipeWire engine is not running";
        return false;
    }
    CaptureSourceRequest request{this, source_node_id, false, {}};
    const int result = pw_loop_invoke(pw_main_loop_get_loop(main_loop_),
        &PipeWireEngine::on_capture_source_request, 0, nullptr, 0, true, &request);
    if (result < 0) {
        error = "could not schedule capture source selection on the PipeWire loop";
        return false;
    }
    error = std::move(request.error);
    return request.succeeded;
}

bool PipeWireEngine::make_output(std::string name, std::string& error)
{
    OutputRequest request{};
    request.engine = this;
    request.action = OutputRequest::Action::make;
    request.first = std::move(name);
    return invoke_output_request(request, error);
}

bool PipeWireEngine::delete_output(std::string_view selector, std::string& error)
{
    trace_output("delete.enter", this, selector);
    OutputRequest request{};
    request.engine = this;
    request.action = OutputRequest::Action::remove;
    request.first = std::string(selector);
    return invoke_output_request(request, error);
}

bool PipeWireEngine::assign_output_sink(std::string_view output_selector, std::string_view sink_selector,
                                        bool assigned, std::string& error)
{
    OutputRequest request{};
    request.engine = this;
    request.action = OutputRequest::Action::assign_sink;
    request.first = std::string(output_selector);
    request.second = std::string(sink_selector);
    request.assigned = assigned;
    return invoke_output_request(request, error);
}

bool PipeWireEngine::assign_app_output(std::string_view app_selector, std::string_view output_selector,
                                       bool assigned, std::string& error)
{
    OutputRequest request{};
    request.engine = this;
    request.action = OutputRequest::Action::assign_app;
    request.first = std::string(app_selector);
    request.second = std::string(output_selector);
    request.assigned = assigned;
    return invoke_output_request(request, error);
}

bool PipeWireEngine::invoke_output_request(OutputRequest& request, std::string& error)
{
    if (main_loop_ == nullptr) { error = "PipeWire engine is not running"; return false; }
    const int result = pw_loop_invoke(pw_main_loop_get_loop(main_loop_), &PipeWireEngine::on_output_request,
                                      0, nullptr, 0, true, &request);
    if (result < 0) {
        error = "could not schedule the output operation on the PipeWire loop";
        return false;
    }
    {
        std::unique_lock lock(request.completion_mutex);
        request.completion_cv.wait(lock, [&request] { return request.completed; });
    }
    error = std::move(request.error);
    if (!request.succeeded && error.empty()) {
        switch (request.action) {
        case OutputRequest::Action::make: error = "PipeWire could not create the output channel"; break;
        case OutputRequest::Action::remove: error = "PipeWire could not delete the output channel"; break;
        case OutputRequest::Action::assign_sink: error = "PipeWire could not update the output sink route"; break;
        case OutputRequest::Action::assign_app: error = "PipeWire could not update the app channel's output sends"; break;
        }
    }
    trace_output("operation.invoke-return", &request,
                 "action=" + std::to_string(static_cast<int>(request.action)) +
                 " result=" + std::to_string(result) + " succeeded=" + std::to_string(request.succeeded) +
                 " error=" + error);
    return request.succeeded;
}

int PipeWireEngine::on_output_request(spa_loop*, bool, std::uint32_t, const void*, std::size_t, void* user_data)
{
    auto& request = *static_cast<OutputRequest*>(user_data);
    auto& engine = *request.engine;
    trace_output("loop.callback-enter", &request,
                 "action=" + std::to_string(static_cast<int>(request.action)) + " first=" + request.first);
    switch (request.action) {
    case OutputRequest::Action::make:
        request.succeeded = engine.make_output_locked(std::move(request.first), request.error);
        break;
    case OutputRequest::Action::remove:
        request.succeeded = engine.delete_output_locked(request.first, request.error);
        break;
    case OutputRequest::Action::assign_sink: {
        std::lock_guard lock(engine.nodes_mutex_);
        request.succeeded = engine.assign_output_sink_locked(request.first, request.second,
                                                              request.assigned, request.error);
        break;
    }
    case OutputRequest::Action::assign_app: {
        std::lock_guard lock(engine.nodes_mutex_);
        request.succeeded = engine.assign_app_output_locked(request.first, request.second,
                                                             request.assigned, request.error);
        break;
    }
    }
    trace_output("loop.callback-exit", &request,
                 "action=" + std::to_string(static_cast<int>(request.action)) +
                 " succeeded=" + std::to_string(request.succeeded) + " error=" + request.error);
    {
        std::lock_guard lock(request.completion_mutex);
        request.completed = true;
    }
    request.completion_cv.notify_one();
    return 0;
}

bool PipeWireEngine::make_output_locked(std::string name, std::string& error)
{
    if (name.empty() || name.size() > 128 || name.find_first_of("\r\n") != std::string::npos ||
        name.find('\0') != std::string::npos) {
        error = "output name must be 1 to 128 characters and cannot contain line breaks";
        return false;
    }
    std::uint32_t numeric_selector = 0;
    const auto [numeric_end, numeric_error] = std::from_chars(name.data(), name.data() + name.size(), numeric_selector);
    if ((numeric_error == std::errc{} && numeric_end == name.data() + name.size()) ||
        name == kSpeakerStreamName ||
        name.starts_with("shatter.output.")) {
        error = "output name cannot look like a PipeWire node ID or generated output node name";
        return false;
    }
    std::unique_lock lock(nodes_mutex_);
    for (const auto& output : output_channels_) {
        if (output.in_use && output.name == name) { error = "an output channel already uses the name: " + name; return false; }
    }
    for (std::size_t index = 1; index < output_channels_.size(); ++index) {
        OutputChannel& output = output_channels_[index];
        if (output.in_use) continue;
        output.engine = this;
        output.in_use = true;
        output.input_users.store(0, std::memory_order_release);
        output.name = std::move(name);
        output.node_name = "shatter.output." + std::to_string(index);
        lock.unlock();
        if (!create_output_stream(output, false, error)) {
            lock.lock();
            output.in_use = false;
            output.name.clear();
            output.node_name.clear();
            return false;
        }
        return true;
    }
    error = "Shatter reached its output channel limit";
    return false;
}

bool PipeWireEngine::delete_output_locked(std::string_view selector, std::string& error)
{
    trace_output("delete.locked-enter", this, selector);
    std::unique_lock lock(nodes_mutex_);
    OutputChannel* output = resolve_output_locked(selector, error);
    trace_output("delete.resolved", output,
                 "selector=" + std::string(selector) + " error=" + error +
                 " channel=" + (output ? output->name : "<null>"));
    if (output == nullptr) return false;
    if (output == &output_channels_[0]) { error = "the built-in speakers output cannot be deleted"; return false; }
    const auto index = static_cast<std::size_t>(output - output_channels_.data());
    const std::uint32_t bit = 1U << index;
    for (std::size_t i = 0; i < allocated_channels_.load(std::memory_order_relaxed); ++i)
        app_channels_[i].output_mask.fetch_and(~bit, std::memory_order_release);
    capture_channel_.output_mask.fetch_and(~bit, std::memory_order_release);
    output->linked.store(false, std::memory_order_release);
    trace_output("delete.before-destroy", output, output->node_name);
    lock.unlock();
    destroy_output_stream(*output);
    trace_output("delete.after-destroy", output, output->node_name);
    for (auto& stream : playback_streams_) {
        stream.audio[index].discard_all();
    }
    capture_channel_.audio[index].discard_all();
    lock.lock();
    output->in_use = false;
    output->name.clear();
    output->node_name.clear();
    output->node_id = PW_ID_ANY;
    trace_output("delete.complete", output);
    return true;
}

PipeWireEngine::OutputChannel* PipeWireEngine::resolve_output_locked(std::string_view selector,
                                                                    std::string& error)
{
    std::uint32_t node_id = PW_ID_ANY;
    const auto [end, parse_error] = std::from_chars(selector.data(), selector.data() + selector.size(), node_id);
    for (auto& output : output_channels_) {
        if (!output.in_use) continue;
        if (output.name == selector || output.node_name == selector ||
            (parse_error == std::errc{} && end == selector.data() + selector.size() && output.node_id == node_id))
            return &output;
    }
    error = "unknown output channel or PipeWire node: " + std::string(selector);
    return nullptr;
}

bool PipeWireEngine::assign_output_sink_locked(std::string_view output_selector,
                                               std::string_view sink_selector, bool assigned,
                                               std::string& error)
{
    OutputChannel* output = resolve_output_locked(output_selector, error);
    if (output == nullptr) return false;
    if (output->node_id == PW_ID_ANY || default_metadata_ == nullptr) {
        error = "output channel is not ready in PipeWire";
        return false;
    }
    NodeInfo* sink = nullptr;
    std::string_view id_text = sink_selector;
    if (id_text.starts_with("sink-")) id_text.remove_prefix(5);
    std::uint32_t sink_id = PW_ID_ANY;
    const auto [end, parse_error] = std::from_chars(id_text.data(), id_text.data() + id_text.size(), sink_id);
    if (parse_error == std::errc{} && end == id_text.data() + id_text.size()) {
        const auto found = nodes_.find(sink_id);
        if (found != nodes_.end() && found->second.media_class == "Audio/Sink" &&
            !found->second.name.starts_with("shatter.stream.")) sink = &found->second;
    } else {
        for (auto& [id, node] : nodes_) {
            (void)id;
            if (node.media_class != "Audio/Sink" || node.name.starts_with("shatter.stream.")) continue;
            if (node.name != sink_selector && node.description != sink_selector) continue;
            if (sink != nullptr) { error = "playback sink name is ambiguous; use its sink ID"; return false; }
            sink = &node;
        }
    }
    if (sink == nullptr) { error = "unknown PipeWire playback sink: " + std::string(sink_selector); return false; }
    if (!assigned && output->sink_node_id.load(std::memory_order_acquire) != sink->node_id) {
        error = "output channel is not assigned to playback sink: " + std::string(sink_selector);
        return false;
    }
    pw_properties* stream_properties = pw_properties_new(PW_KEY_NODE_AUTOCONNECT,
        assigned ? "true" : "false", nullptr);
    if (stream_properties == nullptr) { error = "could not allocate output routing properties"; return false; }
    const int property_result = pw_stream_update_properties(output->stream, &stream_properties->dict);
    pw_properties_free(stream_properties);
    if (property_result < 0) { error = "could not update output auto-connect policy"; return false; }
    const int result = pw_metadata_set_property(default_metadata_, output->node_id, kTargetObjectKey,
        "Spa:String", assigned ? sink->name.c_str() : "-1");
    if (result < 0) { error = "could not update the output channel's PipeWire sink assignment"; return false; }
    return true;
}

bool PipeWireEngine::assign_app_output_locked(std::string_view app_selector,
                                              std::string_view output_selector, bool assigned,
                                              std::string& error)
{
    AppChannel* app = resolve_channel_locked(app_selector, error);
    if (app == nullptr) return false;
    OutputChannel* output = resolve_output_locked(output_selector, error);
    if (output == nullptr) return false;
    const auto index = static_cast<std::size_t>(output - output_channels_.data());
    const auto bit = 1U << index;
    auto mask = app->output_mask.load(std::memory_order_relaxed);
    if (assigned) mask |= bit;
    else mask &= ~bit;
    app->output_mask.store(mask, std::memory_order_release);
    return true;
}

bool PipeWireEngine::create_output_stream(OutputChannel& output, bool autoconnect, std::string& error)
{
    output.events = {};
    output.events.version = PW_VERSION_STREAM_EVENTS;
    output.events.state_changed = &PipeWireEngine::on_output_state_changed;
    output.events.process = &PipeWireEngine::on_output_process;
    const std::string description = output.name == "speakers" ? "Shatter Speakers" : "Shatter output: " + output.name;
    pw_properties* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Production", PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
        PW_KEY_NODE_NAME, output.node_name.c_str(), PW_KEY_NODE_DESCRIPTION, description.c_str(),
        PW_KEY_NODE_AUTOCONNECT, "true", "node.dont-fallback", "true",
        PW_KEY_APP_NAME, "Shatter", nullptr);
    if (properties == nullptr) { error = "could not allocate PipeWire output properties"; return false; }
    output.stream = pw_stream_new(core_, description.c_str(), properties);
    if (output.stream == nullptr) { error = "could not create PipeWire output stream"; return false; }
    pw_stream_add_listener(output.stream, &output.listener, &output.events, &output);
    std::array<std::uint8_t, 1024> storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    const spa_pod* params[] = {build_stereo_format(&builder)};
    auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    if (autoconnect) flags = static_cast<pw_stream_flags>(flags | PW_STREAM_FLAG_AUTOCONNECT);
    if (pw_stream_connect(output.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
        pw_stream_destroy(output.stream);
        output.stream = nullptr;
        error = "could not connect PipeWire output stream";
        return false;
    }
    return true;
}

void PipeWireEngine::destroy_output_stream(OutputChannel& output)
{
    output.active.store(false, std::memory_order_release);
    output.linked.store(false, std::memory_order_release);
    output.sink_node_id.store(PW_ID_ANY, std::memory_order_release);
    output.mixer_users.fetch_or(kMixerClosed, std::memory_order_acq_rel);
    output.input_users.fetch_or(kMixerClosed, std::memory_order_acq_rel);
    if (output.stream != nullptr) {
        pw_stream_disconnect(output.stream);
        pw_stream_destroy(output.stream);
        output.stream = nullptr;
    }
    while ((output.mixer_users.load(std::memory_order_acquire) & kMixerUserMask) != 0)
        std::this_thread::yield();
    while ((output.input_users.load(std::memory_order_acquire) & kMixerUserMask) != 0)
        std::this_thread::yield();
    output.mixer_users.store(0, std::memory_order_release);
}

bool PipeWireEngine::set_capture_gain_db(float decibels, std::string& error)
{
    if (!std::isfinite(decibels) || decibels < kMinimumGainDb || decibels > kMaximumGainDb) {
        error = "gain must be between -60 dB and +12 dB";
        return false;
    }
    capture_channel_.target_gain.store(std::pow(10.0F, decibels / 20.0F), std::memory_order_relaxed);
    return true;
}

bool PipeWireEngine::set_capture_speaker_send(bool enabled, std::string& error)
{
    if (enabled && !capture_channel_.active.load(std::memory_order_acquire)) {
        error = "select an active capture source before enabling its speaker send";
        return false;
    }
    auto mask = capture_channel_.output_mask.load(std::memory_order_relaxed);
    if (enabled) mask |= 1U;
    else mask &= ~1U;
    capture_channel_.output_mask.store(mask, std::memory_order_release);
    return true;
}

bool PipeWireEngine::read_config(std::string_view name, std::vector<SavedApp>& apps,
                                 std::vector<SavedStream>& streams, std::string& error) const
{
    try {
        std::ifstream file(config_path(name));
        if (!file) { error = "configuration not found: " + std::string(name); return false; }
        std::string header;
        if (!std::getline(file, header) || header != "SHATTER-CONFIG 1") { error = "unsupported or invalid configuration format"; return false; }
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream row(line);
            char type = 0;
            if (!(row >> type)) continue;
            if (type == 'A') {
                SavedApp item; int muted = 0;
                if (!(row >> std::quoted(item.identity) >> std::quoted(item.name) >> item.gain_db >> muted) ||
                    (muted != 0 && muted != 1) || !std::isfinite(item.gain_db) || item.gain_db < kMinimumGainDb || item.gain_db > kMaximumGainDb) { error = "invalid app entry in configuration"; return false; }
                item.muted = muted != 0;
                apps.push_back(std::move(item));
            } else if (type == 'S') {
                SavedStream item; int muted = 0;
                if (!(row >> std::quoted(item.app_identity) >> std::quoted(item.original_name) >> std::quoted(item.name) >> item.gain_db >> muted) ||
                    (muted != 0 && muted != 1) || !std::isfinite(item.gain_db) || item.gain_db < kMinimumGainDb || item.gain_db > kMaximumGainDb) { error = "invalid stream entry in configuration"; return false; }
                item.muted = muted != 0; streams.push_back(std::move(item));
            } else { error = "unknown entry in configuration"; return false; }
            std::string extra; if (row >> extra) { error = "extra data in configuration entry"; return false; }
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PipeWireEngine::write_config(std::string_view name, std::string& error) const
{
    try {
        const auto path = config_path(name);
        std::filesystem::create_directories(path.parent_path());
        const auto temp = path.string() + ".tmp";
        std::ofstream file(temp, std::ios::trunc);
        if (!file) { error = "cannot write configuration file"; return false; }
        file << "SHATTER-CONFIG 1\n" << std::setprecision(7);
        std::lock_guard lock(nodes_mutex_);
        for (std::size_t i = 0; i < allocated_channels_.load(); ++i) {
            const auto& app = app_channels_[i]; if (!app.allocated) continue;
            file << "A " << std::quoted(app.identity) << ' ' << std::quoted(app.name) << ' '
                 << gain_db(app.target_gain.load()) << ' ' << (app.muted.load() ? 1 : 0) << '\n';
        }
        for (std::size_t i = 0; i < allocated_streams_.load(); ++i) {
            const auto& stream = playback_streams_[i]; if (!stream.in_use) continue;
            const auto& app = app_channels_[stream.channel_index];
            file << "S " << std::quoted(app.identity) << ' ' << std::quoted(stream.original_name) << ' '
                 << std::quoted(stream.name) << ' ' << gain_db(stream.target_gain.load()) << ' '
                 << (stream.muted.load() ? 1 : 0) << '\n';
        }
        file.close();
        if (!file) { error = "failed while writing configuration file"; return false; }
        std::filesystem::rename(temp, path);
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool PipeWireEngine::save_config(std::string_view name, std::string& error) { return write_config(name, error); }

bool PipeWireEngine::load_config(std::string_view name, std::string& error)
{
    std::vector<SavedApp> apps; std::vector<SavedStream> streams;
    if (!read_config(name, apps, streams, error)) return false;
    std::lock_guard lock(nodes_mutex_);
    saved_apps_ = std::move(apps); saved_streams_ = std::move(streams);
    auto gain = [](float db) { return std::pow(10.0F, db / 20.0F); };
    for (std::size_t i = 0; i < allocated_channels_.load(); ++i) for (const auto& saved : saved_apps_) {
        auto& app = app_channels_[i]; if (app.identity == saved.identity) { app.name = saved.name; app.target_gain.store(gain(saved.gain_db)); app.muted.store(saved.muted); }
    }
    for (std::size_t i = 0; i < allocated_streams_.load(); ++i) for (const auto& saved : saved_streams_) {
        auto& stream = playback_streams_[i]; if (!stream.in_use) continue;
        const auto& app = app_channels_[stream.channel_index];
        if (app.identity == saved.app_identity && stream.original_name == saved.original_name) {
            stream.name = saved.name; stream.target_gain.store(gain(saved.gain_db)); stream.muted.store(saved.muted);
        }
    }
    return true;
}

std::string PipeWireEngine::list_inputs(std::size_t terminal_width) const
{
    terminal_width = std::max<std::size_t>(72, std::min<std::size_t>(terminal_width, 240));
    const std::size_t stream_column = terminal_width - 59;
    std::lock_guard lock(nodes_mutex_);
    std::ostringstream table;
    table << "Shatter inputs\n"
          << std::left << std::setw(23) << "Channel ID" << std::setw(20) << "Application"
          << std::setw(9) << "Gain" << std::setw(7) << "Mute" << "Playback stream (PipeWire node ID)\n"
          << std::string(terminal_width, '-') << '\n';

    const auto channel_count = allocated_channels_.load(std::memory_order_relaxed);
    const auto stream_count = allocated_streams_.load(std::memory_order_relaxed);
    for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
        const AppChannel& channel = app_channels_[channel_index];
        table << std::setw(23) << channel.id << std::setw(20) << fit_column(channel.name, 19)
              << std::setw(9) << (std::to_string(gain_db(channel.target_gain.load())) + " dB")
              << std::setw(7) << (channel.muted.load() ? "yes" : "no");
        bool has_stream = false;
        for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
            const PlaybackStream& stream = playback_streams_[stream_index];
            if (!stream.active.load(std::memory_order_relaxed) || stream.channel_index != channel_index) {
                continue;
            }
            if (has_stream) {
                table << std::setw(23) << "" << std::setw(20) << "" << std::setw(9) << "" << std::setw(7) << "";
            }
            const std::string suffix = " (" + std::to_string(stream.node_id) + ")";
            table << fit_column(stream.name, stream_column > suffix.size() ? stream_column - suffix.size() : 1) << suffix << '\n';
            has_stream = true;
        }
        if (!has_stream) {
            table << "no active playback streams\n";
        }
    }
    if (channel_count == 0) {
        table << "No application playback streams have been discovered\n";
    }

    table << std::setw(23) << "capture" << std::setw(20) << "Microphone"
          << std::setw(9) << (std::to_string(gain_db(capture_channel_.target_gain.load())) + " dB")
          << std::setw(7) << (capture_channel_.muted.load() ? "yes" : "no");
    if (capture_channel_.source_node_id == PW_ID_ANY) {
        table << "No capture source selected; speaker send off\n";
    } else {
        const auto source = nodes_.find(capture_channel_.source_node_id);
        const std::string source_name = source == nodes_.end()
            ? "Capture source unavailable"
            : source->second.name.empty() ? source->second.description : source->second.name;
        table << source_name << " (" << capture_channel_.source_node_id << ") ("
              << (capture_channel_.active.load(std::memory_order_relaxed) ? "connected" : "connecting")
              << "; speaker send "
              << ((capture_channel_.output_mask.load(std::memory_order_relaxed) & 1U) != 0 ? "on" : "off")
              << ")\n";
    }

    std::vector<NodeInfo> capture_sources;
    for (const auto& [id, node] : nodes_) {
        (void)id;
        if (node.media_class == "Audio/Source") {
            capture_sources.push_back(node);
        }
    }
    std::sort(capture_sources.begin(), capture_sources.end(), [](const NodeInfo& left, const NodeInfo& right) {
        return left.name < right.name;
    });
    for (const auto& node : capture_sources) {
        table << std::setw(23) << "available" << std::setw(20) << "Capture source"
              << std::setw(9) << "" << std::setw(7) << ""
              << (node.name.empty() ? node.description : node.name) << " (" << node.node_id << ")"
              << (capture_channel_.source_node_id == node.node_id ? " (selected)\n" : " (available)\n");
    }
    if (capture_sources.empty()) {
        table << std::setw(23) << "available" << std::setw(20) << "Capture source"
              << std::setw(9) << "" << std::setw(7) << ""
              << "No PipeWire capture sources found\n";
    }
    const auto unrouted_count = unrouted_playback_streams_.load(std::memory_order_relaxed);
    if (unrouted_count > 0) {
        table << "Warning: " << unrouted_count
              << " playback streams could not be routed because Shatter reached its channel or stream limit\n";
    }
    return table.str();
}

std::string PipeWireEngine::list_outputs(std::size_t terminal_width) const
{
    std::lock_guard lock(nodes_mutex_);
    std::ostringstream table;
    const std::size_t width = std::max<std::size_t>(40, terminal_width);
    const std::size_t output_width = std::clamp<std::size_t>(width / 5, 12, 24);
    const std::size_t id_width = std::clamp<std::size_t>(width / 6, 10, 16);
    const std::size_t node_width = std::clamp<std::size_t>(width / 4, 10, 25);
    const std::size_t sink_width = width - output_width - id_width - node_width;
    table << "Shatter outputs\n"
          << std::left << std::setw(static_cast<int>(output_width)) << fit_column("Output channel", output_width - 1)
          << std::setw(static_cast<int>(id_width)) << fit_column("PipeWire node ID", id_width - 1)
          << std::setw(static_cast<int>(node_width)) << fit_column("PipeWire node name", node_width - 1)
          << fit_column("Assigned playback sink", sink_width) << '\n'
          << std::string(output_width + id_width + node_width + sink_width, '-') << '\n';
    for (const auto& output : output_channels_) {
        if (!output.in_use) continue;
        table << std::setw(static_cast<int>(output_width)) << fit_column(output.name, output_width - 1);
        if (output.node_id == PW_ID_ANY) table << std::setw(static_cast<int>(id_width)) << "pending";
        else table << std::setw(static_cast<int>(id_width)) << output.node_id;
        table << std::setw(static_cast<int>(node_width)) << fit_column(output.node_name, node_width - 1);
        std::string assigned_sinks;
        std::vector<std::uint32_t> listed_sink_ids;
        for (const auto& [link_id, link] : links_) {
            (void)link_id;
            if (link.output_node != output.node_id) continue;
            const auto target = nodes_.find(link.input_node);
            if (target == nodes_.end() || target->second.media_class != "Audio/Sink" ||
                target->second.name.starts_with("shatter.stream.")) continue;
            if (std::find(listed_sink_ids.begin(), listed_sink_ids.end(), link.input_node) != listed_sink_ids.end()) continue;
            listed_sink_ids.push_back(link.input_node);
            if (!assigned_sinks.empty()) assigned_sinks += ", ";
            assigned_sinks += (target->second.name.empty() ? target->second.description : target->second.name) +
                              " (sink-" + std::to_string(link.input_node) + ")";
        }
        if (assigned_sinks.empty()) assigned_sinks = "not assigned";
        if (output.node_name == kSpeakerStreamName) assigned_sinks += " [default]";
        table << fit_column(assigned_sinks, sink_width);
        table << '\n';
    }

    std::vector<NodeInfo> playback_sinks;
    for (const auto& [id, node] : nodes_) {
        (void)id;
        if (node.media_class == "Audio/Sink" && !node.name.starts_with("shatter.stream.")) {
            playback_sinks.push_back(node);
        }
    }
    std::sort(playback_sinks.begin(), playback_sinks.end(), [](const NodeInfo& left, const NodeInfo& right) {
        return left.name < right.name;
    });
    const std::size_t sink_id_width = std::clamp<std::size_t>(width / 5, 12, 18);
    const std::size_t sink_node_width = std::clamp<std::size_t>(width / 6, 10, 16);
    const std::size_t sink_name_width = width - sink_id_width - sink_node_width;
    table << "\nAvailable playback sinks\n"
          << std::left << std::setw(static_cast<int>(sink_id_width)) << fit_column("Shatter sink ID", sink_id_width - 1)
          << std::setw(static_cast<int>(sink_node_width)) << fit_column("PipeWire node ID", sink_node_width - 1)
          << fit_column("Name", sink_name_width) << '\n'
          << std::string(sink_id_width + sink_node_width + sink_name_width, '-') << '\n';
    for (const auto& node : playback_sinks) {
        table << std::setw(static_cast<int>(sink_id_width)) << fit_column("sink-" + std::to_string(node.node_id), sink_id_width - 1)
              << std::setw(static_cast<int>(sink_node_width)) << node.node_id
              << fit_column(node.name.empty() ? node.description : node.name, sink_name_width) << '\n';
    }
    if (playback_sinks.empty()) {
        table << "No PipeWire playback sinks found\n";
    }
    return table.str();
}

bool PipeWireEngine::available() const noexcept
{
    return available_ && output_linked_.load(std::memory_order_relaxed);
}

namespace {

std::uint64_t timeval_to_microseconds(const timeval& value)
{
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000ULL +
        static_cast<std::uint64_t>(value.tv_usec);
}

} // namespace

bool PipeWireEngine::profile_start(std::string& error)
{
    if (!available()) {
        error = "PipeWire audio is unavailable; start profiling after audio becomes ready";
        return false;
    }
    if (profile_running_.load(std::memory_order_acquire)) {
        error = "a profiling run is already active";
        return false;
    }

    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) < 0) {
        error = "could not read process CPU usage";
        return false;
    }
    profile_user_cpu_us_ = timeval_to_microseconds(usage.ru_utime);
    profile_system_cpu_us_ = timeval_to_microseconds(usage.ru_stime);
    profile_started_ = std::chrono::steady_clock::now();
    profiler_.start();
    profile_running_.store(true, std::memory_order_release);
    return true;
}

std::string PipeWireEngine::profile_stop(std::string& error)
{
    if (!profile_running_.exchange(false, std::memory_order_acq_rel)) {
        error = "no profiling run is active";
        return {};
    }
    profiler_.stop();
    const auto elapsed = std::chrono::steady_clock::now() - profile_started_;
    const auto wall_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());

    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) < 0) {
        error = "profiling stopped, but process CPU usage could not be read";
        return {};
    }
    const auto user_cpu_us = timeval_to_microseconds(usage.ru_utime) - profile_user_cpu_us_;
    const auto system_cpu_us = timeval_to_microseconds(usage.ru_stime) - profile_system_cpu_us_;
    const double cpu_seconds = static_cast<double>(user_cpu_us + system_cpu_us) / 1'000'000.0;
    const double cpu_percent = wall_ns == 0 ? 0.0 : cpu_seconds * 1'000'000'000.0 * 100.0 /
        static_cast<double>(wall_ns);
    return profiler_.report(wall_ns, cpu_seconds, cpu_percent);
}

void PipeWireEngine::on_output_state_changed(void* data, pw_stream_state, pw_stream_state state,
                                             const char* error)
{
    auto& output = *static_cast<OutputChannel*>(data);
    auto& engine = *output.engine;
    output.active.store(stream_is_ready(state), std::memory_order_release);
    if (stream_is_ready(state) && output.stream != nullptr) {
        output.node_id = pw_stream_get_node_id(output.stream);
        if (&output == &engine.output_channels_[0]) {
            engine.speaker_node_id_.store(output.node_id, std::memory_order_relaxed);
        }
        std::lock_guard lock(engine.nodes_mutex_);
        engine.refresh_output_link_locked();
    }
    {
        std::lock_guard lock(engine.state_mutex_);
        if (&output == &engine.output_channels_[0]) engine.output_ready_ = stream_is_ready(state);
        if (state == PW_STREAM_STATE_ERROR && &output == &engine.output_channels_[0]) {
            engine.startup_error_ = error == nullptr ? "PipeWire speaker stream failed" : error;
        }
    }
    engine.state_changed_.notify_all();
}

void PipeWireEngine::on_stream_input_state_changed(void* data, pw_stream_state,
                                                   pw_stream_state state, const char* error)
{
    auto& stream = *static_cast<PlaybackStream*>(data);
    stream.engine->update_playback_stream_state(stream, state, error);
}

void PipeWireEngine::on_output_process(void* data)
{
    auto& output = *static_cast<OutputChannel*>(data);
    output.engine->output_process(output);
}

void PipeWireEngine::on_stream_input_process(void* data)
{
    auto& stream = *static_cast<PlaybackStream*>(data);
    stream.engine->stream_input_process(stream);
}

void PipeWireEngine::on_capture_source_state_changed(void* data, pw_stream_state,
                                                       pw_stream_state state, const char*)
{
    auto& engine = *static_cast<PipeWireEngine*>(data);
    engine.capture_channel_.active.store(stream_is_ready(state), std::memory_order_release);
    if (state == PW_STREAM_STATE_ERROR) {
        engine.capture_channel_.output_mask.store(0, std::memory_order_release);
    }
}

void PipeWireEngine::on_capture_source_process(void* data)
{
    static_cast<PipeWireEngine*>(data)->capture_source_process();
}

int PipeWireEngine::on_capture_source_request(spa_loop*, bool, std::uint32_t,
                                               const void*, std::size_t, void* user_data)
{
    auto& request = *static_cast<CaptureSourceRequest*>(user_data);
    request.succeeded = request.engine->replace_capture_source(request.source_node_id, request.error);
    return 0;
}

bool PipeWireEngine::replace_capture_source(std::uint32_t source_node_id, std::string& error)
{
    NodeInfo source;
    {
        std::lock_guard lock(nodes_mutex_);
        const auto found = nodes_.find(source_node_id);
        if (found == nodes_.end() || found->second.media_class != "Audio/Source") {
            error = "unknown PipeWire capture source: " + std::to_string(source_node_id);
            return false;
        }
        source = found->second;
    }
    if (capture_channel_.source_node_id == source_node_id && capture_channel_.source_stream != nullptr) {
        return true;
    }
    destroy_capture_stream();
    capture_channel_.output_mask.store(0, std::memory_order_release);
    for (auto& ring : capture_channel_.audio) ring.discard_all();
    capture_channel_.smoothed_gain = capture_channel_.target_gain.load(std::memory_order_relaxed);

    pw_properties* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
        PW_KEY_NODE_NAME, "shatter.capture.input", PW_KEY_APP_NAME, "Shatter",
        PW_KEY_TARGET_OBJECT, source.name.c_str(), nullptr);
    if (properties == nullptr) {
        error = "could not allocate capture stream properties";
        return false;
    }
    capture_channel_.events = {};
    capture_channel_.events.version = PW_VERSION_STREAM_EVENTS;
    capture_channel_.events.state_changed = &PipeWireEngine::on_capture_source_state_changed;
    capture_channel_.events.process = &PipeWireEngine::on_capture_source_process;
    capture_channel_.source_stream = pw_stream_new(core_, "Shatter microphone input", properties);
    if (capture_channel_.source_stream == nullptr) {
        error = "could not create PipeWire capture stream";
        return false;
    }
    pw_stream_add_listener(capture_channel_.source_stream, &capture_channel_.listener,
                           &capture_channel_.events, this);
    std::array<std::byte, 1024> storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    const spa_pod* params[] = {build_stereo_format(&builder)};
    {
        std::lock_guard lock(nodes_mutex_);
        capture_channel_.source_node_id = source_node_id;
    }
    if (pw_stream_connect(capture_channel_.source_stream, PW_DIRECTION_INPUT, source_node_id,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                         PW_STREAM_FLAG_RT_PROCESS), params, 1) < 0) {
        error = "could not connect to PipeWire capture source";
        destroy_capture_stream();
        return false;
    }
    return true;
}

void PipeWireEngine::destroy_capture_stream()
{
    capture_channel_.active.store(false, std::memory_order_release);
    capture_channel_.mixer_users.fetch_or(kMixerClosed, std::memory_order_acq_rel);
    if (capture_channel_.source_stream != nullptr) {
        pw_stream_disconnect(capture_channel_.source_stream);
        pw_stream_destroy(capture_channel_.source_stream);
        capture_channel_.source_stream = nullptr;
    }
    while ((capture_channel_.mixer_users.load(std::memory_order_acquire) & kMixerUserMask) != 0) {
        std::this_thread::yield();
    }
    for (auto& ring : capture_channel_.audio) ring.discard_all();
    capture_channel_.mixer_users.store(0, std::memory_order_release);
    {
        std::lock_guard lock(nodes_mutex_);
        capture_channel_.source_node_id = PW_ID_ANY;
    }
}

void PipeWireEngine::on_global(void* data, std::uint32_t id, std::uint32_t,
                               const char* type, std::uint32_t version, const spa_dict* props)
{
    if (type == nullptr || props == nullptr) {
        return;
    }
    auto& engine = *static_cast<PipeWireEngine*>(data);

    if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name != nullptr && std::strcmp(name, "default") == 0 && engine.default_metadata_ == nullptr) {
            engine.default_metadata_ = static_cast<pw_metadata*>(pw_registry_bind(
                engine.registry_, id, PW_TYPE_INTERFACE_Metadata,
                std::min<std::uint32_t>(version, PW_VERSION_METADATA), 0));
            if (engine.default_metadata_ != nullptr) {
                std::lock_guard lock(engine.nodes_mutex_);
                for (std::size_t index = 0; index < engine.allocated_streams_.load(std::memory_order_acquire); ++index) {
                    auto& stream = engine.playback_streams_[index];
                    if (stream.active.load(std::memory_order_relaxed)) {
                        engine.assign_stream_target(stream);
                    }
                }
            }
        }
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        const char* output_text = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
        const char* input_text = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
        if (output_text == nullptr || input_text == nullptr) {
            return;
        }
        LinkInfo link;
        const auto output_parse = std::from_chars(output_text, output_text + std::strlen(output_text), link.output_node);
        const auto input_parse = std::from_chars(input_text, input_text + std::strlen(input_text), link.input_node);
        if (output_parse.ec != std::errc{} || input_parse.ec != std::errc{}) {
            return;
        }
        {
            std::lock_guard lock(engine.nodes_mutex_);
            engine.links_[id] = link;
            engine.refresh_output_link_locked();
        }
        engine.state_changed_.notify_all();
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
        return;
    }
    const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (media_class == nullptr) {
        return;
    }
    const std::string_view media_class_view(media_class);
    if (media_class_view != "Audio/Sink" && media_class_view != "Audio/Source" &&
        media_class_view != "Stream/Output/Audio") {
        return;
    }

    NodeInfo node;
    node.node_id = id;
    node.media_class = media_class;
    if (const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) node.name = name;
    if (const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION)) node.description = description;
    if (const char* application = spa_dict_lookup(props, PW_KEY_APP_NAME)) node.application = application;
    if (const char* app_id = spa_dict_lookup(props, PW_KEY_APP_ID)) node.application_id = app_id;
    if (const char* binary = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY)) node.process_binary = binary;

    if (media_class_view == "Stream/Output/Audio") {
        if (node.name == kSpeakerStreamName || node.application == "Shatter") {
            return;
        }
        engine.handle_playback_node(id, std::move(node));
        return;
    }

    std::lock_guard lock(engine.nodes_mutex_);
    engine.nodes_[id] = std::move(node);
    engine.refresh_output_link_locked();
}

void PipeWireEngine::on_global_remove(void* data, std::uint32_t id)
{
    auto& engine = *static_cast<PipeWireEngine*>(data);
    PlaybackStream* removed = nullptr;
    bool removed_capture_source = false;
    {
        std::lock_guard lock(engine.nodes_mutex_);
        removed_capture_source = engine.capture_channel_.source_node_id == id;
        const auto stream = engine.stream_by_node_.find(id);
        if (stream != engine.stream_by_node_.end()) {
            removed = &engine.playback_streams_[stream->second];
            removed->active.store(false, std::memory_order_release);
            removed->mixer_users.fetch_or(kMixerClosed, std::memory_order_acq_rel);
            engine.stream_by_node_.erase(stream);
        }
        engine.nodes_.erase(id);
        engine.links_.erase(id);
        engine.refresh_output_link_locked();
    }
    if (removed_capture_source) {
        engine.destroy_capture_stream();
        engine.capture_channel_.output_mask.store(0, std::memory_order_release);
    }
    if (removed != nullptr) {
        if (removed->sink != nullptr) {
            pw_stream_disconnect(removed->sink);
            pw_stream_destroy(removed->sink);
            removed->sink = nullptr;
        }
        while ((removed->mixer_users.load(std::memory_order_acquire) & kMixerUserMask) != 0) {
            std::this_thread::yield();
        }
        for (auto& ring : removed->audio) ring.discard_all();
        std::lock_guard lock(engine.nodes_mutex_);
        removed->name.clear();
        removed->application.clear();
        removed->sink_name.clear();
        removed->node_id = PW_ID_ANY;
        removed->in_use = false;
        removed->mixer_users.store(0, std::memory_order_release);
    }
    engine.state_changed_.notify_all();
}

void PipeWireEngine::stream_input_process(PlaybackStream& stream)
{
    RuntimeProfiler::Scope profile_scope(profiler_, RuntimeProfiler::Callback::input);
    pw_buffer* buffer = pw_stream_dequeue_buffer(stream.sink);
    if (buffer == nullptr) {
        return;
    }
    if (buffer->buffer != nullptr && buffer->buffer->n_datas > 0) {
        spa_data& data = buffer->buffer->datas[0];
        if (data.data != nullptr && data.chunk != nullptr) {
            const auto stride = data.chunk->stride > 0 ? static_cast<std::size_t>(data.chunk->stride)
                                                       : sizeof(float) * kChannels;
            const auto frames = static_cast<std::size_t>(data.chunk->size) / stride;
            const auto* samples = reinterpret_cast<const float*>(
                static_cast<const std::uint8_t*>(data.data) + data.chunk->offset);
            const float target = stream.muted.load(std::memory_order_relaxed)
                ? 0.0F : stream.target_gain.load(std::memory_order_relaxed);
            const float increment = frames == 0 ? 0.0F
                : (target - stream.smoothed_gain) / static_cast<float>(frames);
            std::size_t written = frames;
            const auto mask = app_channels_[stream.channel_index].output_mask.load(std::memory_order_acquire);
            for (std::size_t output_index = 0; output_index < output_channels_.size(); ++output_index) {
                if ((mask & (1U << output_index)) == 0) continue;
                OutputChannel& output = output_channels_[output_index];
                std::uint32_t users = output.input_users.load(std::memory_order_acquire);
                bool output_acquired = false;
                while ((users & kMixerClosed) == 0) {
                    if (output.input_users.compare_exchange_weak(users, users + 1,
                            std::memory_order_acquire, std::memory_order_relaxed)) {
                        output_acquired = true;
                        break;
                    }
                }
                if (!output_acquired) continue;
                if (output.linked.load(std::memory_order_acquire)) {
                    written = std::min(written, stream.audio[output_index].write_with_gain_ramp(
                        samples, frames, stream.smoothed_gain, increment));
                }
                output.input_users.fetch_sub(1, std::memory_order_release);
            }
            profile_scope.set_frames(frames, kSampleRate);
            if (mask != 0) profile_scope.record_input_overrun(frames - written);
            stream.smoothed_gain = target;
        }
    }
    pw_stream_queue_buffer(stream.sink, buffer);
}

void PipeWireEngine::capture_source_process()
{
    RuntimeProfiler::Scope profile_scope(profiler_, RuntimeProfiler::Callback::input);
    pw_buffer* buffer = pw_stream_dequeue_buffer(capture_channel_.source_stream);
    if (buffer == nullptr) return;
    if (buffer->buffer != nullptr && buffer->buffer->n_datas > 0) {
        spa_data& data = buffer->buffer->datas[0];
        if (data.data != nullptr && data.chunk != nullptr) {
            const auto stride = data.chunk->stride > 0 ? static_cast<std::size_t>(data.chunk->stride)
                                                       : sizeof(float) * kChannels;
            const auto frames = static_cast<std::size_t>(data.chunk->size) / stride;
            const auto* samples = reinterpret_cast<const float*>(
                static_cast<const std::uint8_t*>(data.data) + data.chunk->offset);
            const float target = capture_channel_.muted.load(std::memory_order_relaxed)
                ? 0.0F : capture_channel_.target_gain.load(std::memory_order_relaxed);
            const float increment = frames == 0 ? 0.0F
                : (target - capture_channel_.smoothed_gain) / static_cast<float>(frames);
            const auto mask = capture_channel_.output_mask.load(std::memory_order_acquire);
            for (std::size_t output_index = 0; output_index < output_channels_.size(); ++output_index) {
                if ((mask & (1U << output_index)) == 0) continue;
                OutputChannel& output = output_channels_[output_index];
                std::uint32_t users = output.input_users.load(std::memory_order_acquire);
                bool output_acquired = false;
                while ((users & kMixerClosed) == 0) {
                    if (output.input_users.compare_exchange_weak(users, users + 1,
                            std::memory_order_acquire, std::memory_order_relaxed)) {
                        output_acquired = true;
                        break;
                    }
                }
                if (!output_acquired) continue;
                if (output.linked.load(std::memory_order_acquire))
                    capture_channel_.audio[output_index].write_with_gain_ramp(samples, frames,
                        capture_channel_.smoothed_gain, increment);
                output.input_users.fetch_sub(1, std::memory_order_release);
            }
            capture_channel_.smoothed_gain = target;
            profile_scope.set_frames(frames, kSampleRate);
        }
    }
    pw_stream_queue_buffer(capture_channel_.source_stream, buffer);
}

void PipeWireEngine::output_process(OutputChannel& output)
{
    RuntimeProfiler::Scope profile_scope(profiler_, RuntimeProfiler::Callback::output);
    pw_buffer* buffer = pw_stream_dequeue_buffer(output.stream);
    if (buffer == nullptr) return;

    std::uint32_t users = output.mixer_users.load(std::memory_order_acquire);
    bool acquired = false;
    while ((users & kMixerClosed) == 0) {
        if (output.mixer_users.compare_exchange_weak(users, users + 1,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            acquired = true;
            break;
        }
    }
    if (!acquired) {
        pw_stream_queue_buffer(output.stream, buffer);
        return;
    }

    if (buffer->buffer != nullptr && buffer->buffer->n_datas > 0) {
        spa_data& data = buffer->buffer->datas[0];
        if (data.data != nullptr && data.chunk != nullptr) {
            const std::size_t output_index = static_cast<std::size_t>(&output - output_channels_.data());
            const std::uint32_t output_bit = 1U << output_index;
            const auto capacity_frames = static_cast<std::size_t>(data.maxsize) / (sizeof(float) * kChannels);
            const auto requested_frames = buffer->requested == 0
                ? capacity_frames : static_cast<std::size_t>(buffer->requested);
            const auto frames = std::min(capacity_frames, requested_frames);
            profile_scope.set_frames(frames, kSampleRate);
            if (profiler_.active()) {
                pw_time timing{};
                if (pw_stream_get_time_n(output.stream, &timing, sizeof(timing)) == 0 && timing.rate.denom != 0) {
                    const auto delay_ns = timing.delay > 0
                        ? static_cast<std::uint64_t>(timing.delay) * timing.rate.num * 1'000'000'000ULL /
                            timing.rate.denom : 0;
                    profile_scope.record_pipewire_time(delay_ns, timing.queued / (sizeof(float) * kChannels));
                }
            }

            auto* samples = reinterpret_cast<float*>(data.data);
            std::fill(samples, samples + frames * kChannels, 0.0F);
            std::array<float, 1024 * kChannels> channel_mix{};
            const auto channel_count = allocated_channels_.load(std::memory_order_acquire);
            const auto stream_count = allocated_streams_.load(std::memory_order_acquire);
            for (std::size_t offset = 0; offset < frames; offset += 1024) {
                const auto block_frames = std::min<std::size_t>(1024, frames - offset);
                const auto output_offset = offset * kChannels;
                for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
                    AppChannel& channel = app_channels_[channel_index];
                    const bool assigned = (channel.output_mask.load(std::memory_order_acquire) & output_bit) != 0;
                    std::fill(channel_mix.begin(), channel_mix.begin() + block_frames * kChannels, 0.0F);
                    for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
                        PlaybackStream& stream = playback_streams_[stream_index];
                        if (!stream.active.load(std::memory_order_acquire) || stream.channel_index != channel_index) continue;
                        std::uint32_t stream_users = stream.mixer_users.load(std::memory_order_acquire);
                        bool stream_acquired = false;
                        while ((stream_users & kMixerClosed) == 0) {
                            if (stream.mixer_users.compare_exchange_weak(stream_users, stream_users + 1,
                                    std::memory_order_acquire, std::memory_order_relaxed)) {
                                stream_acquired = true;
                                break;
                            }
                        }
                        if (!stream_acquired || !stream.active.load(std::memory_order_acquire)) {
                            if (stream_acquired) stream.mixer_users.fetch_sub(1, std::memory_order_release);
                            continue;
                        }
                        auto& ring = stream.audio[output_index];
                        profile_scope.record_ring_occupancy(ring.available_frames());
                        const auto consumed = ring.accumulate(channel_mix.data(), block_frames, 1.0F);
                        profile_scope.record_unfilled_mix_frames(block_frames - consumed);
                        stream.mixer_users.fetch_sub(1, std::memory_order_release);
                    }
                    if (!assigned) continue;
                    const float target = channel.muted.load(std::memory_order_relaxed)
                        ? 0.0F : channel.target_gain.load(std::memory_order_relaxed);
                    float gain = channel.smoothed_gain[output_index];
                    const float increment = block_frames == 0 ? 0.0F
                        : (target - gain) / static_cast<float>(block_frames);
                    for (std::size_t frame = 0; frame < block_frames; ++frame) {
                        samples[output_offset + frame * kChannels] += channel_mix[frame * kChannels] * gain;
                        samples[output_offset + frame * kChannels + 1] += channel_mix[frame * kChannels + 1] * gain;
                        gain += increment;
                    }
                    channel.smoothed_gain[output_index] = target;
                }

                std::fill(channel_mix.begin(), channel_mix.begin() + block_frames * kChannels, 0.0F);
                std::uint32_t capture_users = capture_channel_.mixer_users.load(std::memory_order_acquire);
                bool capture_acquired = false;
                while ((capture_users & kMixerClosed) == 0) {
                    if (capture_channel_.mixer_users.compare_exchange_weak(capture_users, capture_users + 1,
                            std::memory_order_acquire, std::memory_order_relaxed)) {
                        capture_acquired = true;
                        break;
                    }
                }
                if (capture_acquired) {
                    auto& ring = capture_channel_.audio[output_index];
                    const auto consumed = ring.read(channel_mix.data(), block_frames);
                    profile_scope.record_ring_occupancy(ring.available_frames());
                    profile_scope.record_unfilled_mix_frames(block_frames - consumed);
                    if ((capture_channel_.output_mask.load(std::memory_order_acquire) & output_bit) != 0) {
                        for (std::size_t sample = 0; sample < block_frames * kChannels; ++sample)
                            samples[output_offset + sample] += channel_mix[sample];
                    }
                    capture_channel_.mixer_users.fetch_sub(1, std::memory_order_release);
                }
            }
            data.chunk->offset = 0;
            data.chunk->stride = sizeof(float) * kChannels;
            data.chunk->size = frames * sizeof(float) * kChannels;
        }
    }
    output.mixer_users.fetch_sub(1, std::memory_order_release);
    pw_stream_queue_buffer(output.stream, buffer);
}

void PipeWireEngine::update_playback_stream_state(PlaybackStream& stream, pw_stream_state state,
                                                  const char* error)
{
    if (stream_is_ready(state)) {
        stream.active.store(true, std::memory_order_release);
        if (default_metadata_ != nullptr) {
            assign_stream_target(stream);
        }
    } else if (state == PW_STREAM_STATE_ERROR && error != nullptr) {
        std::lock_guard lock(state_mutex_);
        if (startup_error_.empty()) {
            startup_error_ = error;
        }
    }
}

void PipeWireEngine::refresh_output_link_locked()
{
    for (auto& output : output_channels_) {
        output.linked.store(false, std::memory_order_relaxed);
        output.sink_node_id.store(PW_ID_ANY, std::memory_order_relaxed);
    }
    for (const auto& [link_id, link] : links_) {
        (void)link_id;
        const auto target = nodes_.find(link.input_node);
        if (target == nodes_.end() || target->second.media_class != "Audio/Sink" ||
            target->second.name.starts_with("shatter.stream.")) continue;
        for (auto& output : output_channels_) {
            if (output.in_use && output.node_id == link.output_node) {
                output.sink_node_id.store(link.input_node, std::memory_order_relaxed);
                output.linked.store(true, std::memory_order_release);
                break;
            }
        }
    }
    output_linked_.store(output_channels_[0].linked.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void PipeWireEngine::handle_playback_node(std::uint32_t node_id, NodeInfo node)
{
    PlaybackStream* stream = nullptr;
    {
        std::lock_guard lock(nodes_mutex_);
        if (stream_by_node_.contains(node_id)) {
            return;
        }
        if (node.application_id.empty() && node.process_binary.empty() && node.application.empty()) {
            node.identity = "node:" + std::to_string(node_id);
        } else if (!node.application_id.empty()) {
            node.identity = "app-id:" + node.application_id;
        } else if (!node.process_binary.empty()) {
            node.identity = "binary:" + node.process_binary;
        } else {
            node.identity = "app-name:" + node.application;
        }
        node.channel_index = get_or_create_channel_locked(node);
        if (node.channel_index >= max_app_channels) {
            unrouted_playback_streams_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto stream_count = allocated_streams_.load(std::memory_order_relaxed);
        std::size_t stream_index = playback_streams_.size();
        for (std::size_t index = 0; index < stream_count; ++index) {
            if (!playback_streams_[index].in_use) {
                stream_index = index;
                break;
            }
        }
        if (stream_index == playback_streams_.size()) {
            if (stream_count >= playback_streams_.size()) {
                unrouted_playback_streams_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            stream_index = stream_count++;
            allocated_streams_.store(stream_count, std::memory_order_release);
        }
        stream = &playback_streams_[stream_index];
        stream->engine = this;
        stream->in_use = true;
        stream->mixer_users.store(0, std::memory_order_relaxed);
        stream->target_gain.store(1.0F, std::memory_order_relaxed);
        stream->muted.store(false, std::memory_order_relaxed);
        stream->smoothed_gain = 1.0F;
        stream->node_id = node_id;
        stream->channel_index = node.channel_index;
        stream->name = node.name.empty() ? node.description : node.name;
        stream->original_name = stream->name;
        stream->application = node.application;
        for (const auto& saved : saved_streams_) {
            if (saved.app_identity == node.identity && saved.original_name == stream->original_name) {
                stream->name = saved.name;
                stream->target_gain.store(std::pow(10.0F, saved.gain_db / 20.0F));
                stream->muted.store(saved.muted);
                break;
            }
        }
        stream->sink_name = "shatter.stream." + std::to_string(node_id);
        stream->events.version = PW_VERSION_STREAM_EVENTS;
        stream->events.state_changed = &PipeWireEngine::on_stream_input_state_changed;
        stream->events.process = &PipeWireEngine::on_stream_input_process;
        stream_by_node_[node_id] = stream_index;
        nodes_[node_id] = node;
    }
    create_stream_sink(*stream);
}

void PipeWireEngine::create_stream_sink(PlaybackStream& stream)
{
    const std::string description = "Shatter input: " + stream.application + " / " + stream.name;
    stream.sink = pw_stream_new(core_, description.c_str(),
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                         PW_KEY_MEDIA_CATEGORY, "Playback",
                         PW_KEY_MEDIA_ROLE, "Production",
                         PW_KEY_MEDIA_CLASS, "Audio/Sink",
                         PW_KEY_NODE_NAME, stream.sink_name.c_str(),
                         PW_KEY_NODE_DESCRIPTION, description.c_str(),
                         PW_KEY_NODE_VIRTUAL, "true",
                         PW_KEY_APP_NAME, "Shatter",
                         PW_KEY_AUDIO_CHANNELS, "2",
                         nullptr));
    if (stream.sink == nullptr) {
        return;
    }
    pw_stream_add_listener(stream.sink, &stream.listener, &stream.events, &stream);
    std::array<std::uint8_t, 1024> format_storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(format_storage.data(), format_storage.size());
    const spa_pod* params[] = {build_stereo_format(&builder)};
    const auto result = pw_stream_connect(
        stream.sink, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (result < 0) {
        pw_stream_destroy(stream.sink);
        stream.sink = nullptr;
    }
}

void PipeWireEngine::assign_stream_target(PlaybackStream& stream)
{
    if (default_metadata_ == nullptr || !stream.active.load(std::memory_order_relaxed)) {
        return;
    }
    const int result = pw_metadata_set_property(default_metadata_, stream.node_id,
                                                kTargetObjectKey, "Spa:String", stream.sink_name.c_str());
    if (result < 0) {
        std::lock_guard lock(state_mutex_);
        startup_error_ = "could not ask WirePlumber to route playback stream " + std::to_string(stream.node_id);
    }
}

std::size_t PipeWireEngine::get_or_create_channel_locked(const NodeInfo& node)
{
    const auto existing = channel_by_identity_.find(node.identity);
    if (existing != channel_by_identity_.end()) {
        return existing->second;
    }
    const auto current_channel_count = allocated_channels_.load(std::memory_order_relaxed);
    if (current_channel_count >= app_channels_.size()) {
        return max_app_channels;
    }
    const std::size_t index = current_channel_count;
    AppChannel& channel = app_channels_[index];
    channel.allocated = true;
    channel.identity = node.identity;
    channel.id = make_channel_id(node.identity);
    channel.name = !node.application.empty() ? node.application
        : !node.process_binary.empty() ? node.process_binary : node.identity;
    channel.smoothed_gain.fill(1.0F);
    for (const auto& saved : saved_apps_) if (saved.identity == channel.identity) {
        channel.name = saved.name;
        channel.target_gain.store(std::pow(10.0F, saved.gain_db / 20.0F));
        channel.muted.store(saved.muted);
        break;
    }
    channel_by_identity_[channel.identity] = index;
    allocated_channels_.store(current_channel_count + 1, std::memory_order_release);
    return index;
}

PipeWireEngine::PlaybackStream* PipeWireEngine::find_stream_locked(std::uint32_t node_id)
{
    const auto found = stream_by_node_.find(node_id);
    return found == stream_by_node_.end() ? nullptr : &playback_streams_[found->second];
}

PipeWireEngine::AppChannel* PipeWireEngine::find_channel_locked(std::string_view channel_id)
{
    for (std::size_t index = 0; index < allocated_channels_.load(std::memory_order_relaxed); ++index) {
        if (app_channels_[index].id == channel_id) {
            return &app_channels_[index];
        }
    }
    return nullptr;
}

PipeWireEngine::AppChannel* PipeWireEngine::resolve_channel_locked(std::string_view selector,
                                                                   std::string& error)
{
    if (AppChannel* by_id = find_channel_locked(selector); by_id != nullptr) return by_id;
    AppChannel* match = nullptr;
    for (std::size_t index = 0; index < allocated_channels_.load(std::memory_order_relaxed); ++index) {
        AppChannel& channel = app_channels_[index];
        if (channel.name != selector) continue;
        if (match != nullptr) {
            error = "app name is ambiguous; use its channel ID from --list-in";
            return nullptr;
        }
        match = &channel;
    }
    if (match == nullptr) error = "unknown app channel or name: " + std::string(selector);
    return match;
}

PipeWireEngine::PlaybackStream* PipeWireEngine::resolve_stream_locked(std::string_view selector,
                                                                      std::string& error)
{
    std::uint32_t node_id = 0;
    const auto [end, parse_error] = std::from_chars(selector.data(), selector.data() + selector.size(), node_id);
    if (parse_error == std::errc{} && end == selector.data() + selector.size()) {
        if (PlaybackStream* by_id = find_stream_locked(node_id); by_id != nullptr) return by_id;
    }
    PlaybackStream* match = nullptr;
    for (std::size_t index = 0; index < allocated_streams_.load(std::memory_order_relaxed); ++index) {
        PlaybackStream& stream = playback_streams_[index];
        if (!stream.in_use || stream.name != selector) continue;
        if (match != nullptr) {
            error = "stream name is ambiguous; use its PipeWire node ID from --list-in";
            return nullptr;
        }
        match = &stream;
    }
    if (match == nullptr) error = "unknown playback stream or name: " + std::string(selector);
    return match;
}

void PipeWireEngine::run_loop()
{
    pw_main_loop_run(main_loop_);
}

void PipeWireEngine::cleanup()
{
    available_ = false;
    if (main_loop_ != nullptr) {
        pw_main_loop_quit(main_loop_);
    }
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
    destroy_capture_stream();
    for (auto& stream : playback_streams_) {
        if (stream.sink != nullptr) {
            pw_stream_disconnect(stream.sink);
            pw_stream_destroy(stream.sink);
            stream.sink = nullptr;
        }
        stream.active.store(false, std::memory_order_relaxed);
    }
    for (std::size_t index = 0; index < output_channels_.size(); ++index) {
        auto& output = output_channels_[index];
        if (output.stream != nullptr) destroy_output_stream(output);
        output.node_id = PW_ID_ANY;
        output.sink_node_id.store(PW_ID_ANY, std::memory_order_release);
        output.linked.store(false, std::memory_order_release);
        output.in_use = index == 0;
    }
    if (default_metadata_ != nullptr) {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(default_metadata_));
        default_metadata_ = nullptr;
    }
    if (registry_ != nullptr) {
        if (core_ != nullptr) {
            pw_core_destroy(core_, registry_);
        }
        registry_ = nullptr;
    }
    if (core_ != nullptr) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    if (context_ != nullptr) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    if (main_loop_ != nullptr) {
        pw_main_loop_destroy(main_loop_);
        main_loop_ = nullptr;
    }
}

} // namespace shatter::engine
