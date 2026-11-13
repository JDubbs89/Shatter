#include "backend/pipewire/pipewire_backend.hpp"
#include "mixer/identifiers.hpp"

#include <pipewire/keys.h>
#include <pipewire/loop.h>
#include <pipewire/port.h>
#include <pipewire/stream.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace shatter::backend {

namespace {
constexpr char kSpeakerStreamName[] = "shatter.speakers";

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
}

bool PipeWireBackend::is_output_destination(const NodeInfo& node)
{
    return (node.media_class == "Audio/Sink" && !node.name.starts_with("shatter.stream.")) ||
        node.media_class == "Stream/Input/Audio";
}

std::string PipeWireBackend::output_destination_name(const NodeInfo& node)
{
    return node.description.empty() ? node.name : node.description;
}

std::string PipeWireBackend::capture_source_name(const NodeInfo& node)
{
    return node.description.empty() ? node.name : node.description;
}

std::string PipeWireBackend::output_destination_identity(const NodeInfo& node)
{
    if (node.media_class == "Stream/Input/Audio") {
        const std::string name = node.name.empty() ? node.description : node.name;
        const auto identity = mixer::stable_application_input_identity(node.application_id, node.process_binary,
                                                                 node.application, name);
        return identity.empty() ? "app-input-runtime:" + name + ":" + std::to_string(node.node_id) : identity;
    }
    const std::string name = output_destination_name(node);
    return mixer::stable_device_identity(node.device_identity, name);
}

std::string PipeWireBackend::output_destination_selector(const NodeInfo& node)
{
    const auto identity = output_destination_identity(node);
    return mixer::make_stable_selector_id(node.media_class == "Stream/Input/Audio" ? "input" : "device", identity);
}

void PipeWireBackend::refresh_output_link_locked()
{
    bool speaker_playback_linked = false;
    for (auto& output : output_channels_) {
        output.linked.store(false, std::memory_order_relaxed);
        output.sink_count = 0;
    }
    for (const auto& [link_id, link] : links_) {
        (void)link_id;
        const auto target = nodes_.find(link.input_node);
        if (target == nodes_.end() || !is_output_destination(target->second)) continue;
        for (auto& output : output_channels_) {
            if (output.in_use && output.node_id == link.output_node) {
                if (output.sink_count < output.sink_node_ids.size())
                    output.sink_node_ids[output.sink_count++] = link.input_node;
                output.linked.store(true, std::memory_order_release);
                if (&output == &output_channels_[0] && target->second.media_class == "Audio/Sink")
                    speaker_playback_linked = true;
            }
        }
    }
    output_linked_.store(speaker_playback_linked, std::memory_order_relaxed);
}

bool PipeWireBackend::make_output(std::string name, bool capture_source, std::string& error)
{
    OutputRequest request{};
    request.backend = this;
    request.action = OutputRequest::Action::make;
    request.first = std::move(name);
    request.capture_source = capture_source;
    return invoke_output_request(request, error);
}

bool PipeWireBackend::delete_output(std::string_view selector, std::string& error)
{
    trace_output("delete.enter", this, selector);
    OutputRequest request{};
    request.backend = this;
    request.action = OutputRequest::Action::remove;
    request.first = std::string(selector);
    return invoke_output_request(request, error);
}

bool PipeWireBackend::assign_output_destination(std::string_view output_selector,
                                               std::string_view destination_selector,
                                               bool assigned, std::string& error)
{
    OutputRequest request{};
    request.backend = this;
    request.action = OutputRequest::Action::assign_sink;
    request.first = std::string(output_selector);
    request.second = std::string(destination_selector);
    request.assigned = assigned;
    return invoke_output_request(request, error);
}

bool PipeWireBackend::assign_app_output(std::string_view app_selector, std::string_view output_selector,
                                       bool assigned, std::string& error)
{
    OutputRequest request{};
    request.backend = this;
    request.action = OutputRequest::Action::assign_app;
    request.first = std::string(app_selector);
    request.second = std::string(output_selector);
    request.assigned = assigned;
    return invoke_output_request(request, error);
}

bool PipeWireBackend::assign_input_output(std::string_view scope, std::string_view input_selector,
                                         std::string_view output_selector, bool assigned, std::string& error)
{
    OutputRequest request{};
    request.backend = this;
    request.action = OutputRequest::Action::assign_input;
    request.first = std::string(scope) + "\n" + std::string(input_selector);
    request.second = std::string(output_selector);
    request.assigned = assigned;
    return invoke_output_request(request, error);
}

bool PipeWireBackend::invoke_output_request(OutputRequest& request, std::string& error)
{
    if (main_loop_ == nullptr) { error = "PipeWire engine is not running"; return false; }
    const int result = pw_loop_invoke(pw_main_loop_get_loop(main_loop_), &PipeWireBackend::on_output_request,
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
        case OutputRequest::Action::assign_input: error = "could not update input channel output sends"; break;
        case OutputRequest::Action::load_config: error = "could not apply configuration"; break;
        }
    }
    trace_output("operation.invoke-return", &request,
                 "action=" + std::to_string(static_cast<int>(request.action)) +
                 " result=" + std::to_string(result) + " succeeded=" + std::to_string(request.succeeded) +
                 " error=" + error);
    return request.succeeded;
}

int PipeWireBackend::on_output_request(spa_loop*, bool, std::uint32_t, const void*, std::size_t, void* user_data)
{
    auto& request = *static_cast<OutputRequest*>(user_data);
    auto& backend = *request.backend;
    trace_output("loop.callback-enter", &request,
                 "action=" + std::to_string(static_cast<int>(request.action)) + " first=" + request.first);
    switch (request.action) {
    case OutputRequest::Action::make:
        request.succeeded = backend.make_output_locked(std::move(request.first), request.capture_source, request.error);
        break;
    case OutputRequest::Action::remove:
        request.succeeded = backend.delete_output_locked(request.first, request.error);
        break;
    case OutputRequest::Action::assign_sink: {
        std::lock_guard lock(backend.nodes_mutex_);
        request.succeeded = backend.assign_output_sink_locked(request.first, request.second,
                                                              request.assigned, request.error);
        break;
    }
    case OutputRequest::Action::assign_app: {
        std::lock_guard lock(backend.nodes_mutex_);
        request.succeeded = backend.assign_app_output_locked(request.first, request.second,
                                                             request.assigned, request.error);
        break;
    }
    case OutputRequest::Action::assign_input: {
        const auto separator = request.first.find('\n');
        const auto scope = std::string_view(request.first).substr(0, separator);
        const auto selector = separator == std::string::npos ? std::string_view{} :
            std::string_view(request.first).substr(separator + 1);
        std::lock_guard lock(backend.nodes_mutex_);
        request.succeeded = backend.channel_controls().assign_input_output_locked(
            scope, selector, request.second, request.assigned, request.error);
        break;
    }
    case OutputRequest::Action::load_config:
        request.succeeded = backend.apply_config_locked(std::move(request.show), request.error);
        break;
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

bool PipeWireBackend::make_output_locked(std::string name, bool capture_source, std::string& error)
{
    std::size_t slot = max_output_channels;
    {
        std::lock_guard lock(nodes_mutex_);
        for (std::size_t index = 1; index < output_channels_.size(); ++index) {
            if (!output_channels_[index].in_use) { slot = index; break; }
        }
    }
    if (slot == max_output_channels) {
        error = "Shatter reached its output channel limit";
        return false;
    }
    return make_output_at_locked(std::move(name), slot, capture_source, error);
}

bool PipeWireBackend::make_output_at_locked(std::string name, std::size_t slot, bool capture_source, std::string& error)
{
    if (name.empty() || name.size() > 128 || name.find_first_of("\r\n") != std::string::npos ||
        name.find('\0') != std::string::npos) {
        error = "output name must be 1 to 128 characters and cannot contain line breaks";
        return false;
    }
    if (name == kSpeakerStreamName ||
        name.starts_with("shatter.output.")) {
        error = "output name is reserved for a built-in or generated Shatter identity";
        return false;
    }
    std::unique_lock lock(nodes_mutex_);
    if (slot == 0 || slot >= output_channels_.size()) {
        error = "configuration output slot is outside the supported range";
        return false;
    }
    for (const auto& output : output_channels_) {
        if (output.in_use && output.name == name) {
            error = "an output channel already uses the name: " + name;
            return false;
        }
    }
    {
        const std::size_t index = slot;
        OutputChannel& output = output_channels_[index];
        if (output.in_use) { error = "configuration output slot is already in use"; return false; }
        output.input_generation.fetch_add(1, std::memory_order_acq_rel);
        output.backend = this;
        output.in_use = true;
        output.capture_source = capture_source;
        output.target_gain.store(1.0F, std::memory_order_relaxed);
        output.muted.store(false, std::memory_order_relaxed);
        output.smoothed_gain = 1.0F;
        output.assigned_sink_count = 0;
        output.meter.reset();
        const auto app_count = allocated_channels_.load(std::memory_order_relaxed);
        for (std::size_t app_index = 0; app_index < app_count; ++app_index)
            app_channels_[app_index].meters[index].reset();
        for (auto& assigned_name : output.assigned_sink_names) assigned_name.clear();
        for (auto& identity : output.assigned_sink_identities) identity.clear();
        output.name = std::move(name);
        output.identity = "shatter.output." + std::to_string(index);
        lock.unlock();
        if (!create_output_stream(output, false, error)) {
            lock.lock();
            output.in_use = false;
            output.name.clear();
            output.identity.clear();
            return false;
        }
        return true;
    }
}

bool PipeWireBackend::apply_config_locked(state::Show show, std::string& error)
{
    std::set<std::size_t> slots;
    std::set<std::string> identities;
    std::set<std::string> names{"speakers"};
    for (const auto& saved : show.outputs) {
        if (saved.slot == 0 || saved.slot >= max_output_channels ||
            saved.identity != "shatter.output." + std::to_string(saved.slot) ||
            saved.name.empty() || saved.name.size() > 128 || saved.name == "speakers" ||
            !slots.insert(saved.slot).second || !identities.insert(saved.identity).second ||
            !names.insert(saved.name).second || saved.devices.size() > 8 ||
            (saved.capture_source && !saved.devices.empty())) {
            error = "configuration contains an invalid output channel identity, name, or slot";
            return false;
        }
        for (const auto& device : saved.devices) {
            if (device.name.empty()) { error = "configuration contains an output device without a name"; return false; }
        }
    }

    for (std::size_t index = 1; index < output_channels_.size(); ++index) {
        if (output_channels_[index].in_use && !delete_output_locked(output_channels_[index].name, error))
            return false;
    }
    playback_route_state_.clear();
    for (const auto& saved : show.outputs) {
        if (!make_output_at_locked(saved.name, saved.slot, saved.capture_source, error)) return false;
    }

    {
        std::lock_guard lock(nodes_mutex_);
        loaded_show_ = std::move(show);
        const auto migrate_legacy_app_identity = [this](std::string& identity) {
            constexpr std::string_view prefix = "node:";
            if (!identity.starts_with(prefix)) return;
            std::uint32_t node_id = PW_ID_ANY;
            const std::string_view value(identity.data() + prefix.size(), identity.size() - prefix.size());
            const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), node_id);
            if (parse_error != std::errc{} || end != value.data() + value.size()) return;
            const auto node = nodes_.find(node_id);
            if (node == nodes_.end() || node->second.channel_index >= allocated_channels_.load()) return;
            identity = app_channels_[node->second.channel_index].identity;
        };
        for (auto& app : loaded_show_.apps) migrate_legacy_app_identity(app.identity);
        for (auto& stream : loaded_show_.streams) migrate_legacy_app_identity(stream.app_identity);
        for (const auto& saved : loaded_show_.outputs) {
            auto& output = output_channels_[saved.slot];
            output.assigned_sink_count = saved.devices.size();
            for (std::size_t i = 0; i < saved.devices.size(); ++i) {
                output.assigned_sink_node_ids[i] = PW_ID_ANY;
                output.assigned_sink_names[i] = saved.devices[i].name;
                output.assigned_sink_identities[i] = saved.devices[i].identity;
            }
            output.target_gain.store(std::pow(10.0F, saved.gain_db / 20.0F), std::memory_order_relaxed);
            output.muted.store(saved.muted, std::memory_order_relaxed);
        }
        for (std::size_t index = 0; index < allocated_channels_.load(); ++index) {
            auto& app = app_channels_[index];
            app.target_gain.store(1.0F, std::memory_order_relaxed);
            app.muted.store(false, std::memory_order_relaxed);
            app.output_mask.store(1U, std::memory_order_relaxed);
            for (const auto& saved : loaded_show_.apps) {
                if (saved.identity != app.identity) continue;
                app.name = saved.name;
                app.target_gain.store(std::pow(10.0F, saved.gain_db / 20.0F));
                app.muted.store(saved.muted);
                if (saved.has_output_sends)
                    app.output_mask.store(output_mask_for_identities(saved.output_identities));
                break;
            }
        }
        for (std::size_t index = 0; index < allocated_streams_.load(); ++index) {
            auto& stream = playback_streams_[index];
            if (!stream.in_use) continue;
            stream.target_gain.store(1.0F, std::memory_order_relaxed);
            stream.muted.store(false, std::memory_order_relaxed);
            stream.output_mask.store(0, std::memory_order_relaxed);
            const auto& app = app_channels_[stream.channel_index];
            for (const auto& saved : loaded_show_.streams) {
                if (app.identity != saved.app_identity || stream.original_name != saved.original_name) continue;
                stream.name = saved.name;
                stream.target_gain.store(std::pow(10.0F, saved.gain_db / 20.0F));
                stream.muted.store(saved.muted);
                if (saved.has_output_sends)
                    stream.output_mask.store(output_mask_for_identities(saved.output_identities));
                break;
            }
        }
        for (const auto& saved : loaded_show_.captures) {
            const bool exists = std::any_of(capture_channels_.begin(), capture_channels_.end(),
                [&](const auto& channel) { return channel.in_use && channel.identity == saved.identity; });
            if (exists) continue;
            auto slot = std::find_if(capture_channels_.begin(), capture_channels_.end(),
                [](const auto& channel) { return !channel.in_use; });
            if (slot == capture_channels_.end()) {
                error = "configuration contains more capture channels than Shatter can restore";
                return false;
            }
            slot->backend = this;
            slot->in_use = true;
            slot->name = saved.name;
            slot->identity = saved.identity;
            slot->selector_id = mixer::make_stable_selector_id("source", saved.identity);
            slot->source_node_id = PW_ID_ANY;
        }
        for (auto& channel : capture_channels_) {
            if (!channel.in_use) continue;
            const auto saved = std::find_if(loaded_show_.captures.begin(), loaded_show_.captures.end(),
                [&](const auto& item) { return item.identity == channel.identity; });
            if (saved != loaded_show_.captures.end()) {
                channel.output_mask.store(output_mask_for_identities(saved->output_identities),
                                          std::memory_order_release);
                channel.target_gain.store(std::pow(10.0F, saved->gain_db / 20.0F), std::memory_order_relaxed);
                channel.muted.store(saved->muted, std::memory_order_release);
            } else {
                channel.output_mask.store(loaded_show_.has_capture_sends
                    ? output_mask_for_identities(loaded_show_.capture_output_identities) : 0U,
                    std::memory_order_release);
                channel.target_gain.store(std::pow(10.0F, loaded_show_.capture_gain_db / 20.0F),
                                          std::memory_order_relaxed);
                channel.muted.store(loaded_show_.capture_muted, std::memory_order_release);
            }
        }
    }
    return true;
}

void PipeWireBackend::restore_output_routes_locked(OutputChannel& output)
{
    if (output.capture_source || output.node_id == PW_ID_ANY || output.assigned_sink_count == 0) return;
    std::vector<state::DeviceBinding> pending;
    std::vector<state::DeviceBinding> attached;
    std::vector<std::uint32_t> attached_node_ids;
    for (std::size_t i = 0; i < output.assigned_sink_count; ++i) {
        if (output.assigned_sink_node_ids[i] == PW_ID_ANY) {
            pending.push_back({output.assigned_sink_identities[i], output.assigned_sink_names[i]});
        } else {
            attached.push_back({output.assigned_sink_identities[i], output.assigned_sink_names[i]});
            attached_node_ids.push_back(output.assigned_sink_node_ids[i]);
        }
    }
    if (pending.empty()) return;

    output.assigned_sink_count = attached.size();
    for (auto& node_id : output.assigned_sink_node_ids) node_id = PW_ID_ANY;
    for (auto& name : output.assigned_sink_names) name.clear();
    for (auto& identity : output.assigned_sink_identities) identity.clear();
    for (std::size_t i = 0; i < attached.size(); ++i) {
        output.assigned_sink_node_ids[i] = attached_node_ids[i];
        output.assigned_sink_names[i] = std::move(attached[i].name);
        output.assigned_sink_identities[i] = std::move(attached[i].identity);
    }

    std::vector<bool> matched(pending.size(), false);
    for (std::size_t index = 0; index < pending.size(); ++index) {
        NodeInfo* match = nullptr;
        for (auto& [node_id, sink] : nodes_) {
            (void)node_id;
            if (!is_output_destination(sink)) continue;
            const std::string sink_name = output_destination_name(sink);
            const std::string identity = output_destination_identity(sink);
            const bool same_identity = !pending[index].identity.empty() &&
                (pending[index].identity == sink.device_identity || pending[index].identity == identity);
            const bool fallback_name = pending[index].identity.empty() && pending[index].name == sink_name;
            if (!same_identity && !fallback_name) continue;
            if (match != nullptr) { match = nullptr; break; }
            match = &sink;
        }
        if (match == nullptr) continue;
        if (std::find(output.assigned_sink_node_ids.begin(),
                      output.assigned_sink_node_ids.begin() + output.assigned_sink_count,
                      match->node_id) != output.assigned_sink_node_ids.begin() + output.assigned_sink_count) {
            matched[index] = true;
            continue;
        }
        if (output.assigned_sink_count != 0 && !output.linked.load(std::memory_order_acquire))
            continue;
        std::string route_error;
        const std::string sink_name = match->name.empty() ? match->description : match->name;
        const std::string sink_selector = output_destination_selector(*match);
        if (assign_output_sink_locked(output.name, sink_selector, true, route_error)) matched[index] = true;
    }
    for (std::size_t index = 0; index < pending.size() && output.assigned_sink_count < output.assigned_sink_node_ids.size(); ++index) {
        if (matched[index]) continue;
        const auto slot = output.assigned_sink_count++;
        output.assigned_sink_node_ids[slot] = PW_ID_ANY;
        output.assigned_sink_names[slot] = std::move(pending[index].name);
        output.assigned_sink_identities[slot] = std::move(pending[index].identity);
    }
}

bool PipeWireBackend::delete_output_locked(std::string_view selector, std::string& error)
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
    output->input_generation.fetch_add(1, std::memory_order_acq_rel);
    for (std::size_t i = 0; i < allocated_channels_.load(std::memory_order_relaxed); ++i)
        app_channels_[i].output_mask.fetch_and(~bit, std::memory_order_release);
    for (auto& channel : capture_channels_)
        channel.output_mask.fetch_and(~bit, std::memory_order_release);
    output->linked.store(false, std::memory_order_release);
    trace_output("delete.before-destroy", output, output->identity);
    lock.unlock();
    destroy_output_stream(*output);
    trace_output("delete.after-destroy", output, output->identity);
    for (auto& stream : playback_streams_) {
        stream.output_mask.fetch_and(~bit, std::memory_order_release);
        stream.audio[index].discard_all();
        stream.send_audio[index].discard_all();
    }
    for (auto& [identity, route] : playback_route_state_) {
        (void)identity;
        route.output_mask &= ~bit;
    }
    for (auto& channel : capture_channels_) channel.audio[index].discard_all();
    lock.lock();
    output->in_use = false;
    output->name.clear();
    output->identity.clear();
    output->node_id = PW_ID_ANY;
    output->sink_count = 0;
    output->assigned_sink_count = 0;
    for (auto& assigned_name : output->assigned_sink_names) assigned_name.clear();
    for (auto& identity : output->assigned_sink_identities) identity.clear();
    trace_output("delete.complete", output);
    return true;
}

bool PipeWireBackend::assign_output_sink_locked(std::string_view output_selector,
                                               std::string_view sink_selector, bool assigned,
                                               std::string& error)
{
    OutputChannel* output = resolve_output_locked(output_selector, error);
    if (output == nullptr) return false;
    if (output->capture_source) {
        error = "capture-type output is a virtual microphone source; select it in the application's input settings";
        return false;
    }
    if (output->node_id == PW_ID_ANY) {
        error = "output channel is not ready on the active audio backend";
        return false;
    }
    NodeInfo* sink = nullptr;
    for (auto& [id, node] : nodes_) {
        (void)id;
        if (!is_output_destination(node)) continue;
        const std::string name = output_destination_name(node);
        const bool stable_match = output_destination_selector(node) == sink_selector;
        const bool name_match = mixer::selector_name_matches(node.name, sink_selector) ||
            mixer::selector_name_matches(node.description, sink_selector);
        if (!stable_match && !name_match) continue;
        if (sink != nullptr) {
            error = "output destination selector is ambiguous; use its listed ID";
            return false;
        }
        sink = &node;
    }
    if (sink == nullptr) { error = "unknown output destination: " + std::string(sink_selector); return false; }
    const std::string identity = output_destination_identity(*sink);
    auto route_it = std::find(output->assigned_sink_node_ids.begin(),
                              output->assigned_sink_node_ids.begin() + output->assigned_sink_count,
                              sink->node_id);
    const bool route_exists = route_it != output->assigned_sink_node_ids.begin() + output->assigned_sink_count;
    std::size_t placeholder_index = output->assigned_sink_count;
    for (std::size_t i = 0; i < output->assigned_sink_count; ++i) {
        if (output->assigned_sink_node_ids[i] == PW_ID_ANY &&
            sink_binding_matches(*output, i, *sink)) placeholder_index = i;
    }
    if (assigned && route_exists) {
        error = "output channel is already assigned to playback sink: " + std::string(sink_selector);
        return false;
    }
    if (!assigned) {
        if (!route_exists) {
            error = "output channel is not assigned to playback sink: " + std::string(sink_selector);
            return false;
        }
        bool managed = false;
        for (const auto& route : managed_links_)
            if (route.output_node == output->node_id && route.sink_node == sink->node_id) managed = true;
        if (!managed) {
            std::uint32_t replacement_id = PW_ID_ANY;
            for (std::size_t i = 0; i < output->assigned_sink_count; ++i) {
                if (output->assigned_sink_node_ids[i] != sink->node_id) {
                    replacement_id = output->assigned_sink_node_ids[i];
                    break;
                }
            }
            if (replacement_id == PW_ID_ANY) {
                if (default_metadata_ != nullptr)
                    pw_metadata_set_property(default_metadata_, output->node_id, "target.object", "Spa:String", "-1");
            } else {
                const auto replacement = nodes_.find(replacement_id);
                if (replacement != nodes_.end()) {
                    for (auto it = managed_links_.begin(); it != managed_links_.end();) {
                        if (it->output_node == output->node_id && it->sink_node == replacement_id) {
                            if (it->proxy != nullptr) pw_core_destroy(core_, it->proxy);
                            it = managed_links_.erase(it);
                        } else ++it;
                    }
                    pw_properties* properties = pw_properties_new(PW_KEY_NODE_AUTOCONNECT, "true", nullptr);
                    if (properties != nullptr) {
                        pw_stream_update_properties(output->stream, &properties->dict);
                        pw_properties_free(properties);
                    }
                    if (default_metadata_ != nullptr)
                        pw_metadata_set_property(default_metadata_, output->node_id, "target.object", "Spa:String",
                                                 replacement->second.name.c_str());
                }
            }
        }
        for (auto it = managed_links_.begin(); it != managed_links_.end();) {
            if (it->output_node == output->node_id && it->sink_node == sink->node_id) {
                if (it->proxy != nullptr) pw_core_destroy(core_, it->proxy);
                it = managed_links_.erase(it);
            } else ++it;
        }
        for (std::size_t i = static_cast<std::size_t>(route_it - output->assigned_sink_node_ids.begin());
             i + 1 < output->assigned_sink_count; ++i)
        {
            output->assigned_sink_node_ids[i] = output->assigned_sink_node_ids[i + 1];
            output->assigned_sink_names[i] = std::move(output->assigned_sink_names[i + 1]);
            output->assigned_sink_identities[i] = std::move(output->assigned_sink_identities[i + 1]);
        }
        --output->assigned_sink_count;
        output->assigned_sink_node_ids[output->assigned_sink_count] = PW_ID_ANY;
        output->assigned_sink_names[output->assigned_sink_count].clear();
        output->assigned_sink_identities[output->assigned_sink_count].clear();
        refresh_output_link_locked();
        return true;
    }

    if (output->assigned_sink_count == 0 && sink->media_class == "Audio/Sink") {
        pw_properties* properties = pw_properties_new(PW_KEY_NODE_AUTOCONNECT, "true", nullptr);
        if (properties == nullptr) { error = "could not allocate output routing properties"; return false; }
        const int property_result = pw_stream_update_properties(output->stream, &properties->dict);
        pw_properties_free(properties);
        if (property_result < 0 || default_metadata_ == nullptr ||
            pw_metadata_set_property(default_metadata_, output->node_id, "target.object", "Spa:String", sink->name.c_str()) < 0) {
            error = "could not assign output to its first playback sink";
            return false;
        }
        output->assigned_sink_node_ids[0] = sink->node_id;
        output->assigned_sink_names[0] = output_destination_name(*sink);
        output->assigned_sink_identities[0] = identity;
        output->assigned_sink_count = 1;
        return true;
    }
    if (placeholder_index == 0 && output->assigned_sink_node_ids[0] == PW_ID_ANY &&
        std::none_of(managed_links_.begin(), managed_links_.end(), [output](const ManagedLink& link) {
            return link.output_node == output->node_id;
        })) {
        pw_properties* properties = pw_properties_new(PW_KEY_NODE_AUTOCONNECT, "false", nullptr);
        if (properties == nullptr) {
            error = "could not allocate output reconnection properties";
            return false;
        }
        const int property_result = pw_stream_update_properties(output->stream, &properties->dict);
        pw_properties_free(properties);
        if (property_result < 0 || (default_metadata_ != nullptr &&
            pw_metadata_set_property(default_metadata_, output->node_id, "target.object",
                                     "Spa:String", "-1") < 0)) {
            error = "could not prepare output for playback sink reconnection";
            return false;
        }
    }
    if (placeholder_index >= output->assigned_sink_count &&
        output->assigned_sink_count >= output->assigned_sink_node_ids.size()) {
        error = "output channel reached its playback sink limit";
        return false;
    }
    const std::vector<std::uint32_t> route_sink_ids{sink->node_id};

    std::vector<PortInfo> output_ports;
    std::unordered_map<std::uint32_t, std::vector<PortInfo>> sink_ports_by_node;
    for (const auto& [global_id, port] : ports_) {
        (void)global_id;
        if (port.node_id == output->node_id && !port.input) output_ports.push_back(port);
        if (port.input) sink_ports_by_node[port.node_id].push_back(port);
    }
    // PipeWire does not expose ports for every stream node to every client. Shatter's
    // output format is fixed stereo, and stream output port IDs are zero-based.
    auto& sink_ports = sink_ports_by_node[sink->node_id];
    if (output_ports.empty() && sink_ports.size() == 2) {
        output_ports.push_back({output->node_id, 0, "output_FL", false});
        output_ports.push_back({output->node_id, 1, "output_FR", false});
    }
    const auto order_ports = [](const PortInfo& left, const PortInfo& right) {
        return left.port_id < right.port_id;
    };
    std::sort(output_ports.begin(), output_ports.end(), order_ports);
    for (std::size_t i = 0; i < output_ports.size(); ++i)
        if (output_ports[i].name.empty()) output_ports[i].name = i == 0 ? "output_FL" : "output_FR";
    for (auto& [id, ports] : sink_ports_by_node) {
        (void)id;
        std::sort(ports.begin(), ports.end(), order_ports);
        for (std::size_t i = 0; i < ports.size(); ++i)
            if (ports[i].name.empty()) ports[i].name = i == 0 ? "playback_FL" : "playback_FR";
    }
    if (output_ports.empty() || output_ports.size() != sink_ports.size()) {
        error = "PipeWire output and sink ports are not ready or have incompatible channel counts (output=" +
            std::to_string(output_ports.size()) + ", sink=" + std::to_string(sink_ports.size()) + ")";
        return false;
    }

    std::vector<pw_proxy*> created_links;
    std::vector<std::uint32_t> created_link_sinks;
    for (const std::uint32_t sink_node_id : route_sink_ids) {
      auto ports = sink_ports_by_node[sink_node_id];
      if (ports.empty() && sink_ports.size() == 2) {
          ports.push_back({sink_node_id, 0, "playback_FL", true});
          ports.push_back({sink_node_id, 1, "playback_FR", true});
      }
      if (ports.size() != output_ports.size()) {
          error = "PipeWire sink ports are not ready or have incompatible channel counts";
          break;
      }
      for (std::size_t index = 0; index < output_ports.size(); ++index) {
        pw_properties* properties = pw_properties_new(
            PW_KEY_LINK_OUTPUT_NODE, std::to_string(output->node_id).c_str(),
            PW_KEY_LINK_OUTPUT_PORT, output_ports[index].name.c_str(),
            PW_KEY_LINK_INPUT_NODE, std::to_string(sink_node_id).c_str(),
            PW_KEY_LINK_INPUT_PORT, ports[index].name.c_str(),
            nullptr);
        if (properties == nullptr) {
            error = "could not allocate PipeWire link properties";
            break;
        }
        auto* proxy = static_cast<pw_proxy*>(pw_core_create_object(core_, "link-factory",
            PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &properties->dict, 0));
        pw_properties_free(properties);
        if (proxy == nullptr) {
            error = "PipeWire could not create an output sink link";
            break;
        }
        created_links.push_back(proxy);
        created_link_sinks.push_back(sink_node_id);
      }
      if (!error.empty()) break;
    }
    if (created_links.size() != output_ports.size() * route_sink_ids.size()) {
        for (pw_proxy* proxy : created_links) pw_core_destroy(core_, proxy);
        if (error.empty()) error = "PipeWire could not create the complete output sink route";
        return false;
    }

    for (std::size_t i = 0; i < created_links.size(); ++i)
        managed_links_.push_back({output->node_id, created_link_sinks[i], created_links[i]});
    if (placeholder_index < output->assigned_sink_count) {
        output->assigned_sink_node_ids[placeholder_index] = sink->node_id;
        output->assigned_sink_identities[placeholder_index] = identity;
    } else {
        output->assigned_sink_node_ids[output->assigned_sink_count] = sink->node_id;
        output->assigned_sink_names[output->assigned_sink_count] = output_destination_name(*sink);
        output->assigned_sink_identities[output->assigned_sink_count] = identity;
        ++output->assigned_sink_count;
    }
    return true;
}

PipeWireBackend::CaptureChannel* PipeWireBackend::resolve_capture_channel_locked(std::string_view selector, std::string& error)
{
    return static_cast<CaptureChannel*>(channel_controls().resolve_capture_channel_locked(selector, error));
}

PipeWireBackend::OutputChannel* PipeWireBackend::resolve_output_locked(std::string_view selector, std::string& error)
{
    return static_cast<OutputChannel*>(channel_controls().resolve_output_locked(selector, error));
}

bool PipeWireBackend::assign_app_output_locked(std::string_view app_selector,
                                                std::string_view output_selector, bool assigned,
                                                std::string& error)
{
    return channel_controls().assign_app_output_locked(app_selector, output_selector, assigned, error);
}

} // namespace shatter::backend
