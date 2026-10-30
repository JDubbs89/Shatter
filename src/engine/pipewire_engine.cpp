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

    output_events_.version = PW_VERSION_STREAM_EVENTS;
    output_events_.state_changed = &PipeWireEngine::on_output_state_changed;
    output_events_.process = &PipeWireEngine::on_output_process;
    speaker_output_ = pw_stream_new(core_, "Shatter Speakers",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                         PW_KEY_MEDIA_CATEGORY, "Playback",
                         PW_KEY_MEDIA_ROLE, "Production",
                         PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
                         PW_KEY_NODE_NAME, kSpeakerStreamName,
                         PW_KEY_NODE_DESCRIPTION, "Shatter Speakers",
                         nullptr));
    if (speaker_output_ == nullptr) {
        error = "could not create the speaker output stream";
        cleanup();
        return false;
    }
    pw_stream_add_listener(speaker_output_, &output_listener_, &output_events_, this);

    std::array<std::uint8_t, 1024> format_storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(format_storage.data(), format_storage.size());
    const spa_pod* params[] = {build_stereo_format(&builder)};
    const auto result = pw_stream_connect(
        speaker_output_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (result < 0) {
        error = "could not connect the speaker output to PipeWire";
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
    capture_channel_.speaker_send_enabled.store(enabled, std::memory_order_release);
    return true;
}

std::string PipeWireEngine::list_inputs() const
{
    std::lock_guard lock(nodes_mutex_);
    std::ostringstream table;
    table << "Shatter inputs\n"
          << std::left << std::setw(25) << "Channel ID" << std::setw(24) << "Application"
          << "Playback stream (PipeWire node ID)\n"
          << std::string(96, '-') << '\n';

    const auto channel_count = allocated_channels_.load(std::memory_order_relaxed);
    const auto stream_count = allocated_streams_.load(std::memory_order_relaxed);
    for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
        const AppChannel& channel = app_channels_[channel_index];
        table << std::setw(25) << channel.id << std::setw(24) << channel.name;
        bool has_stream = false;
        for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
            const PlaybackStream& stream = playback_streams_[stream_index];
            if (!stream.active.load(std::memory_order_relaxed) || stream.channel_index != channel_index) {
                continue;
            }
            if (has_stream) {
                table << std::setw(25) << "" << std::setw(24) << "";
            }
            table << stream.name << " (" << stream.node_id << ")\n";
            has_stream = true;
        }
        if (!has_stream) {
            table << "no active playback streams\n";
        }
    }
    if (channel_count == 0) {
        table << "No application playback streams have been discovered\n";
    }

    table << std::setw(25) << "capture" << std::setw(24) << "Microphone";
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
              << (capture_channel_.speaker_send_enabled.load(std::memory_order_relaxed) ? "on" : "off")
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
        table << std::setw(25) << "available" << std::setw(24) << "Capture source"
              << (node.name.empty() ? node.description : node.name) << " (" << node.node_id << ")"
              << (capture_channel_.source_node_id == node.node_id ? " (selected)\n" : " (available)\n");
    }
    if (capture_sources.empty()) {
        table << std::setw(25) << "available" << std::setw(24) << "Capture source"
              << "No PipeWire capture sources found\n";
    }
    const auto unrouted_count = unrouted_playback_streams_.load(std::memory_order_relaxed);
    if (unrouted_count > 0) {
        table << "Warning: " << unrouted_count
              << " playback streams could not be routed because Shatter reached its channel or stream limit\n";
    }
    return table.str();
}

std::string PipeWireEngine::list_outputs() const
{
    std::lock_guard lock(nodes_mutex_);
    std::ostringstream table;
    std::string speaker_endpoint = "no PipeWire sink linked";
    const auto speaker_id = speaker_node_id_.load(std::memory_order_relaxed);
    for (const auto& [link_id, link] : links_) {
        (void)link_id;
        if (link.output_node != speaker_id) {
            continue;
        }
        const auto target = nodes_.find(link.input_node);
        if (target != nodes_.end() && target->second.media_class == "Audio/Sink") {
            speaker_endpoint = target->second.name.empty() ? target->second.description : target->second.name;
            break;
        }
    }

    table << "Shatter outputs\n"
          << std::left << std::setw(24) << "Destination" << std::setw(22) << "Type" << "PipeWire endpoint\n"
          << std::string(78, '-') << '\n'
          << std::setw(24) << "speakers" << std::setw(22) << "Playback"
          << kSpeakerStreamName << " -> " << speaker_endpoint << '\n';

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
    for (const auto& node : playback_sinks) {
        table << std::setw(24) << "available" << std::setw(22) << "Playback sink"
              << (node.name.empty() ? node.description : node.name)
              << (node.name == speaker_endpoint ? " (current speaker route)" : " (available)") << '\n';
    }
    if (playback_sinks.empty()) {
        table << std::setw(24) << "available" << std::setw(22) << "Playback sink"
              << "No PipeWire playback sinks found\n";
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
    auto& engine = *static_cast<PipeWireEngine*>(data);
    {
        std::lock_guard lock(engine.state_mutex_);
        engine.output_ready_ = stream_is_ready(state);
        if (state == PW_STREAM_STATE_ERROR) {
            engine.startup_error_ = error == nullptr ? "PipeWire speaker stream failed" : error;
        }
    }
    if (stream_is_ready(state) && engine.speaker_output_ != nullptr) {
        engine.speaker_node_id_.store(pw_stream_get_node_id(engine.speaker_output_), std::memory_order_relaxed);
        std::lock_guard lock(engine.nodes_mutex_);
        engine.refresh_output_link_locked();
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
    static_cast<PipeWireEngine*>(data)->output_process();
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
        engine.capture_channel_.speaker_send_enabled.store(false, std::memory_order_release);
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
    capture_channel_.speaker_send_enabled.store(false, std::memory_order_release);
    capture_channel_.audio.discard_all();
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
    capture_channel_.audio.discard_all();
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
        engine.capture_channel_.speaker_send_enabled.store(false, std::memory_order_release);
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
        removed->audio.discard_all();
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
            const auto written = stream.audio.write_with_gain_ramp(samples, frames, stream.smoothed_gain, increment);
            profile_scope.set_frames(frames, kSampleRate);
            profile_scope.record_input_overrun(frames - written);
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
            capture_channel_.audio.write_with_gain_ramp(samples, frames,
                capture_channel_.smoothed_gain, increment);
            capture_channel_.smoothed_gain = target;
            profile_scope.set_frames(frames, kSampleRate);
        }
    }
    pw_stream_queue_buffer(capture_channel_.source_stream, buffer);
}

void PipeWireEngine::output_process()
{
    RuntimeProfiler::Scope profile_scope(profiler_, RuntimeProfiler::Callback::output);
    pw_buffer* buffer = pw_stream_dequeue_buffer(speaker_output_);
    if (buffer == nullptr) {
        return;
    }
    if (buffer->buffer != nullptr && buffer->buffer->n_datas > 0) {
        spa_data& data = buffer->buffer->datas[0];
        if (data.data != nullptr && data.chunk != nullptr) {
            const auto capacity_frames = static_cast<std::size_t>(data.maxsize) / (sizeof(float) * kChannels);
            // requested is a frame count; maxsize is the backing buffer's byte capacity.
            const auto requested_frames = buffer->requested == 0
                ? capacity_frames
                : static_cast<std::size_t>(buffer->requested);
            const auto frames = std::min(capacity_frames, requested_frames);
            profile_scope.set_frames(frames, kSampleRate);
            if (profiler_.active()) {
                pw_time timing{};
                if (pw_stream_get_time_n(speaker_output_, &timing, sizeof(timing)) == 0 && timing.rate.denom != 0) {
                    const auto delay_ns = timing.delay > 0
                        ? static_cast<std::uint64_t>(timing.delay) * timing.rate.num * 1'000'000'000ULL /
                            timing.rate.denom
                        : 0;
                    const auto queued_frames = timing.queued / (sizeof(float) * kChannels);
                    profile_scope.record_pipewire_time(delay_ns, queued_frames);
                }
            }
            auto* samples = reinterpret_cast<float*>(data.data);
            std::fill(samples, samples + frames * kChannels, 0.0F);

            std::array<float, max_app_channels> channel_start{};
            std::array<float, max_app_channels> channel_increment{};
            std::array<float, 1024 * kChannels> channel_mix{};
            const auto channel_count = allocated_channels_.load(std::memory_order_acquire);
            const auto stream_count = allocated_streams_.load(std::memory_order_acquire);
            for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
                AppChannel& channel = app_channels_[channel_index];
                const float target = channel.muted.load(std::memory_order_relaxed)
                    ? 0.0F : channel.target_gain.load(std::memory_order_relaxed);
                channel_start[channel_index] = channel.smoothed_gain;
                channel_increment[channel_index] = frames == 0 ? 0.0F
                    : (target - channel.smoothed_gain) / static_cast<float>(frames);
            }

            for (std::size_t offset = 0; offset < frames; offset += 1024) {
                const auto block_frames = std::min<std::size_t>(1024, frames - offset);
                for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
                    std::fill(channel_mix.begin(), channel_mix.begin() + block_frames * kChannels, 0.0F);
                    for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
                        PlaybackStream& stream = playback_streams_[stream_index];
                        if (!stream.active.load(std::memory_order_acquire) ||
                            stream.channel_index != channel_index) {
                            continue;
                        }
                        std::uint32_t users = stream.mixer_users.load(std::memory_order_acquire);
                        bool acquired = false;
                        while ((users & kMixerClosed) == 0) {
                            if (stream.mixer_users.compare_exchange_weak(users, users + 1,
                                    std::memory_order_acquire, std::memory_order_relaxed)) {
                                acquired = true;
                                break;
                            }
                        }
                        if (!acquired || !stream.active.load(std::memory_order_acquire)) {
                            if (acquired) {
                                stream.mixer_users.fetch_sub(1, std::memory_order_release);
                            }
                            continue;
                        }
                        profile_scope.record_ring_occupancy(stream.audio.available_frames());
                        const auto consumed = stream.audio.accumulate(channel_mix.data(), block_frames, 1.0F);
                        profile_scope.record_unfilled_mix_frames(block_frames - consumed);
                        stream.mixer_users.fetch_sub(1, std::memory_order_release);
                    }

                    float gain = channel_start[channel_index] + channel_increment[channel_index] *
                        static_cast<float>(offset);
                    const auto output_offset = offset * kChannels;
                    for (std::size_t frame = 0; frame < block_frames; ++frame) {
                        samples[output_offset + frame * kChannels] += channel_mix[frame * kChannels] * gain;
                        samples[output_offset + frame * kChannels + 1] +=
                            channel_mix[frame * kChannels + 1] * gain;
                        gain += channel_increment[channel_index];
                    }
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
                    const auto consumed = capture_channel_.audio.read(channel_mix.data(), block_frames);
                    profile_scope.record_ring_occupancy(capture_channel_.audio.available_frames());
                    profile_scope.record_unfilled_mix_frames(block_frames - consumed);
                    const bool send_enabled = capture_channel_.speaker_send_enabled.load(std::memory_order_acquire);
                    if (send_enabled) {
                        const auto output_offset = offset * kChannels;
                        for (std::size_t sample = 0; sample < block_frames * kChannels; ++sample) {
                            samples[output_offset + sample] += channel_mix[sample];
                        }
                    }
                    capture_channel_.mixer_users.fetch_sub(1, std::memory_order_release);
                }
            }

            for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
                AppChannel& channel = app_channels_[channel_index];
                channel.smoothed_gain = channel.muted.load(std::memory_order_relaxed)
                    ? 0.0F : channel.target_gain.load(std::memory_order_relaxed);
            }
            data.chunk->offset = 0;
            data.chunk->stride = sizeof(float) * kChannels;
            data.chunk->size = frames * sizeof(float) * kChannels;
        }
    }
    pw_stream_queue_buffer(speaker_output_, buffer);
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
    const auto speaker_id = speaker_node_id_.load(std::memory_order_relaxed);
    bool linked = false;
    for (const auto& [link_id, link] : links_) {
        (void)link_id;
        if (link.output_node != speaker_id) {
            continue;
        }
        const auto target = nodes_.find(link.input_node);
        if (target != nodes_.end() && target->second.media_class == "Audio/Sink") {
            linked = true;
            break;
        }
    }
    output_linked_.store(linked, std::memory_order_relaxed);
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
        stream->application = node.application;
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
    if (speaker_output_ != nullptr) {
        pw_stream_disconnect(speaker_output_);
        pw_stream_destroy(speaker_output_);
        speaker_output_ = nullptr;
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
