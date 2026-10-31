#include "pipewire_engine.hpp"

#include <pipewire/keys.h>
#include <pipewire/port.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <thread>
#include <utility>

namespace shatter::engine {
namespace {
constexpr char kSpeakerStreamName[] = "shatter.speakers";
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
            for (auto& output : engine.output_channels_)
                if (output.in_use && output.node_id == link.output_node)
                    engine.restore_output_routes_locked(output);
            engine.refresh_output_link_locked();
        }
        engine.state_changed_.notify_all();
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        const char* node_text = spa_dict_lookup(props, PW_KEY_NODE_ID);
        const char* port_text = spa_dict_lookup(props, PW_KEY_PORT_ID);
        const char* direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        if (node_text == nullptr || port_text == nullptr || direction == nullptr) return;
        PortInfo port;
        const auto node_parse = std::from_chars(node_text, node_text + std::strlen(node_text), port.node_id);
        const auto port_parse = std::from_chars(port_text, port_text + std::strlen(port_text), port.port_id);
        if (node_parse.ec != std::errc{} || port_parse.ec != std::errc{}) return;
        port.input = std::strcmp(direction, "in") == 0;
        if (const char* name = spa_dict_lookup(props, PW_KEY_PORT_NAME)) port.name = name;
        std::lock_guard lock(engine.nodes_mutex_);
        engine.ports_[id] = port;
        const auto sink = engine.nodes_.find(port.node_id);
        for (auto& output : engine.output_channels_)
            if (output.in_use && (output.node_id == port.node_id ||
                (sink != engine.nodes_.end() && sink->second.media_class == "Audio/Sink")))
                engine.restore_output_routes_locked(output);
        engine.refresh_output_link_locked();
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
    if (const char* serial = spa_dict_lookup(props, PW_KEY_DEVICE_SERIAL)) node.device_identity = serial;
    if (node.device_identity.empty()) {
        if (const char* path = spa_dict_lookup(props, PW_KEY_OBJECT_PATH)) node.device_identity = path;
    }
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
    const auto added = engine.nodes_.find(id);
    if (added != engine.nodes_.end() && added->second.media_class == "Audio/Sink") {
        for (auto& output : engine.output_channels_)
            if (output.in_use) engine.restore_output_routes_locked(output);
    }
    engine.refresh_output_link_locked();
}

void PipeWireEngine::on_global_remove(void* data, std::uint32_t id)
{
    auto& engine = *static_cast<PipeWireEngine*>(data);
    PlaybackStream* removed = nullptr;
    bool removed_capture_source = false;
    {
        std::lock_guard lock(engine.nodes_mutex_);
        const auto removed_node = engine.nodes_.find(id);
        if (removed_node != engine.nodes_.end() && removed_node->second.media_class == "Audio/Sink") {
            for (auto& output : engine.output_channels_) {
                if (!output.in_use) continue;
                for (std::size_t i = 0; i < output.assigned_sink_count; ++i)
                    if (output.assigned_sink_node_ids[i] == id) output.assigned_sink_node_ids[i] = PW_ID_ANY;
            }
        }
        removed_capture_source = engine.capture_channel_.source_node_id == id;
        const auto stream = engine.stream_by_node_.find(id);
        if (stream != engine.stream_by_node_.end()) {
            removed = &engine.playback_streams_[stream->second];
            const auto app_identity = engine.app_channels_[removed->channel_index].identity;
            engine.playback_route_state_[engine.playback_route_key(app_identity, removed->original_name)] = {
                removed->name,
                removed->target_gain.load(std::memory_order_relaxed),
                removed->muted.load(std::memory_order_relaxed),
                removed->output_mask.load(std::memory_order_relaxed)
            };
            removed->active.store(false, std::memory_order_release);
            removed->in_use = false;
            removed->process_gate.close();
            removed->mixer_gate.close();
            engine.stream_by_node_.erase(stream);
        }
        engine.nodes_.erase(id);
        engine.links_.erase(id);
        engine.ports_.erase(id);
        for (auto it = engine.managed_links_.begin(); it != engine.managed_links_.end();) {
            if (it->output_node == id || it->sink_node == id) {
                if (it->proxy != nullptr) pw_proxy_destroy(it->proxy);
                it = engine.managed_links_.erase(it);
            }
            else ++it;
        }
        engine.refresh_output_link_locked();
    }
    if (removed_capture_source) {
        engine.destroy_capture_stream();
        engine.capture_channel_.output_mask.store(0, std::memory_order_release);
    }
    if (removed != nullptr) {
        if (removed->sink != nullptr) {
            pw_stream_disconnect(removed->sink);
            removed->process_gate.wait_for_quiescence();
            pw_stream_destroy(removed->sink);
            removed->sink = nullptr;
        }
        removed->process_gate.wait_for_quiescence();
        removed->mixer_gate.wait_for_quiescence();
        for (auto& ring : removed->audio) ring.discard_all();
        for (auto& ring : removed->send_audio) ring.discard_all();
        std::lock_guard lock(engine.nodes_mutex_);
        removed->name.clear();
        removed->original_name.clear();
        removed->application.clear();
        removed->sink_name.clear();
        removed->node_id = PW_ID_ANY;
        removed->in_use = false;
        removed->process_gate.reopen();
        removed->mixer_gate.reopen();
    }
    engine.state_changed_.notify_all();
}

} // namespace shatter::engine
