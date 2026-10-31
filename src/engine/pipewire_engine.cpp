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
constexpr std::size_t kFaderTrackWidth = 25;
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

using TableRow = std::vector<std::string>;

void append_table_border(std::ostringstream& table, const std::vector<std::size_t>& widths)
{
    table << '+';
    for (const std::size_t width : widths) table << std::string(width + 2, '-') << '+';
    table << '\n';
}

void append_table_row(std::ostringstream& table, const TableRow& row,
                      const std::vector<std::size_t>& widths)
{
    table << '|';
    for (std::size_t column = 0; column < widths.size(); ++column) {
        const std::string_view value = column < row.size() ? std::string_view(row[column]) : std::string_view{};
        table << ' ' << std::left << std::setw(static_cast<int>(widths[column]))
              << fit_column(value, widths[column]) << " |";
    }
    table << '\n';
}

void append_table(std::ostringstream& table, const std::vector<std::string>& headers,
                  const std::vector<TableRow>& rows, const std::vector<std::size_t>& maximum_widths)
{
    std::vector<std::size_t> widths(headers.size(), 1);
    for (std::size_t column = 0; column < headers.size(); ++column) {
        widths[column] = std::min(headers[column].size(), maximum_widths[column]);
        for (const auto& row : rows) {
            if (column < row.size()) widths[column] = std::max(widths[column],
                std::min(row[column].size(), maximum_widths[column]));
        }
    }
    append_table_border(table, widths);
    append_table_row(table, headers, widths);
    append_table_border(table, widths);
    for (const auto& row : rows) append_table_row(table, row, widths);
    append_table_border(table, widths);
}

float gain_db(float linear)
{
    return linear <= 0.0F ? -60.0F : std::max(-60.0F, 20.0F * std::log10(linear));
}

std::string format_gain(float linear)
{
    std::ostringstream value;
    value << std::fixed << std::setprecision(1) << gain_db(linear) << " dB";
    return value.str();
}

std::string format_fader_position(float linear)
{
    const float decibels = gain_db(linear);
    const float normalized = std::clamp((decibels - kMinimumGainDb) /
        (kMaximumGainDb - kMinimumGainDb), 0.0F, 1.0F);
    const auto marker = static_cast<std::size_t>(std::lround(normalized * (kFaderTrackWidth - 1)));
    std::string track(kFaderTrackWidth, '-');
    track[marker] = 'O';

    std::ostringstream position;
    position << "-inf |" << track << "| +12.0 dB  ";
    if (linear <= 0.0F || decibels <= kMinimumGainDb) position << "-inf";
    else position << format_gain(linear);
    return position.str();
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
    speakers.target_gain.store(1.0F, std::memory_order_relaxed);
    speakers.muted.store(false, std::memory_order_relaxed);
    speakers.smoothed_gain = 1.0F;

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

bool PipeWireEngine::set_capture_gain_db(float decibels, std::string& error)
{
    if (!std::isfinite(decibels) || decibels < kMinimumGainDb || decibels > kMaximumGainDb) {
        error = "gain must be between -60 dB and +12 dB";
        return false;
    }
    capture_channel_.target_gain.store(std::pow(10.0F, decibels / 20.0F), std::memory_order_relaxed);
    return true;
}

bool PipeWireEngine::set_capture_muted(bool muted, std::string&)
{
    capture_channel_.muted.store(muted, std::memory_order_release);
    return true;
}

bool PipeWireEngine::adjust_fader_db(std::string_view scope, std::string_view selector, float delta_db,
                                     bool minimum, std::string& error)
{
    if (!minimum && !std::isfinite(delta_db)) {
        error = "fader increment must be a finite number of dB or -inf";
        return false;
    }
    const auto adjusted = [delta_db, minimum](float current_gain) {
        if (minimum) return 0.0F;
        const float target_db = std::clamp(gain_db(current_gain) + delta_db,
                                           kMinimumGainDb, kMaximumGainDb);
        return std::pow(10.0F, target_db / 20.0F);
    };

    if (scope == "app") {
        std::lock_guard lock(nodes_mutex_);
        AppChannel* channel = resolve_channel_locked(selector, error);
        if (channel == nullptr) return false;
        channel->target_gain.store(adjusted(channel->target_gain.load(std::memory_order_relaxed)),
                                   std::memory_order_release);
        return true;
    }
    if (scope == "stream") {
        std::lock_guard lock(nodes_mutex_);
        PlaybackStream* stream = resolve_stream_locked(selector, error);
        if (stream == nullptr || !stream->active.load(std::memory_order_relaxed)) {
            if (error.empty()) error = "unknown or inactive playback stream: " + std::string(selector);
            return false;
        }
        stream->target_gain.store(adjusted(stream->target_gain.load(std::memory_order_relaxed)),
                                  std::memory_order_release);
        return true;
    }
    if (scope == "capture") {
        if (selector != "capture") {
            error = "capture input selector must be 'capture'";
            return false;
        }
        capture_channel_.target_gain.store(adjusted(capture_channel_.target_gain.load(std::memory_order_relaxed)),
                                           std::memory_order_release);
        return true;
    }
    if (scope == "output") {
        std::lock_guard lock(nodes_mutex_);
        OutputChannel* output = resolve_output_locked(selector, error);
        if (output == nullptr) return false;
        output->target_gain.store(adjusted(output->target_gain.load(std::memory_order_relaxed)),
                                  std::memory_order_release);
        return true;
    }
    error = "fader scope must be app, stream, capture, or output";
    return false;
}

bool PipeWireEngine::set_output_gain_db(std::string_view output_selector, float decibels, std::string& error)
{
    if (!std::isfinite(decibels) || decibels < kMinimumGainDb || decibels > kMaximumGainDb) {
        error = "gain must be between -60 dB and +12 dB";
        return false;
    }
    std::lock_guard lock(nodes_mutex_);
    OutputChannel* output = resolve_output_locked(output_selector, error);
    if (output == nullptr) return false;
    output->target_gain.store(std::pow(10.0F, decibels / 20.0F), std::memory_order_release);
    return true;
}

bool PipeWireEngine::set_output_muted(std::string_view output_selector, bool muted, std::string& error)
{
    std::lock_guard lock(nodes_mutex_);
    OutputChannel* output = resolve_output_locked(output_selector, error);
    if (output == nullptr) return false;
    output->muted.store(muted, std::memory_order_release);
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

bool PipeWireEngine::save_config(std::string_view name, std::string& error)
{
    state::Show show;
    {
        std::lock_guard lock(nodes_mutex_);
        for (std::size_t i = 0; i < allocated_channels_.load(); ++i) {
            const auto& app = app_channels_[i];
            if (!app.allocated) continue;
            show.apps.push_back({app.identity, app.name, gain_db(app.target_gain.load()), app.muted.load()});
        }
        for (std::size_t i = 0; i < allocated_streams_.load(); ++i) {
            const auto& stream = playback_streams_[i];
            if (!stream.in_use) continue;
            const auto& app = app_channels_[stream.channel_index];
            show.streams.push_back({app.identity, stream.original_name, stream.name,
                                    gain_db(stream.target_gain.load()), stream.muted.load()});
        }
    }
    return state_store_.save(name, show, error);
}

bool PipeWireEngine::load_config(std::string_view name, std::string& error)
{
    state::Show show;
    if (!state_store_.load(name, show, error)) return false;
    std::lock_guard lock(nodes_mutex_);
    loaded_show_ = std::move(show);
    auto gain = [](float db) { return std::pow(10.0F, db / 20.0F); };
    for (std::size_t i = 0; i < allocated_channels_.load(); ++i) {
        auto& app = app_channels_[i];
        for (const auto& saved : loaded_show_.apps) {
            if (app.identity != saved.identity) continue;
            app.name = saved.name;
            app.target_gain.store(gain(saved.gain_db));
            app.muted.store(saved.muted);
            break;
        }
    }
    for (std::size_t i = 0; i < allocated_streams_.load(); ++i) {
        auto& stream = playback_streams_[i];
        if (!stream.in_use) continue;
        const auto& app = app_channels_[stream.channel_index];
        for (const auto& saved : loaded_show_.streams) {
            if (app.identity != saved.app_identity || stream.original_name != saved.original_name) continue;
            stream.name = saved.name;
            stream.target_gain.store(gain(saved.gain_db));
            stream.muted.store(saved.muted);
            break;
        }
    }
    return true;
}

bool PipeWireEngine::load_default_config(std::string& name, std::string& error)
{
    std::string selected;
    if (!state_store_.default_show(selected, error)) return false;
    if (!load_config(selected, error)) {
        if (selected != "default" || state_store_.has_show(selected)) return false;
        error.clear();
    }
    name = std::move(selected);
    return true;
}

std::string PipeWireEngine::list_inputs(std::size_t) const
{
    std::lock_guard lock(nodes_mutex_);
    std::ostringstream table;
    table << "Shatter inputs\n";
    std::vector<TableRow> input_rows;

    const auto channel_count = allocated_channels_.load(std::memory_order_relaxed);
    const auto stream_count = allocated_streams_.load(std::memory_order_relaxed);
    const auto send_names = [this](std::uint32_t mask) {
        std::string sends;
        bool first = true;
        for (std::size_t i = 0; i < output_channels_.size(); ++i) {
            if ((mask & (1U << i)) == 0 || !output_channels_[i].in_use) continue;
            sends += (first ? "" : ", ") + output_channels_[i].name;
            first = false;
        }
        return first ? std::string("off") : sends;
    };
    for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
        const AppChannel& channel = app_channels_[channel_index];
        bool has_stream = false;
        for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
            const PlaybackStream& stream = playback_streams_[stream_index];
            if (!stream.active.load(std::memory_order_relaxed) || stream.channel_index != channel_index) {
                continue;
            }
            input_rows.push_back({channel.id + " / stream", channel.name,
                format_gain(stream.target_gain.load()),
                stream.muted.load() ? "yes" : "no",
                stream.name + " (" + std::to_string(stream.node_id) + ")",
                send_names(stream.output_mask.load(std::memory_order_relaxed))});
            has_stream = true;
        }
        input_rows.push_back({channel.id + " / app", channel.name,
            format_gain(channel.target_gain.load()),
            channel.muted.load() ? "yes" : "no",
            has_stream ? "App channel mix" : "No active playback streams",
            send_names(channel.output_mask.load(std::memory_order_relaxed))});
    }
    if (channel_count == 0) {
        input_rows.push_back({"-", "-", "-", "-", "No app streams discovered", "-"});
    }

    std::string capture_input;
    if (capture_channel_.source_node_id == PW_ID_ANY) {
        capture_input = "No source selected";
    } else {
        const auto source = nodes_.find(capture_channel_.source_node_id);
        const std::string source_name = source == nodes_.end()
            ? "Capture source unavailable"
            : source->second.name.empty() ? source->second.description : source->second.name;
        capture_input = source_name + " (" + std::to_string(capture_channel_.source_node_id) + ", " +
            (capture_channel_.active.load(std::memory_order_relaxed) ? "connected" : "connecting") + ")";
    }
    input_rows.push_back({"capture", "Microphone", format_gain(capture_channel_.target_gain.load()),
        capture_channel_.muted.load() ? "yes" : "no", capture_input,
        send_names(capture_channel_.output_mask.load(std::memory_order_relaxed))});
    append_table(table, {"Channel / type", "Application", "Gain", "Mute", "Input / PipeWire node", "Output sends"},
        input_rows, {30, 18, 8, 5, 30, 20});

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
    std::vector<TableRow> source_rows;
    for (const auto& node : capture_sources) {
        source_rows.push_back({"source-" + std::to_string(node.node_id),
            node.name.empty() ? node.description : node.name,
            capture_channel_.source_node_id == node.node_id ? "selected" : "available"});
    }
    if (capture_sources.empty()) {
        source_rows.push_back({"-", "No PipeWire capture sources found", "-"});
    }
    table << "\nAvailable capture sources\n";
    append_table(table, {"PipeWire node ID", "Name", "Status"}, source_rows, {20, 48, 12});
    const auto unrouted_count = unrouted_playback_streams_.load(std::memory_order_relaxed);
    if (unrouted_count > 0) {
        table << "Warning: " << unrouted_count
              << " playback streams could not be routed because Shatter reached its channel or stream limit\n";
    }
    return table.str();
}

std::string PipeWireEngine::list_outputs(std::size_t) const
{
    std::lock_guard lock(nodes_mutex_);
    std::ostringstream table;
    table << "Shatter outputs\n";
    std::vector<TableRow> output_rows;
    for (const auto& output : output_channels_) {
        if (!output.in_use) continue;
        const std::string node_id = output.node_id == PW_ID_ANY ? "pending" : std::to_string(output.node_id);
        const std::string output_name = output.name + (output.node_name == kSpeakerStreamName ? " [default]" : "");
        bool has_route = false;
        for (std::size_t i = 0; i < output.assigned_sink_count; ++i) {
            const auto sink_id = output.assigned_sink_node_ids[i];
            std::string route;
            if (sink_id == PW_ID_ANY) {
                route = fit_column(output.assigned_sink_names[i], 20) + " (reconnecting)";
            } else {
                const auto target = nodes_.find(sink_id);
                const std::string& sink_name = target == nodes_.end() ? output.assigned_sink_names[i] :
                    (target->second.name.empty() ? target->second.description : target->second.name);
                route = fit_column(sink_name, 20) + " (sink-" + std::to_string(sink_id) + ")";
            }
            output_rows.push_back({output_name, node_id, output.node_name, format_gain(output.target_gain.load()),
                output.muted.load() ? "yes" : "no", route});
            has_route = true;
        }
        if (!has_route && output.node_name == kSpeakerStreamName) {
            for (std::size_t i = 0; i < output.sink_count; ++i) {
                const auto sink_id = output.sink_node_ids[i];
                const auto target = nodes_.find(sink_id);
                if (target == nodes_.end()) continue;
                const std::string& sink_name = target->second.name.empty() ? target->second.description : target->second.name;
                output_rows.push_back({output_name, node_id, output.node_name,
                    format_gain(output.target_gain.load()), output.muted.load() ? "yes" : "no",
                    fit_column(sink_name, 20) + " (sink-" + std::to_string(sink_id) + ")"});
                has_route = true;
            }
        }
        if (!has_route) output_rows.push_back({output_name, node_id, output.node_name,
            format_gain(output.target_gain.load()), output.muted.load() ? "yes" : "no", "not assigned"});
    }
    if (output_rows.empty()) output_rows.push_back({"-", "-", "-", "No output channels"});
    append_table(table, {"Output channel", "PipeWire node ID", "PipeWire node name", "Gain", "Mute", "Assigned playback sinks"},
        output_rows, {24, 16, 28, 8, 5, 40});

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
    std::vector<TableRow> sink_rows;
    for (const auto& node : playback_sinks) {
        sink_rows.push_back({"sink-" + std::to_string(node.node_id), std::to_string(node.node_id),
            node.name.empty() ? node.description : node.name});
    }
    if (playback_sinks.empty()) {
        sink_rows.push_back({"-", "-", "No PipeWire playback sinks found"});
    }
    table << "\nAvailable playback sinks\n";
    append_table(table, {"Shatter sink ID", "PipeWire node ID", "Name"}, sink_rows, {20, 18, 48});
    return table.str();
}

std::string PipeWireEngine::fader_status() const
{
    std::lock_guard lock(nodes_mutex_);
    std::vector<TableRow> rows;
    for (std::size_t i = 0; i < allocated_channels_.load(std::memory_order_relaxed); ++i) {
        const auto& channel = app_channels_[i];
        rows.push_back({channel.name + " (app)", format_fader_position(channel.target_gain.load())});
    }
    for (std::size_t i = 0; i < allocated_streams_.load(std::memory_order_relaxed); ++i) {
        const auto& stream = playback_streams_[i];
        if (!stream.in_use || !stream.active.load(std::memory_order_relaxed)) continue;
        rows.push_back({stream.name + " (stream)", format_fader_position(stream.target_gain.load())});
    }
    rows.push_back({"Capture", format_fader_position(capture_channel_.target_gain.load())});
    for (const auto& output : output_channels_) {
        if (!output.in_use) continue;
        rows.push_back({output.name + " (output)", format_fader_position(output.target_gain.load())});
    }

    std::ostringstream table;
    table << "Fader status (range: -inf to +12 dB)\n";
    append_table(table, {"Channel name", "Fader position"}, rows, {32, 70});
    return table.str();
}

bool PipeWireEngine::available() const noexcept
{
    return available_ && output_linked_.load(std::memory_order_relaxed);
}

void PipeWireEngine::begin_shutdown_fade() noexcept
{
    shutdown_fade_requested_.store(true, std::memory_order_release);
}

void PipeWireEngine::pause_streams_for_shutdown() noexcept
{
    for (auto& stream : playback_streams_) {
        if (stream.sink == nullptr) continue;
        pw_stream_set_active(stream.sink, false);
        pw_stream_flush(stream.sink, false);
    }
    if (capture_channel_.source_stream != nullptr) {
        pw_stream_set_active(capture_channel_.source_stream, false);
        pw_stream_flush(capture_channel_.source_stream, false);
    }
    for (auto& output : output_channels_) {
        if (output.stream == nullptr) continue;
        pw_stream_set_active(output.stream, false);
        pw_stream_flush(output.stream, false);
    }
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

void PipeWireEngine::on_capture_source_state_changed(void* data, pw_stream_state,
                                                       pw_stream_state state, const char*)
{
    auto& engine = *static_cast<PipeWireEngine*>(data);
    engine.capture_channel_.active.store(stream_is_ready(state), std::memory_order_release);
    if (state == PW_STREAM_STATE_ERROR) {
        engine.capture_channel_.output_mask.store(0, std::memory_order_release);
    }
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
    for (const auto& saved : loaded_show_.apps) if (saved.identity == channel.identity) {
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
        output.sink_count = 0;
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
