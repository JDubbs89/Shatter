#include "shatter/engine/pipewire_engine.hpp"

#include <pipewire/keys.h>
#include <pipewire/loop.h>
#include <pipewire/stream.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace shatter::engine {

namespace {
constexpr char kSpeakerStreamName[] = "shatter.speakers";
constexpr char kTargetObjectKey[] = "target.object";
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

} // namespace shatter::engine
