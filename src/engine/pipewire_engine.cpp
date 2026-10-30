#include "shatter/engine/pipewire_engine.hpp"

#include <pipewire/keys.h>
#include <pipewire/stream.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace shatter::engine {
namespace {

constexpr char kAppSinkName[] = "shatter.app_playback";
constexpr char kAppSinkDescription[] = "Shatter App Playback";
constexpr char kSpeakerStreamName[] = "shatter.speakers";
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr float kMinimumGainDb = -60.0F;
constexpr float kMaximumGainDb = 12.0F;

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

    input_events_.version = PW_VERSION_STREAM_EVENTS;
    input_events_.state_changed = &PipeWireEngine::on_input_state_changed;
    input_events_.process = &PipeWireEngine::on_input_process;
    output_events_.version = PW_VERSION_STREAM_EVENTS;
    output_events_.state_changed = &PipeWireEngine::on_output_state_changed;
    output_events_.process = &PipeWireEngine::on_output_process;

    app_playback_sink_ = pw_stream_new(core_, kAppSinkDescription,
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                         PW_KEY_MEDIA_CATEGORY, "Playback",
                         PW_KEY_MEDIA_ROLE, "Production",
                         PW_KEY_MEDIA_CLASS, "Audio/Sink",
                         PW_KEY_NODE_NAME, kAppSinkName,
                         PW_KEY_NODE_DESCRIPTION, kAppSinkDescription,
                         PW_KEY_NODE_VIRTUAL, "true",
                         PW_KEY_AUDIO_CHANNELS, "2",
                         nullptr));
    if (app_playback_sink_ == nullptr) {
        error = "could not create the app playback channel endpoint";
        cleanup();
        return false;
    }
    pw_stream_add_listener(app_playback_sink_, &input_listener_, &input_events_, this);

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

    std::array<std::uint8_t, 1024> input_format_storage{};
    spa_pod_builder input_builder = SPA_POD_BUILDER_INIT(input_format_storage.data(), input_format_storage.size());
    const spa_pod* input_params[] = {build_stereo_format(&input_builder)};
    const auto input_result = pw_stream_connect(
        app_playback_sink_, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        input_params, 1);
    if (input_result < 0) {
        error = "could not connect the app playback channel to PipeWire";
        cleanup();
        return false;
    }

    std::array<std::uint8_t, 1024> output_format_storage{};
    spa_pod_builder output_builder = SPA_POD_BUILDER_INIT(output_format_storage.data(), output_format_storage.size());
    const spa_pod* output_params[] = {build_stereo_format(&output_builder)};
    const auto output_result = pw_stream_connect(
        speaker_output_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        output_params, 1);
    if (output_result < 0) {
        error = "could not connect the speaker output to PipeWire";
        cleanup();
        return false;
    }

    loop_thread_ = std::thread(&PipeWireEngine::run_loop, this);
    std::unique_lock lock(state_mutex_);
    const bool ready = state_changed_.wait_for(lock, std::chrono::seconds(5), [this] {
        return !startup_error_.empty() || (input_ready_ && output_ready_ && output_linked_.load());
    });
    if (!ready || !startup_error_.empty()) {
        error = !startup_error_.empty() ? startup_error_
               : (output_ready_ && !output_linked_.load())
                   ? "speaker playback stream is not linked to a PipeWire sink"
                   : "PipeWire streams did not become ready";
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

bool PipeWireEngine::set_app_gain_db(float decibels, std::string& error)
{
    if (!available_) {
        error = "PipeWire audio is unavailable";
        return false;
    }
    if (!std::isfinite(decibels) || decibels < kMinimumGainDb || decibels > kMaximumGainDb) {
        error = "gain must be between -60 dB and +12 dB";
        return false;
    }

    target_gain_.store(std::pow(10.0F, decibels / 20.0F), std::memory_order_relaxed);
    return true;
}

std::string PipeWireEngine::list_inputs() const
{
    std::lock_guard lock(nodes_mutex_);
    std::ostringstream table;
    table << "Shatter inputs\n"
          << std::left << std::setw(24) << "Channel" << std::setw(22) << "Type" << "PipeWire endpoint\n"
          << std::string(78, '-') << '\n'
          << std::setw(24) << "app-playback" << std::setw(22) << "App playback"
          << kAppSinkName << (available() ? " (virtual sink)\n" : " (PipeWire unavailable)\n");

    std::vector<std::pair<std::string, std::string>> assigned_apps;
    const auto app_sink_id = app_sink_node_id_.load(std::memory_order_relaxed);
    for (const auto& [link_id, link] : links_) {
        (void)link_id;
        if (link.input_node != app_sink_id) {
            continue;
        }
        const auto stream = nodes_.find(link.output_node);
        if (stream == nodes_.end() || stream->second.media_class != "Stream/Output/Audio") {
            continue;
        }
        const std::string name = stream->second.application.empty()
            ? (stream->second.name.empty() ? stream->second.description : stream->second.name)
            : stream->second.application;
        assigned_apps.emplace_back(name, stream->second.name);
    }
    std::sort(assigned_apps.begin(), assigned_apps.end());
    for (const auto& [application, endpoint] : assigned_apps) {
        table << std::setw(24) << application << std::setw(22) << "Assigned app stream"
              << endpoint << '\n';
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
        table << std::setw(24) << "available" << std::setw(22) << "Capture source"
              << (node.name.empty() ? node.description : node.name) << " (available; not routed)\n";
    }
    if (capture_sources.empty()) {
        table << std::setw(24) << "available" << std::setw(22) << "Capture source"
              << "No PipeWire capture sources found\n";
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
        if (target != nodes_.end()) {
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
        if (node.media_class == "Audio/Sink" && node.name != kAppSinkName) {
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

void PipeWireEngine::on_input_state_changed(void* data, pw_stream_state, pw_stream_state state,
                                            const char* error)
{
    static_cast<PipeWireEngine*>(data)->update_stream_state(true, state, error);
}

void PipeWireEngine::on_output_state_changed(void* data, pw_stream_state, pw_stream_state state,
                                             const char* error)
{
    static_cast<PipeWireEngine*>(data)->update_stream_state(false, state, error);
}

void PipeWireEngine::on_input_process(void* data)
{
    static_cast<PipeWireEngine*>(data)->input_process();
}

void PipeWireEngine::on_output_process(void* data)
{
    static_cast<PipeWireEngine*>(data)->output_process();
}

void PipeWireEngine::on_global(void* data, std::uint32_t id, std::uint32_t,
                               const char* type, std::uint32_t, const spa_dict* props)
{
    if (type == nullptr || props == nullptr) {
        return;
    }

    auto& engine = *static_cast<PipeWireEngine*>(data);
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
    node.media_class = media_class;
    if (const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) {
        node.name = name;
    }
    if (const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION)) {
        node.description = description;
    }
    if (const char* application = spa_dict_lookup(props, PW_KEY_APP_NAME)) {
        node.application = application;
    }

    {
        std::lock_guard lock(engine.nodes_mutex_);
        engine.nodes_[id] = std::move(node);
        engine.refresh_output_link_locked();
    }
    engine.state_changed_.notify_all();
}

void PipeWireEngine::on_global_remove(void* data, std::uint32_t id)
{
    auto& engine = *static_cast<PipeWireEngine*>(data);
    std::lock_guard lock(engine.nodes_mutex_);
    engine.nodes_.erase(id);
    engine.links_.erase(id);
    engine.refresh_output_link_locked();
    engine.state_changed_.notify_all();
}

void PipeWireEngine::input_process()
{
    pw_buffer* pipewire_buffer = pw_stream_dequeue_buffer(app_playback_sink_);
    if (pipewire_buffer == nullptr || pipewire_buffer->buffer == nullptr ||
        pipewire_buffer->buffer->n_datas == 0) {
        return;
    }

    spa_data& data = pipewire_buffer->buffer->datas[0];
    if (data.data != nullptr && data.chunk != nullptr) {
        const auto stride = data.chunk->stride > 0 ? static_cast<std::size_t>(data.chunk->stride)
                                                   : sizeof(float) * kChannels;
        const auto frames = static_cast<std::size_t>(data.chunk->size) / stride;
        const auto* samples = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(data.data) + data.chunk->offset);
        audio_ring_.write(samples, frames);
    }
    pw_stream_queue_buffer(app_playback_sink_, pipewire_buffer);
}

void PipeWireEngine::output_process()
{
    pw_buffer* pipewire_buffer = pw_stream_dequeue_buffer(speaker_output_);
    if (pipewire_buffer == nullptr || pipewire_buffer->buffer == nullptr ||
        pipewire_buffer->buffer->n_datas == 0) {
        return;
    }

    spa_data& data = pipewire_buffer->buffer->datas[0];
    if (data.data != nullptr && data.chunk != nullptr) {
        const auto frames = static_cast<std::size_t>(data.maxsize) / (sizeof(float) * kChannels);
        auto* samples = reinterpret_cast<float*>(data.data);
        audio_ring_.read(samples, frames);

        const float target = target_gain_.load(std::memory_order_relaxed);
        const float increment = frames == 0 ? 0.0F : (target - smoothed_gain_) / static_cast<float>(frames);
        float gain = smoothed_gain_;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            samples[frame * kChannels] *= gain;
            samples[frame * kChannels + 1] *= gain;
            gain += increment;
        }
        smoothed_gain_ = target;
        data.chunk->offset = 0;
        data.chunk->stride = sizeof(float) * kChannels;
        data.chunk->size = frames * sizeof(float) * kChannels;
    }
    pw_stream_queue_buffer(speaker_output_, pipewire_buffer);
}

void PipeWireEngine::update_stream_state(bool input, pw_stream_state state, const char* error)
{
    {
        std::lock_guard lock(state_mutex_);
        if (input) {
            input_ready_ = stream_is_ready(state);
        } else {
            output_ready_ = stream_is_ready(state);
        }
        if (state == PW_STREAM_STATE_ERROR) {
            startup_error_ = error == nullptr ? "PipeWire stream failed" : error;
        }
    }
    if (stream_is_ready(state)) {
        if (input && app_playback_sink_ != nullptr) {
            app_sink_node_id_.store(pw_stream_get_node_id(app_playback_sink_), std::memory_order_relaxed);
        } else if (!input && speaker_output_ != nullptr) {
            speaker_node_id_.store(pw_stream_get_node_id(speaker_output_), std::memory_order_relaxed);
        }
        std::lock_guard lock(nodes_mutex_);
        refresh_output_link_locked();
    }
    state_changed_.notify_all();
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
    if (app_playback_sink_ != nullptr) {
        pw_stream_disconnect(app_playback_sink_);
        pw_stream_destroy(app_playback_sink_);
        app_playback_sink_ = nullptr;
    }
    if (speaker_output_ != nullptr) {
        pw_stream_disconnect(speaker_output_);
        pw_stream_destroy(speaker_output_);
        speaker_output_ = nullptr;
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
