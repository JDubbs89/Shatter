#include "shatter/engine/pipewire_engine.hpp"

#include <pipewire/keys.h>
#include <pipewire/loop.h>
#include <pipewire/port.h>
#include <pipewire/stream.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace shatter::engine {

namespace {
constexpr char kSpeakerStreamName[] = "shatter.speakers";
constexpr std::uint32_t kMixerClosed = 1U << 31U;
constexpr std::uint32_t kMixerUserMask = kMixerClosed - 1U;

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

void PipeWireEngine::refresh_output_link_locked()
{
    for (auto& output : output_channels_) {
        output.linked.store(false, std::memory_order_relaxed);
        output.sink_count = 0;
    }
    for (const auto& [link_id, link] : links_) {
        (void)link_id;
        const auto target = nodes_.find(link.input_node);
        if (target == nodes_.end() || target->second.media_class != "Audio/Sink" ||
            target->second.name.starts_with("shatter.stream.")) continue;
        for (auto& output : output_channels_) {
            if (output.in_use && output.node_id == link.output_node) {
                if (output.sink_count < output.sink_node_ids.size())
                    output.sink_node_ids[output.sink_count++] = link.input_node;
                output.linked.store(true, std::memory_order_release);
            }
        }
    }
    output_linked_.store(output_channels_[0].linked.load(std::memory_order_relaxed), std::memory_order_relaxed);
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

bool PipeWireEngine::assign_output_destination(std::string_view output_selector,
                                               std::string_view destination_selector,
                                               bool assigned, std::string& error)
{
    OutputRequest request{};
    request.engine = this;
    request.action = OutputRequest::Action::assign_sink;
    request.first = std::string(output_selector);
    request.second = std::string(destination_selector);
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

bool PipeWireEngine::assign_input_output(std::string_view scope, std::string_view input_selector,
                                         std::string_view output_selector, bool assigned, std::string& error)
{
    OutputRequest request{};
    request.engine = this;
    request.action = OutputRequest::Action::assign_input;
    request.first = std::string(scope) + "\n" + std::string(input_selector);
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
    case OutputRequest::Action::assign_input: {
        const auto separator = request.first.find('\n');
        const auto scope = std::string_view(request.first).substr(0, separator);
        const auto selector = separator == std::string::npos ? std::string_view{} :
            std::string_view(request.first).substr(separator + 1);
        std::lock_guard lock(engine.nodes_mutex_);
        OutputChannel* output = engine.resolve_output_locked(request.second, request.error);
        if (output != nullptr) {
            const auto index = static_cast<std::size_t>(output - engine.output_channels_.data());
            const std::uint32_t bit = 1U << index;
            std::atomic<std::uint32_t>* mask = nullptr;
            if (scope == "stream") {
                auto* stream = engine.resolve_stream_locked(selector, request.error);
                if (stream != nullptr) mask = &stream->output_mask;
            } else if (scope == "capture") {
                if (selector != "capture") request.error = "capture input selector must be 'capture'";
                else if (request.assigned && !engine.capture_channel_.active.load(std::memory_order_acquire))
                    request.error = "select an active capture source before assigning its send";
                else mask = &engine.capture_channel_.output_mask;
            } else request.error = "input send scope must be stream or capture";
            if (mask != nullptr) {
                auto value = mask->load(std::memory_order_relaxed);
                value = request.assigned ? (value | bit) : (value & ~bit);
                mask->store(value, std::memory_order_release);
                request.succeeded = true;
            }
        }
        break;
    }
    case OutputRequest::Action::load_config:
        request.succeeded = engine.apply_config_locked(std::move(request.show), request.error);
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

bool PipeWireEngine::make_output_locked(std::string name, std::string& error)
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
    return make_output_at_locked(std::move(name), slot, error);
}

bool PipeWireEngine::make_output_at_locked(std::string name, std::size_t slot, std::string& error)
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
        output.engine = this;
        output.in_use = true;
        output.target_gain.store(1.0F, std::memory_order_relaxed);
        output.muted.store(false, std::memory_order_relaxed);
        output.smoothed_gain = 1.0F;
        output.input_users.store(0, std::memory_order_release);
        output.assigned_sink_count = 0;
        for (auto& assigned_name : output.assigned_sink_names) assigned_name.clear();
        for (auto& identity : output.assigned_sink_identities) identity.clear();
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
}

bool PipeWireEngine::apply_config_locked(state::Show show, std::string& error)
{
    std::set<std::size_t> slots;
    std::set<std::string> identities;
    std::set<std::string> names{"speakers"};
    for (const auto& saved : show.outputs) {
        if (saved.slot == 0 || saved.slot >= max_output_channels ||
            saved.identity != "shatter.output." + std::to_string(saved.slot) ||
            saved.name.empty() || saved.name.size() > 128 || saved.name == "speakers" ||
            !slots.insert(saved.slot).second || !identities.insert(saved.identity).second ||
            !names.insert(saved.name).second || saved.sinks.size() > 8) {
            error = "configuration contains an invalid output channel identity, name, or slot";
            return false;
        }
        for (const auto& sink : saved.sinks) {
            if (sink.name.empty()) { error = "configuration contains an output sink without a name"; return false; }
        }
    }

    for (std::size_t index = 1; index < output_channels_.size(); ++index) {
        if (output_channels_[index].in_use && !delete_output_locked(output_channels_[index].name, error))
            return false;
    }
    playback_route_state_.clear();
    for (const auto& saved : show.outputs) {
        if (!make_output_at_locked(saved.name, saved.slot, error)) return false;
    }

    {
        std::lock_guard lock(nodes_mutex_);
        loaded_show_ = std::move(show);
        for (const auto& saved : loaded_show_.outputs) {
            auto& output = output_channels_[saved.slot];
            output.assigned_sink_count = saved.sinks.size();
            for (std::size_t i = 0; i < saved.sinks.size(); ++i) {
                output.assigned_sink_node_ids[i] = PW_ID_ANY;
                output.assigned_sink_names[i] = saved.sinks[i].name;
                output.assigned_sink_identities[i] = saved.sinks[i].identity;
            }
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
        capture_channel_.output_mask.store(loaded_show_.has_capture_sends
            ? output_mask_for_identities(loaded_show_.capture_output_identities) : 0U,
            std::memory_order_release);
    }
    return true;
}

void PipeWireEngine::restore_output_routes_locked(OutputChannel& output)
{
    if (output.node_id == PW_ID_ANY || output.assigned_sink_count == 0) return;
    std::vector<state::SinkBinding> pending;
    std::vector<state::SinkBinding> attached;
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
            if (sink.media_class != "Audio/Sink" || sink.name.starts_with("shatter.stream.")) continue;
            const bool same_identity = !pending[index].identity.empty() &&
                pending[index].identity == sink.device_identity;
            const bool fallback_name = (pending[index].identity.empty() || sink.device_identity.empty()) &&
                pending[index].name == sink.name;
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
        const std::string sink_selector = "sink-" + std::to_string(match->node_id);
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
        stream.output_mask.fetch_and(~bit, std::memory_order_release);
        stream.audio[index].discard_all();
        stream.send_audio[index].discard_all();
    }
    for (auto& [identity, route] : playback_route_state_) {
        (void)identity;
        route.output_mask &= ~bit;
    }
    capture_channel_.audio[index].discard_all();
    lock.lock();
    output->in_use = false;
    output->name.clear();
    output->node_name.clear();
    output->node_id = PW_ID_ANY;
    output->sink_count = 0;
    output->assigned_sink_count = 0;
    for (auto& assigned_name : output->assigned_sink_names) assigned_name.clear();
    for (auto& identity : output->assigned_sink_identities) identity.clear();
    trace_output("delete.complete", output);
    return true;
}

PipeWireEngine::OutputChannel* PipeWireEngine::resolve_output_locked(std::string_view selector,
                                                                    std::string& error)
{
    std::uint32_t node_id = PW_ID_ANY;
    const auto [end, parse_error] = std::from_chars(selector.data(), selector.data() + selector.size(), node_id);
    OutputChannel* match = nullptr;
    for (auto& output : output_channels_) {
        if (!output.in_use) continue;
        if (output.name == selector || output.node_name == selector ||
            (parse_error == std::errc{} && end == selector.data() + selector.size() && output.node_id == node_id)) {
            if (match != nullptr) {
                error = "output selector is ambiguous; use its PipeWire node ID";
                return nullptr;
            }
            match = &output;
        }
    }
    if (match != nullptr) return match;
    error = "unknown output channel or PipeWire node: " + std::string(selector);
    return nullptr;
}

bool PipeWireEngine::assign_output_sink_locked(std::string_view output_selector,
                                               std::string_view sink_selector, bool assigned,
                                               std::string& error)
{
    OutputChannel* output = resolve_output_locked(output_selector, error);
    if (output == nullptr) return false;
    if (output->node_id == PW_ID_ANY) {
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

    if (output->assigned_sink_count == 0) {
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
        output->assigned_sink_names[0] = sink->name;
        output->assigned_sink_identities[0] = sink->device_identity;
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
        output->assigned_sink_identities[placeholder_index] = sink->device_identity;
    } else {
        output->assigned_sink_node_ids[output->assigned_sink_count] = sink->node_id;
        output->assigned_sink_names[output->assigned_sink_count] = sink->name;
        output->assigned_sink_identities[output->assigned_sink_count] = sink->device_identity;
        ++output->assigned_sink_count;
    }
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

} // namespace shatter::engine
