#include "backend/pipewire/pipewire_backend.hpp"
#include "mixer/identifiers.hpp"

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
#include <limits>
#include <sstream>
#include <set>
#include <sys/resource.h>
#include <utility>
#include <vector>

namespace shatter::backend {
namespace {

constexpr char kSpeakerStreamName[] = "shatter.speakers";
constexpr char kTargetObjectKey[] = "target.object";
constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr float kMinimumGainDb = -60.0F;
constexpr float kMaximumGainDb = 12.0F;
constexpr std::size_t kFaderTrackWidth = 25;
constexpr std::size_t kUnboundedColumn = std::numeric_limits<std::size_t>::max();

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

PipeWireBackend::PipeWireBackend()
    : processor_({app_channels_, mixer_streams_, mixer_outputs_, mixer_captures_,
                  allocated_channels_, allocated_streams_, profiler_, xruns_,
                  latest_input_ring_frames_, shutdown_fade_requested_})
{
    for (std::size_t index = 0; index < playback_streams_.size(); ++index)
        mixer_streams_[index] = &playback_streams_[index];
    for (std::size_t index = 0; index < output_channels_.size(); ++index)
        mixer_outputs_[index] = &output_channels_[index];
    for (std::size_t index = 0; index < capture_channels_.size(); ++index)
        mixer_captures_[index] = &capture_channels_[index];
}

PipeWireBackend::~PipeWireBackend()
{
    stop();
}

bool PipeWireBackend::start(std::string& error)
{
    if (available_) {
        return true;
    }

    OutputChannel& speakers = output_channels_[0];
    speakers.backend = this;
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
    registry_events_.global = &PipeWireBackend::on_global;
    registry_events_.global_remove = &PipeWireBackend::on_global_remove;
    pw_registry_add_listener(registry_, &registry_listener_, &registry_events_, this);

    if (!create_output_stream(speakers, true, error)) {
        if (error.empty()) error = "could not create the speaker output stream";
        cleanup();
        return false;
    }

    loop_thread_ = std::thread(&PipeWireBackend::run_loop, this);
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

void PipeWireBackend::stop()
{
    if (main_loop_ == nullptr && context_ == nullptr && core_ == nullptr) {
        return;
    }
    cleanup();
}

bool PipeWireBackend::set_app_gain_db(std::string_view channel_id, float decibels, std::string& error)
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

bool PipeWireBackend::set_stream_gain_db(std::string_view stream_selector, float decibels, std::string& error)
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

bool PipeWireBackend::set_app_muted(std::string_view channel_id, bool muted, std::string& error)
{
    std::lock_guard lock(nodes_mutex_);
    AppChannel* channel = resolve_channel_locked(channel_id, error);
    if (channel == nullptr) {
        return false;
    }
    channel->muted.store(muted, std::memory_order_relaxed);
    return true;
}

bool PipeWireBackend::set_stream_muted(std::string_view stream_selector, bool muted, std::string& error)
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

bool PipeWireBackend::rename_app(std::string_view current_name, std::string new_name, std::string& error)
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

bool PipeWireBackend::rename_stream(std::string_view current_name, std::string new_name, std::string& error)
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

bool PipeWireBackend::set_capture_source(std::string_view source_selector, std::string& error)
{
    std::lock_guard lock(nodes_mutex_);
    if (resolve_capture_channel_locked(source_selector, error) == nullptr) return false;
    error = "capture sources are already available as input channels; assign the channel directly";
    return false;
}

bool PipeWireBackend::set_capture_gain_db(float decibels, std::string& error)
{
    if (!std::isfinite(decibels) || decibels < kMinimumGainDb || decibels > kMaximumGainDb) {
        error = "gain must be between -60 dB and +12 dB";
        return false;
    }
    for (auto& channel : capture_channels_)
        if (channel.in_use) channel.target_gain.store(std::pow(10.0F, decibels / 20.0F), std::memory_order_relaxed);
    return true;
}

bool PipeWireBackend::set_capture_muted(std::string_view selector, bool muted, std::string& error)
{
    std::lock_guard lock(nodes_mutex_);
    CaptureChannel* channel = resolve_capture_channel_locked(selector, error);
    if (channel == nullptr) return false;
    channel->muted.store(muted, std::memory_order_release);
    return true;
}

bool PipeWireBackend::adjust_fader_db(std::string_view scope, std::string_view selector, float delta_db,
                                     bool minimum, std::string& error)
{
    return update_fader_db(scope, selector, delta_db,
                           minimum ? FaderUpdateMode::minimum : FaderUpdateMode::relative, error);
}

bool PipeWireBackend::set_fader_db(std::string_view scope, std::string_view selector, float decibels,
                                  bool minimum, std::string& error)
{
    return update_fader_db(scope, selector, decibels,
                           minimum ? FaderUpdateMode::minimum : FaderUpdateMode::absolute, error);
}

bool PipeWireBackend::update_fader_db(std::string_view scope, std::string_view selector, float decibels,
                                     FaderUpdateMode mode, std::string& error)
{
    if (mode != FaderUpdateMode::minimum && !std::isfinite(decibels)) {
        error = mode == FaderUpdateMode::absolute
            ? "fader level must be a finite number of dB or -inf"
            : "fader increment must be a finite number of dB";
        return false;
    }
    const auto adjusted = [decibels, mode](float current_gain) {
        if (mode == FaderUpdateMode::minimum) return 0.0F;
        const float target_db = mode == FaderUpdateMode::absolute
            ? std::clamp(decibels, kMinimumGainDb, kMaximumGainDb)
            : std::clamp(gain_db(current_gain) + decibels, kMinimumGainDb, kMaximumGainDb);
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
        std::lock_guard lock(nodes_mutex_);
        CaptureChannel* channel = resolve_capture_channel_locked(selector, error);
        if (channel == nullptr) return false;
        channel->target_gain.store(adjusted(channel->target_gain.load(std::memory_order_relaxed)),
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

bool PipeWireBackend::set_output_gain_db(std::string_view output_selector, float decibels, std::string& error)
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

bool PipeWireBackend::set_output_muted(std::string_view output_selector, bool muted, std::string& error)
{
    std::lock_guard lock(nodes_mutex_);
    OutputChannel* output = resolve_output_locked(output_selector, error);
    if (output == nullptr) return false;
    output->muted.store(muted, std::memory_order_release);
    return true;
}

bool PipeWireBackend::set_capture_speaker_send(bool enabled, std::string& error)
{
    (void)error;
    for (auto& channel : capture_channels_) {
        if (!channel.in_use) continue;
        auto mask = channel.output_mask.load(std::memory_order_relaxed);
        if (enabled) mask |= 1U;
        else mask &= ~1U;
        channel.output_mask.store(mask, std::memory_order_release);
    }
    return true;
}

bool PipeWireBackend::save_config(std::string_view name, std::string& error)
{
    state::Show show;
    {
        std::lock_guard lock(nodes_mutex_);
        show = loaded_show_;
        show.outputs.clear();
        const auto identities_for_mask = [this](std::uint32_t mask) {
            std::vector<std::string> identities;
            for (std::size_t i = 0; i < output_channels_.size(); ++i) {
                if ((mask & (1U << i)) != 0 && output_channels_[i].in_use)
                    identities.push_back(output_channels_[i].node_name);
            }
            return identities;
        };
        for (std::size_t i = 0; i < allocated_channels_.load(); ++i) {
            const auto& app = app_channels_[i];
            if (!app.allocated) continue;
            auto found = std::find_if(show.apps.begin(), show.apps.end(), [&](const auto& item) {
                return item.identity == app.identity;
            });
            state::AppSetting setting;
            setting.identity = app.identity;
            setting.name = app.name;
            setting.gain_db = gain_db(app.target_gain.load());
            setting.muted = app.muted.load();
            setting.output_identities = identities_for_mask(app.output_mask.load(std::memory_order_relaxed));
            setting.has_output_sends = true;
            if (found == show.apps.end()) show.apps.push_back(std::move(setting));
            else *found = std::move(setting);
        }
        for (std::size_t i = 0; i < allocated_streams_.load(); ++i) {
            const auto& stream = playback_streams_[i];
            if (!stream.in_use) continue;
            const auto& app = app_channels_[stream.channel_index];
            auto found = std::find_if(show.streams.begin(), show.streams.end(), [&](const auto& item) {
                return item.app_identity == app.identity && item.original_name == stream.original_name;
            });
            state::StreamSetting setting;
            setting.app_identity = app.identity;
            setting.original_name = stream.original_name;
            setting.name = stream.name;
            setting.gain_db = gain_db(stream.target_gain.load());
            setting.muted = stream.muted.load();
            setting.output_identities = identities_for_mask(stream.output_mask.load(std::memory_order_relaxed));
            setting.has_output_sends = true;
            if (found == show.streams.end()) show.streams.push_back(std::move(setting));
            else *found = std::move(setting);
        }
        for (const auto& [key, saved_state] : playback_route_state_) {
            const auto separator = key.find('\x1f');
            if (separator == std::string::npos) continue;
            const std::string app_identity = key.substr(0, separator);
            const std::string original_name = key.substr(separator + 1);
            const bool currently_active = std::any_of(
                playback_streams_.begin(), playback_streams_.begin() + allocated_streams_.load(),
                [&](const auto& stream) {
                    return stream.in_use &&
                        app_channels_[stream.channel_index].identity == app_identity &&
                        stream.original_name == original_name;
                });
            if (currently_active) continue;
            auto found = std::find_if(show.streams.begin(), show.streams.end(), [&](const auto& item) {
                return item.app_identity == app_identity && item.original_name == original_name;
            });
            if (found == show.streams.end()) {
                state::StreamSetting setting;
                setting.app_identity = app_identity;
                setting.original_name = original_name;
                setting.name = saved_state.name;
                setting.gain_db = gain_db(saved_state.gain);
                setting.muted = saved_state.muted;
                setting.output_identities = identities_for_mask(saved_state.output_mask);
                setting.has_output_sends = true;
                show.streams.push_back(std::move(setting));
            } else {
                found->name = saved_state.name;
                found->gain_db = gain_db(saved_state.gain);
                found->muted = saved_state.muted;
                found->output_identities = identities_for_mask(saved_state.output_mask);
                found->has_output_sends = true;
            }
        }
        show.capture_output_identities.clear();
        show.capture_source_identity.clear();
        show.captures.clear();
        bool has_legacy_capture_state = false;
        for (const auto& channel : capture_channels_) {
            if (!channel.in_use) continue;
            if (!has_legacy_capture_state) {
                show.capture_output_identities = identities_for_mask(channel.output_mask.load());
                show.capture_gain_db = gain_db(channel.target_gain.load());
                show.capture_muted = channel.muted.load();
                has_legacy_capture_state = true;
            }
            show.captures.push_back({channel.identity, channel.name, gain_db(channel.target_gain.load()),
                channel.muted.load(), identities_for_mask(channel.output_mask.load())});
        }
        show.has_capture_sends = true;
        for (std::size_t i = 1; i < output_channels_.size(); ++i) {
            const auto& output = output_channels_[i];
            if (!output.in_use) continue;
            state::OutputSetting setting;
            setting.slot = i;
            setting.identity = output.node_name;
            setting.name = output.name;
            setting.capture_source = output.capture_source;
            setting.gain_db = gain_db(output.target_gain.load(std::memory_order_relaxed));
            setting.muted = output.muted.load(std::memory_order_relaxed);
            for (std::size_t sink = 0; sink < output.assigned_sink_count; ++sink) {
                if (output.assigned_sink_names[sink].empty()) continue;
                const auto& identity = output.assigned_sink_identities[sink];
                setting.devices.push_back({identity.starts_with("app-input") ? identity :
                    mixer::stable_device_identity(identity, output.assigned_sink_names[sink]),
                    output.assigned_sink_names[sink]});
            }
            show.outputs.push_back(std::move(setting));
        }
        std::set<std::string> valid_output_ids{"shatter.speakers"};
        for (const auto& output : show.outputs) valid_output_ids.insert(output.identity);
        const auto prune_sends = [&valid_output_ids](auto& item) {
            std::erase_if(item.output_identities, [&](const auto& identity) {
                return !valid_output_ids.contains(identity);
            });
        };
        for (auto& app : show.apps) prune_sends(app);
        for (auto& stream : show.streams) prune_sends(stream);
        for (auto& capture : show.captures) std::erase_if(capture.output_identities,
            [&](const auto& identity) { return !valid_output_ids.contains(identity); });
        for (auto& app : show.apps) {
            if (!app.has_output_sends) {
                app.output_identities = {"shatter.speakers"};
                app.has_output_sends = true;
            }
        }
        for (auto& stream : show.streams) stream.has_output_sends = true;
        show.has_capture_sends = true;
        std::erase_if(show.capture_output_identities, [&](const auto& identity) {
            return !valid_output_ids.contains(identity);
        });
    }
    return state_store_.save(name, show, error);
}

bool PipeWireBackend::load_config(std::string_view name, std::string& error)
{
    state::Show show;
    if (!state_store_.load(name, show, error)) return false;
    OutputRequest request{};
    request.backend = this;
    request.action = OutputRequest::Action::load_config;
    request.first = std::string(name);
    request.show = std::move(show);
    return invoke_output_request(request, error);
}

std::uint32_t PipeWireBackend::output_mask_for_identities(const std::vector<std::string>& identities) const
{
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < output_channels_.size(); ++i) {
        if (!output_channels_[i].in_use) continue;
        if (std::find(identities.begin(), identities.end(), output_channels_[i].node_name) != identities.end())
            mask |= 1U << i;
    }
    return mask;
}

bool PipeWireBackend::load_default_config(std::string& name, std::string& error)
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

std::string PipeWireBackend::list_inputs(std::size_t) const
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
                stream.selector_id + " (" + stream.name + ")",
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

    std::vector<const CaptureChannel*> capture_channels;
    for (const auto& channel : capture_channels_)
        if (channel.in_use) capture_channels.push_back(&channel);
    std::sort(capture_channels.begin(), capture_channels.end(), [](const auto* left, const auto* right) {
        return left->name < right->name;
    });
    for (const auto* channel : capture_channels) {
        const auto status = channel->source_node_id == PW_ID_ANY ? "disconnected" :
            channel->active.load(std::memory_order_relaxed) ? "connected" : "connecting";
        input_rows.push_back({"capture / " + channel->selector_id, channel->name,
            format_gain(channel->target_gain.load()), channel->muted.load() ? "yes" : "no",
            channel->name + " (" + status + ")", send_names(channel->output_mask.load())});
    }
    append_table(table, {"Input channel / type", "Application", "Gain", "Mute", "Playback stream or capture device", "Output sends"},
        input_rows, {kUnboundedColumn, kUnboundedColumn, 8, 5, kUnboundedColumn, kUnboundedColumn});

    const auto unrouted_count = unrouted_playback_streams_.load(std::memory_order_relaxed);
    if (unrouted_count > 0) {
        table << "Warning: " << unrouted_count
              << " playback streams could not be routed because Shatter reached its channel or stream limit\n";
    }
    return table.str();
}

std::string PipeWireBackend::list_outputs(std::size_t) const
{
    std::lock_guard lock(nodes_mutex_);
    std::ostringstream table;
    table << "Shatter outputs\n";
    std::vector<TableRow> output_rows;
    for (const auto& output : output_channels_) {
        if (!output.in_use) continue;
        const std::string output_identity = output.node_name;
        const std::string output_name = output.name + (output.node_name == kSpeakerStreamName ? " [default]" : "");
        const std::string output_type = output.capture_source ? "capture" : "playback";
        bool has_route = false;
        for (std::size_t i = 0; i < output.assigned_sink_count; ++i) {
            const auto sink_id = output.assigned_sink_node_ids[i];
            std::string route;
            if (sink_id == PW_ID_ANY) {
                route = output.assigned_sink_names[i] + " (reconnecting)";
            } else {
                const auto target = nodes_.find(sink_id);
                if (target == nodes_.end()) {
                    const bool app_input = output.assigned_sink_identities[i].starts_with("app-input");
                    route = output.assigned_sink_names[i] +
                        (app_input ? " [input] (reconnecting)" : " (reconnecting)");
                } else {
                    const std::string sink_name = output_destination_name(target->second);
                    route = output_destination_selector(target->second) + " (" + fit_column(sink_name, 12) + ")";
                    if (target->second.media_class == "Stream/Input/Audio") route += " [input]";
                }
            }
            output_rows.push_back({output_name, output_identity, output_type, format_gain(output.target_gain.load()),
                output.muted.load() ? "yes" : "no", route});
            has_route = true;
        }
        if (!has_route && output.node_name == kSpeakerStreamName) {
            for (std::size_t i = 0; i < output.sink_count; ++i) {
                const auto sink_id = output.sink_node_ids[i];
                const auto target = nodes_.find(sink_id);
                if (target == nodes_.end()) continue;
                const std::string sink_name = output_destination_name(target->second);
                std::string destination = output_destination_selector(target->second) +
                    " (" + fit_column(sink_name, 12) + ")";
                if (target->second.media_class == "Stream/Input/Audio") destination += " [input]";
                output_rows.push_back({output_name, output_identity, output_type,
                    format_gain(output.target_gain.load()), output.muted.load() ? "yes" : "no",
                    std::move(destination)});
                has_route = true;
            }
        }
        if (!has_route) output_rows.push_back({output_name, output_identity, output_type,
            format_gain(output.target_gain.load()), output.muted.load() ? "yes" : "no",
            output.capture_source ? "select as microphone in app settings" : "not assigned"});
    }
    if (output_rows.empty()) output_rows.push_back({"-", "-", "-", "-", "-", "No output channels"});
    append_table(table, {"Output channel", "Output identity", "Type", "Gain", "Mute", "Assigned destinations"},
        output_rows, {kUnboundedColumn, kUnboundedColumn, 9, 8, 5, kUnboundedColumn});

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
        const std::string name = output_destination_name(node);
        sink_rows.push_back({mixer::make_stable_selector_id("device", mixer::stable_device_identity(node.device_identity, name)), name});
    }
    if (playback_sinks.empty()) {
        sink_rows.push_back({"-", "No playback devices found"});
    }
    table << "\nAvailable playback devices\n";
    append_table(table, {"Playback device ID", "Name"}, sink_rows, {kUnboundedColumn, kUnboundedColumn});

    std::vector<NodeInfo> application_inputs;
    for (const auto& [id, node] : nodes_) {
        (void)id;
        if (node.media_class == "Stream/Input/Audio") application_inputs.push_back(node);
    }
    std::sort(application_inputs.begin(), application_inputs.end(), [](const NodeInfo& left, const NodeInfo& right) {
        if (left.application != right.application) return left.application < right.application;
        return left.name < right.name;
    });
    std::vector<TableRow> input_rows;
    for (const auto& node : application_inputs) {
        const std::string name = output_destination_name(node);
        const std::string application = node.application.empty() ? node.application_id : node.application;
        input_rows.push_back({output_destination_selector(node),
            (application.empty() ? std::string{} : application + " / ") + name});
    }
    if (input_rows.empty()) input_rows.push_back({"-", "No application input captures found"});
    table << "\nAvailable application input captures\n";
    append_table(table, {"Application input ID", "Application / capture"}, input_rows,
        {kUnboundedColumn, kUnboundedColumn});
    return table.str();
}

std::string PipeWireBackend::list_meters(std::string_view selector) const
{
    std::lock_guard lock(nodes_mutex_);
    struct MeterRow { std::string scope, selector, peak, rms; };
    std::vector<MeterRow> rows;
    const auto matches = [selector](std::initializer_list<std::string_view> names) {
        return selector.empty() || std::find(names.begin(), names.end(), selector) != names.end();
    };
    const auto append = [&rows](std::string scope, std::string name, meter::Reading reading,
                                float gain = 1.0F) {
        const auto format_dbfs = [](float linear) {
            std::ostringstream value;
            value << std::fixed << std::setprecision(1) << meter::to_dbfs(linear);
            return value.str();
        };
        rows.push_back({std::move(scope), std::move(name), format_dbfs(reading.peak * gain),
                        format_dbfs(reading.rms * gain)});
    };

    const auto channel_count = allocated_channels_.load(std::memory_order_relaxed);
    const auto stream_count = allocated_streams_.load(std::memory_order_relaxed);
    for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
        const auto& channel = app_channels_[channel_index];
        if (!matches({channel.id, channel.identity, channel.name})) continue;
        const auto output_mask = channel.output_mask.load(std::memory_order_relaxed);
        std::size_t meter_output = max_output_channels;
        for (std::size_t index = 0; index < max_output_channels; ++index) {
            if ((output_mask & (1U << index)) != 0) { meter_output = index; break; }
        }
        if (meter_output < max_output_channels) {
            append("app", channel.id + " (" + channel.name + ")", channel.meters[meter_output].reading());
        } else {
            float peak = 0.0F;
            double square_sum = 0.0;
            for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
                const auto& stream = playback_streams_[stream_index];
                if (!stream.active.load(std::memory_order_relaxed) || stream.channel_index != channel_index) continue;
                const auto reading = stream.meter.reading();
                peak = std::max(peak, reading.peak);
                square_sum += static_cast<double>(reading.rms) * reading.rms;
            }
            append("app", channel.id + " (" + channel.name + ")",
                {peak, static_cast<float>(std::sqrt(square_sum))});
        }
    }
    for (std::size_t stream_index = 0; stream_index < stream_count; ++stream_index) {
        const auto& stream = playback_streams_[stream_index];
        if (!stream.active.load(std::memory_order_relaxed) || !stream.in_use ||
            !matches({stream.selector_id, stream.name})) continue;
        append("stream", stream.selector_id + " (" + stream.name + ")", stream.meter.reading());
    }
    for (const auto& channel : capture_channels_) {
        if (!channel.in_use || !matches({"capture", channel.name, channel.selector_id})) continue;
        append("capture", channel.name + " (" + channel.selector_id + ")", channel.meter.reading());
    }
    for (const auto& output : output_channels_) {
        if (!output.in_use || !matches({output.name, output.node_name})) continue;
        append("output", output.name + " (" + output.node_name + ")", output.meter.reading());
    }

    std::ostringstream result;
    result << "Live meters (peak and RMS in dBFS; 100 ms windows; app readings use its first assigned output mix; stream, capture, and output readings are post-fader)\n"
           << "Scope | Channel | Peak dBFS | RMS dBFS\n";
    for (const auto& row : rows)
        result << row.scope << " | " << row.selector << " | " << row.peak << " | " << row.rms << '\n';
    if (rows.empty()) result << (selector.empty() ? "No active channels\n" : "No meter matches '" + std::string(selector) + "'\n");
    return result.str();
}

std::string PipeWireBackend::fader_status() const
{
    std::lock_guard lock(nodes_mutex_);
    std::vector<TableRow> app_rows;
    for (std::size_t i = 0; i < allocated_channels_.load(std::memory_order_relaxed); ++i) {
        const auto& channel = app_channels_[i];
        app_rows.push_back({channel.name, format_fader_position(channel.target_gain.load())});
    }
    std::vector<TableRow> stream_rows;
    for (std::size_t i = 0; i < allocated_streams_.load(std::memory_order_relaxed); ++i) {
        const auto& stream = playback_streams_[i];
        if (!stream.in_use || !stream.active.load(std::memory_order_relaxed)) continue;
        stream_rows.push_back({stream.name, format_fader_position(stream.target_gain.load())});
    }
    std::vector<TableRow> capture_rows;
    for (const auto& channel : capture_channels_)
        if (channel.in_use) capture_rows.push_back({channel.name,
            format_fader_position(channel.target_gain.load())});
    std::vector<TableRow> output_rows;
    for (const auto& output : output_channels_) {
        if (!output.in_use) continue;
        output_rows.push_back({output.name, format_fader_position(output.target_gain.load())});
    }

    std::ostringstream table;
    table << "Fader status (range: -inf to +12 dB)\n";
    const auto append_group = [&table](std::string_view title, std::vector<TableRow> rows,
                                       std::string_view empty_message) {
        if (rows.empty()) rows.push_back({std::string(empty_message), ""});
        table << '\n' << title << '\n';
        append_table(table, {"Channel name", "Fader position"}, rows, {kUnboundedColumn, 70});
    };
    append_group("Apps", std::move(app_rows), "No app channels");
    append_group("Streams", std::move(stream_rows), "No active playback streams");
    append_group("Capture", std::move(capture_rows), "No capture channel");
    append_group("Outputs", std::move(output_rows), "No output channels");
    return table.str();
}

bool PipeWireBackend::available() const noexcept
{
    return available_ && output_linked_.load(std::memory_order_relaxed);
}

std::string PipeWireBackend::health_status() const
{
    const auto unrouted = unrouted_playback_streams_.load(std::memory_order_relaxed);
    const auto connected = output_linked_.load(std::memory_order_relaxed);
    const auto state = mixer::health_state(available_, connected, unrouted);
    const auto xrun = xruns_.snapshot();
    std::uint64_t ring_frames = 0;
    for (const auto& occupancy : latest_input_ring_frames_)
        ring_frames = std::max(ring_frames, occupancy.load(std::memory_order_relaxed));
    std::ostringstream report;
    report << "Engine health: " << mixer::health_state_name(state) << '\n';
    if (state == mixer::HealthState::failed_open) {
        report << "Reason: PipeWire engine did not open successfully\n";
    } else if (state == mixer::HealthState::degraded) {
        if (!connected) report << "Issue: speaker output is disconnected\n";
        if (unrouted != 0) report << "Issue: " << unrouted << " playback stream(s) are unrouted\n";
    } else {
        report << "Issue: none detected\n";
    }
    report << "Xruns since engine start (Shatter ring buffers): input overruns "
           << xrun.input_overrun_events << " events / " << xrun.input_overrun_frames << " frames; "
           << "playback underruns " << xrun.playback_underrun_events << " events / "
           << xrun.playback_underrun_frames << " frames\n";
    if (available_) {
        report << std::fixed << std::setprecision(3)
               << "I/O buffering estimate: " << ring_frames * 1000.0 / kSampleRate
               << " ms (latest Shatter playback input ring occupancy, at 48 kHz; excludes device latency)\n";
    } else {
        report << "I/O buffering estimate: unavailable (PipeWire engine is not open)\n";
    }
    return report.str();
}

void PipeWireBackend::begin_shutdown_fade() noexcept
{
    shutdown_fade_requested_.store(true, std::memory_order_release);
}

void PipeWireBackend::pause_streams_for_shutdown() noexcept
{
    for (auto& stream : playback_streams_) {
        if (stream.sink == nullptr) continue;
        pw_stream_set_active(stream.sink, false);
        pw_stream_flush(stream.sink, false);
    }
    for (auto& channel : capture_channels_) {
        if (channel.source_stream == nullptr) continue;
        pw_stream_set_active(channel.source_stream, false);
        pw_stream_flush(channel.source_stream, false);
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

bool PipeWireBackend::profile_start(std::string& error)
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

std::string PipeWireBackend::profile_stop(std::string& error)
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

void PipeWireBackend::on_output_state_changed(void* data, pw_stream_state, pw_stream_state state,
                                             const char* error)
{
    auto& output = *static_cast<OutputChannel*>(data);
    auto& backend = *output.backend;
    output.active.store(stream_is_ready(state), std::memory_order_release);
    if (stream_is_ready(state) && output.stream != nullptr) {
        output.node_id = pw_stream_get_node_id(output.stream);
        if (&output == &backend.output_channels_[0]) {
            backend.speaker_node_id_.store(output.node_id, std::memory_order_relaxed);
        }
        std::lock_guard lock(backend.nodes_mutex_);
        backend.restore_output_routes_locked(output);
        backend.refresh_output_link_locked();
    }
    {
        std::lock_guard lock(backend.state_mutex_);
        if (&output == &backend.output_channels_[0]) backend.output_ready_ = stream_is_ready(state);
        if (state == PW_STREAM_STATE_ERROR && &output == &backend.output_channels_[0]) {
            backend.startup_error_ = error == nullptr ? "PipeWire speaker stream failed" : error;
        }
    }
    backend.state_changed_.notify_all();
}

void PipeWireBackend::on_stream_input_state_changed(void* data, pw_stream_state,
                                                   pw_stream_state state, const char* error)
{
    auto& stream = *static_cast<PlaybackStream*>(data);
    stream.backend->update_playback_stream_state(stream, state, error);
}

void PipeWireBackend::on_capture_source_state_changed(void* data, pw_stream_state,
                                                       pw_stream_state state, const char*)
{
    auto& channel = *static_cast<CaptureChannel*>(data);
    channel.active.store(stream_is_ready(state), std::memory_order_release);
    if (state == PW_STREAM_STATE_ERROR) {
        channel.output_mask.store(0, std::memory_order_release);
    }
}

bool PipeWireBackend::add_capture_channel(const NodeInfo& source, std::string& error)
{
    const std::string name = capture_source_name(source);
    const std::string identity = mixer::stable_device_identity(source.device_identity, name);
    CaptureChannel* channel = nullptr;
    std::size_t index = 0;
    bool new_channel = false;
    {
        std::lock_guard lock(nodes_mutex_);
        for (auto& existing : capture_channels_) {
            if (!existing.in_use || existing.identity != identity) continue;
            if (existing.source_stream != nullptr) return true;
            channel = &existing;
            index = static_cast<std::size_t>(&existing - capture_channels_.data());
            break;
        }
        if (channel == nullptr) {
            for (; index < capture_channels_.size(); ++index) {
                if (!capture_channels_[index].in_use) { channel = &capture_channels_[index]; break; }
            }
        }
        if (channel == nullptr) {
            error = "capture channel limit reached";
            return false;
        }
        if (!channel->in_use) {
            new_channel = true;
            channel->backend = this;
            channel->in_use = true;
            channel->name = name;
            channel->identity = identity;
            channel->selector_id = mixer::make_stable_selector_id("source", identity);
        }
        channel->source_node_id = source.node_id;
        if (new_channel) {
            const auto saved = std::find_if(loaded_show_.captures.begin(), loaded_show_.captures.end(),
                [&](const auto& item) { return item.identity == identity; });
            if (saved != loaded_show_.captures.end()) {
                channel->output_mask.store(output_mask_for_identities(saved->output_identities));
                channel->target_gain.store(std::pow(10.0F, saved->gain_db / 20.0F));
                channel->muted.store(saved->muted);
            } else {
                if (loaded_show_.has_capture_sends)
                    channel->output_mask.store(output_mask_for_identities(loaded_show_.capture_output_identities));
                channel->target_gain.store(std::pow(10.0F, loaded_show_.capture_gain_db / 20.0F));
                channel->muted.store(loaded_show_.capture_muted);
            }
        }
        channel->smoothed_gain = channel->target_gain.load(std::memory_order_relaxed);
        channel->meter.reset();
    }

    const std::string stream_name = "Shatter microphone input: " + name;
    const std::string node_name = "shatter.capture.input." + std::to_string(index + 1);
    pw_properties* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
        PW_KEY_NODE_NAME, node_name.c_str(), PW_KEY_APP_NAME, "Shatter",
        PW_KEY_TARGET_OBJECT, source.name.c_str(), nullptr);
    if (properties == nullptr) {
        error = "could not allocate capture stream properties";
        destroy_capture_channel(*channel);
        return false;
    }
    channel->events = {};
    channel->events.version = PW_VERSION_STREAM_EVENTS;
    channel->events.state_changed = &PipeWireBackend::on_capture_source_state_changed;
    channel->events.process = &PipeWireBackend::on_capture_source_process;
    channel->source_stream = pw_stream_new(core_, stream_name.c_str(), properties);
    if (channel->source_stream == nullptr) {
        error = "could not create PipeWire capture stream";
        destroy_capture_channel(*channel);
        return false;
    }
    pw_stream_add_listener(channel->source_stream, &channel->listener, &channel->events, channel);
    std::array<std::byte, 1024> storage{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    const spa_pod* params[] = {build_stereo_format(&builder)};
    if (pw_stream_connect(channel->source_stream, PW_DIRECTION_INPUT, source.node_id,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                         PW_STREAM_FLAG_RT_PROCESS), params, 1) < 0) {
        error = "could not connect to PipeWire capture source: " + name;
        destroy_capture_channel(*channel);
        return false;
    }
    return true;
}

void PipeWireBackend::destroy_capture_channel(CaptureChannel& channel, bool forget)
{
    channel.active.store(false, std::memory_order_release);
    channel.process_gate.close();
    channel.mixer_gate.close();
    if (channel.source_stream != nullptr) {
        pw_stream_disconnect(channel.source_stream);
        channel.process_gate.wait_for_quiescence();
        pw_stream_destroy(channel.source_stream);
        channel.source_stream = nullptr;
    }
    channel.process_gate.wait_for_quiescence();
    channel.mixer_gate.wait_for_quiescence();
    for (auto& ring : channel.audio) ring.discard_all();
    {
        std::lock_guard lock(nodes_mutex_);
        channel.source_node_id = PW_ID_ANY;
        channel.meter.reset();
        if (forget) {
            channel.output_mask.store(0, std::memory_order_release);
            channel.name.clear();
            channel.identity.clear();
            channel.selector_id.clear();
            channel.in_use = false;
        }
    }
    channel.process_gate.reopen();
    channel.mixer_gate.reopen();
}

void PipeWireBackend::update_playback_stream_state(PlaybackStream& stream, pw_stream_state state,
                                                  const char* error)
{
    if (!stream.in_use || stream.node_id == PW_ID_ANY) {
        stream.active.store(false, std::memory_order_release);
        return;
    }
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

void PipeWireBackend::assign_stream_target(PlaybackStream& stream)
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

std::size_t PipeWireBackend::get_or_create_channel_locked(const NodeInfo& node)
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
        : !node.process_binary.empty() ? node.process_binary
        : !node.name.empty() ? node.name
        : !node.description.empty() ? node.description : "Unknown application";
    channel.smoothed_gain.fill(1.0F);
    for (const auto& saved : loaded_show_.apps) if (saved.identity == channel.identity) {
        channel.name = saved.name;
        channel.target_gain.store(std::pow(10.0F, saved.gain_db / 20.0F));
        channel.muted.store(saved.muted);
        if (saved.has_output_sends)
            channel.output_mask.store(output_mask_for_identities(saved.output_identities));
        break;
    }
    channel_by_identity_[channel.identity] = index;
    allocated_channels_.store(current_channel_count + 1, std::memory_order_release);
    return index;
}

PipeWireBackend::PlaybackStream* PipeWireBackend::find_stream_locked(std::uint32_t node_id)
{
    const auto found = stream_by_node_.find(node_id);
    return found == stream_by_node_.end() ? nullptr : &playback_streams_[found->second];
}

PipeWireBackend::AppChannel* PipeWireBackend::find_channel_locked(std::string_view channel_id)
{
    for (std::size_t index = 0; index < allocated_channels_.load(std::memory_order_relaxed); ++index) {
        if (app_channels_[index].id == channel_id) {
            return &app_channels_[index];
        }
    }
    return nullptr;
}

PipeWireBackend::AppChannel* PipeWireBackend::resolve_channel_locked(std::string_view selector,
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

PipeWireBackend::PlaybackStream* PipeWireBackend::resolve_stream_locked(std::string_view selector,
                                                                      std::string& error)
{
    PlaybackStream* match = nullptr;
    for (std::size_t index = 0; index < allocated_streams_.load(std::memory_order_relaxed); ++index) {
        PlaybackStream& stream = playback_streams_[index];
        if (!stream.in_use) continue;
        if (stream.selector_id != selector && stream.name != selector) continue;
        if (match != nullptr) {
            error = "playback stream selector is ambiguous; use its listed stream ID";
            return nullptr;
        }
        match = &stream;
    }
    if (match == nullptr) error = "unknown playback stream: " + std::string(selector);
    return match;
}

std::string PipeWireBackend::playback_route_key(std::string_view app_identity,
                                               std::string_view original_name) const
{
    return std::string(app_identity) + '\x1f' + std::string(original_name);
}

bool PipeWireBackend::sink_binding_matches(const OutputChannel& output, std::size_t index,
                                          const NodeInfo& sink) const
{
    const std::string& identity = output.assigned_sink_identities[index];
    const std::string name = output_destination_name(sink);
    const std::string stable_identity = output_destination_identity(sink);
    return (!identity.empty() && (identity == sink.device_identity || identity == stable_identity)) ||
        (identity.empty() && output.assigned_sink_names[index] == name);
}

void PipeWireBackend::run_loop()
{
    pw_main_loop_run(main_loop_);
}

void PipeWireBackend::cleanup()
{
    available_ = false;
    if (main_loop_ != nullptr) {
        pw_main_loop_quit(main_loop_);
    }
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
    for (auto& channel : capture_channels_)
        if (channel.in_use) destroy_capture_channel(channel);
    for (auto& stream : playback_streams_) {
        stream.active.store(false, std::memory_order_release);
        stream.process_gate.close();
        stream.mixer_gate.close();
        if (stream.sink != nullptr) {
            pw_stream_disconnect(stream.sink);
            stream.process_gate.wait_for_quiescence();
            pw_stream_destroy(stream.sink);
            stream.sink = nullptr;
        }
        stream.process_gate.wait_for_quiescence();
        stream.mixer_gate.wait_for_quiescence();
        for (auto& ring : stream.audio) ring.discard_all();
        for (auto& ring : stream.send_audio) ring.discard_all();
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

} // namespace shatter::backend
