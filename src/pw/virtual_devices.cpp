#include "shatter/engine/pipewire_engine.hpp"

#include <pipewire/keys.h>
#include <pipewire/stream.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>

#include <array>
#include <cmath>
#include <utility>

namespace shatter::engine {
namespace {
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 2;
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
        for (const auto& saved : loaded_show_.streams) {
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

} // namespace shatter::engine
