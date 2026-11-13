#include "backend/pipewire/pipewire_backend.hpp"
#include "mixer/identifiers.hpp"

#include <algorithm>
#include <utility>

namespace shatter::backend {

bool PipeWireBackend::enumerate_endpoints(std::vector<Endpoint>& endpoints, std::string& error) const
{
    std::lock_guard lock(nodes_mutex_);
    if (main_loop_ == nullptr) {
        error = "PipeWire engine is not running";
        return false;
    }
    std::vector<Endpoint> result;
    for (const auto& [id, node] : nodes_) {
        if (node.application == "Shatter") continue;
        Endpoint endpoint;
        endpoint.name = node.description.empty() ? node.name : node.description;
        if (node.media_class == "Audio/Sink") {
            endpoint.kind = EndpointKind::playback_device;
            endpoint.id = output_destination_selector(node);
            endpoint.identity = output_destination_identity(node);
        } else if (node.media_class == "Audio/Source") {
            endpoint.kind = EndpointKind::capture_device;
            endpoint.identity = mixer::stable_device_identity(node.device_identity, capture_source_name(node));
            endpoint.id = mixer::make_stable_selector_id("source", endpoint.identity);
        } else if (node.media_class == "Stream/Input/Audio") {
            endpoint.kind = EndpointKind::application_capture;
            endpoint.id = output_destination_selector(node);
            endpoint.identity = output_destination_identity(node);
        } else if (node.media_class == "Stream/Output/Audio") {
            endpoint.kind = EndpointKind::playback_stream;
            const auto stream = stream_by_node_.find(id);
            if (stream == stream_by_node_.end()) continue;
            endpoint.id = playback_streams_[stream->second].selector_id;
            endpoint.identity = node.name;
        } else continue;
        result.push_back(std::move(endpoint));
    }
    std::sort(result.begin(), result.end(), [](const Endpoint& left, const Endpoint& right) {
        return left.id < right.id;
    });
    endpoints = std::move(result);
    error.clear();
    return true;
}

} // namespace shatter::backend
