#include "mixer/channel_controls.hpp"
#include "mixer/identifiers.hpp"
#include "mixer/presentation.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ranges>
#include <utility>

namespace shatter::mixer {
using namespace presentation;
namespace {
template<class T>
auto records(std::span<T* const> slots)
{
    return slots | std::views::transform([](T* record) -> T& { return *record; });
}
} // namespace

bool ChannelControls::set_app_gain_db(std::string_view channel_id, float decibels, std::string& error)
{
    if (!std::isfinite(decibels) || decibels < kMinimumGainDb || decibels > kMaximumGainDb) {
        error = "gain must be between -60 dB and +12 dB";
        return false;
    }
    std::lock_guard lock(mutex_);
    AppChannel* channel = resolve_channel_locked(channel_id, error);
    if (channel == nullptr) {
        return false;
    }
    channel->target_gain.store(std::pow(10.0F, decibels / 20.0F), std::memory_order_relaxed);
    return true;
}

bool ChannelControls::set_stream_gain_db(std::string_view stream_selector, float decibels, std::string& error)
{
    if (!std::isfinite(decibels) || decibels < kMinimumGainDb || decibels > kMaximumGainDb) {
        error = "gain must be between -60 dB and +12 dB";
        return false;
    }
    std::lock_guard lock(mutex_);
    PlaybackStream* stream = resolve_stream_locked(stream_selector, error);
    if (stream == nullptr || !stream->active.load(std::memory_order_relaxed)) {
        if (error.empty()) error = "unknown or inactive playback stream: " + std::string(stream_selector);
        return false;
    }
    stream->target_gain.store(std::pow(10.0F, decibels / 20.0F), std::memory_order_relaxed);
    return true;
}

bool ChannelControls::set_app_muted(std::string_view channel_id, bool muted, std::string& error)
{
    std::lock_guard lock(mutex_);
    AppChannel* channel = resolve_channel_locked(channel_id, error);
    if (channel == nullptr) {
        return false;
    }
    channel->muted.store(muted, std::memory_order_relaxed);
    return true;
}

bool ChannelControls::set_stream_muted(std::string_view stream_selector, bool muted, std::string& error)
{
    std::lock_guard lock(mutex_);
    PlaybackStream* stream = resolve_stream_locked(stream_selector, error);
    if (stream == nullptr || !stream->active.load(std::memory_order_relaxed)) {
        if (error.empty()) error = "unknown or inactive playback stream: " + std::string(stream_selector);
        return false;
    }
    stream->muted.store(muted, std::memory_order_relaxed);
    return true;
}

bool ChannelControls::rename_app(std::string_view current_name, std::string new_name, std::string& error)
{
    if (new_name.empty()) {
        error = "new app name cannot be empty";
        return false;
    }
    std::lock_guard lock(mutex_);
    AppChannel* channel = resolve_channel_locked(current_name, error);
    if (channel == nullptr) return false;
    for (std::size_t index = 0; index < state_.allocated_channels.load(std::memory_order_relaxed); ++index) {
        if (&state_.apps[index] != channel && state_.apps[index].name == new_name) {
            error = "another app channel already uses the name: " + new_name;
            return false;
        }
    }
    channel->name = std::move(new_name);
    return true;
}

bool ChannelControls::rename_stream(std::string_view current_name, std::string new_name, std::string& error)
{
    if (new_name.empty()) {
        error = "new stream name cannot be empty";
        return false;
    }
    std::lock_guard lock(mutex_);
    PlaybackStream* stream = resolve_stream_locked(current_name, error);
    if (stream == nullptr) return false;
    for (std::size_t index = 0; index < state_.allocated_streams.load(std::memory_order_relaxed); ++index) {
        if (&(*state_.streams[index]) != stream && (*state_.streams[index]).in_use &&
            (*state_.streams[index]).name == new_name) {
            error = "another playback stream already uses the name: " + new_name;
            return false;
        }
    }
    stream->name = std::move(new_name);
    return true;
}

bool ChannelControls::set_capture_source(std::string_view source_selector, std::string& error)
{
    std::lock_guard lock(mutex_);
    if (resolve_capture_channel_locked(source_selector, error) == nullptr) return false;
    error = "capture sources are already available as input channels; assign the channel directly";
    return false;
}

bool ChannelControls::set_capture_gain_db(float decibels, std::string& error)
{
    if (!std::isfinite(decibels) || decibels < kMinimumGainDb || decibels > kMaximumGainDb) {
        error = "gain must be between -60 dB and +12 dB";
        return false;
    }
    for (auto& channel : records(state_.captures))
        if (channel.in_use) channel.target_gain.store(std::pow(10.0F, decibels / 20.0F), std::memory_order_relaxed);
    return true;
}

bool ChannelControls::set_capture_muted(std::string_view selector, bool muted, std::string& error)
{
    std::lock_guard lock(mutex_);
    CaptureChannel* channel = resolve_capture_channel_locked(selector, error);
    if (channel == nullptr) return false;
    channel->muted.store(muted, std::memory_order_release);
    return true;
}

bool ChannelControls::adjust_fader_db(std::string_view scope, std::string_view selector, float delta_db,
                                     bool minimum, std::string& error)
{
    return update_fader_db(scope, selector, delta_db,
                           minimum ? FaderUpdateMode::minimum : FaderUpdateMode::relative, error);
}

bool ChannelControls::set_fader_db(std::string_view scope, std::string_view selector, float decibels,
                                  bool minimum, std::string& error)
{
    return update_fader_db(scope, selector, decibels,
                           minimum ? FaderUpdateMode::minimum : FaderUpdateMode::absolute, error);
}

bool ChannelControls::update_fader_db(std::string_view scope, std::string_view selector, float decibels,
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
        std::lock_guard lock(mutex_);
        AppChannel* channel = resolve_channel_locked(selector, error);
        if (channel == nullptr) return false;
        channel->target_gain.store(adjusted(channel->target_gain.load(std::memory_order_relaxed)),
                                   std::memory_order_release);
        return true;
    }
    if (scope == "stream") {
        std::lock_guard lock(mutex_);
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
        std::lock_guard lock(mutex_);
        CaptureChannel* channel = resolve_capture_channel_locked(selector, error);
        if (channel == nullptr) return false;
        channel->target_gain.store(adjusted(channel->target_gain.load(std::memory_order_relaxed)),
                                   std::memory_order_release);
        return true;
    }
    if (scope == "output") {
        std::lock_guard lock(mutex_);
        OutputChannel* output = resolve_output_locked(selector, error);
        if (output == nullptr) return false;
        output->target_gain.store(adjusted(output->target_gain.load(std::memory_order_relaxed)),
                                  std::memory_order_release);
        return true;
    }
    error = "fader scope must be app, stream, capture, or output";
    return false;
}

bool ChannelControls::set_output_gain_db(std::string_view output_selector, float decibels, std::string& error)
{
    if (!std::isfinite(decibels) || decibels < kMinimumGainDb || decibels > kMaximumGainDb) {
        error = "gain must be between -60 dB and +12 dB";
        return false;
    }
    std::lock_guard lock(mutex_);
    OutputChannel* output = resolve_output_locked(output_selector, error);
    if (output == nullptr) return false;
    output->target_gain.store(std::pow(10.0F, decibels / 20.0F), std::memory_order_release);
    return true;
}

bool ChannelControls::set_output_muted(std::string_view output_selector, bool muted, std::string& error)
{
    std::lock_guard lock(mutex_);
    OutputChannel* output = resolve_output_locked(output_selector, error);
    if (output == nullptr) return false;
    output->muted.store(muted, std::memory_order_release);
    return true;
}

bool ChannelControls::set_capture_speaker_send(bool enabled, std::string& error)
{
    (void)error;
    for (auto& channel : records(state_.captures)) {
        if (!channel.in_use) continue;
        auto mask = channel.output_mask.load(std::memory_order_relaxed);
        if (enabled) mask |= 1U;
        else mask &= ~1U;
        channel.output_mask.store(mask, std::memory_order_release);
    }
    return true;
}

std::string ChannelControls::list_meters(std::string_view selector) const
{
    std::lock_guard lock(mutex_);
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

    const auto channel_count = state_.allocated_channels.load(std::memory_order_relaxed);
    const auto stream_count = state_.allocated_streams.load(std::memory_order_relaxed);
    for (std::size_t channel_index = 0; channel_index < channel_count; ++channel_index) {
        const auto& channel = state_.apps[channel_index];
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
                const auto& stream = (*state_.streams[stream_index]);
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
        const auto& stream = (*state_.streams[stream_index]);
        if (!stream.active.load(std::memory_order_relaxed) || !stream.in_use ||
            !matches({stream.selector_id, stream.name})) continue;
        append("stream", stream.selector_id + " (" + stream.name + ")", stream.meter.reading());
    }
    for (const auto& channel : records(state_.captures)) {
        if (!channel.in_use || !matches({"capture", channel.name, channel.selector_id})) continue;
        append("capture", channel.name + " (" + channel.selector_id + ")", channel.meter.reading());
    }
    for (const auto& output : records(state_.outputs)) {
        if (!output.in_use || !matches({output.name, output.identity})) continue;
        append("output", output.name + " (" + output.identity + ")", output.meter.reading());
    }

    std::ostringstream result;
    result << "Live meters (peak and RMS in dBFS; 100 ms windows; app readings use its first assigned output mix; stream, capture, and output readings are post-fader)\n"
           << "Scope | Channel | Peak dBFS | RMS dBFS\n";
    for (const auto& row : rows)
        result << row.scope << " | " << row.selector << " | " << row.peak << " | " << row.rms << '\n';
    if (rows.empty()) result << (selector.empty() ? "No active channels\n" : "No meter matches '" + std::string(selector) + "'\n");
    return result.str();
}

std::string ChannelControls::fader_status() const
{
    std::lock_guard lock(mutex_);
    std::vector<TableRow> app_rows;
    for (std::size_t i = 0; i < state_.allocated_channels.load(std::memory_order_relaxed); ++i) {
        const auto& channel = state_.apps[i];
        app_rows.push_back({channel.name, format_fader_position(channel.target_gain.load())});
    }
    std::vector<TableRow> stream_rows;
    for (std::size_t i = 0; i < state_.allocated_streams.load(std::memory_order_relaxed); ++i) {
        const auto& stream = (*state_.streams[i]);
        if (!stream.in_use || !stream.active.load(std::memory_order_relaxed)) continue;
        stream_rows.push_back({stream.name, format_fader_position(stream.target_gain.load())});
    }
    std::vector<TableRow> capture_rows;
    for (const auto& channel : records(state_.captures))
        if (channel.in_use) capture_rows.push_back({channel.name,
            format_fader_position(channel.target_gain.load())});
    std::vector<TableRow> output_rows;
    for (const auto& output : records(state_.outputs)) {
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

void ChannelControls::begin_shutdown_fade() noexcept
{
    state_.shutdown_fade_requested.store(true, std::memory_order_release);
}

AppChannel* ChannelControls::find_channel_locked(std::string_view channel_id)
{
    for (std::size_t index = 0; index < state_.allocated_channels.load(std::memory_order_relaxed); ++index) {
        if (state_.apps[index].id == channel_id) {
            return &state_.apps[index];
        }
    }
    return nullptr;
}

AppChannel* ChannelControls::resolve_channel_locked(std::string_view selector,
                                                                   std::string& error)
{
    if (AppChannel* by_id = find_channel_locked(selector); by_id != nullptr) return by_id;
    AppChannel* match = nullptr;
    for (std::size_t index = 0; index < state_.allocated_channels.load(std::memory_order_relaxed); ++index) {
        AppChannel& channel = state_.apps[index];
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

PlaybackStream* ChannelControls::resolve_stream_locked(std::string_view selector,
                                                                      std::string& error)
{
    PlaybackStream* match = nullptr;
    for (std::size_t index = 0; index < state_.allocated_streams.load(std::memory_order_relaxed); ++index) {
        PlaybackStream& stream = (*state_.streams[index]);
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

CaptureChannel* ChannelControls::resolve_capture_channel_locked(
    std::string_view selector, std::string& error)
{
    if (selector == "capture" || selector_name_matches(selector, "Microphone")) {
        CaptureChannel* only = nullptr;
        for (auto& channel : records(state_.captures)) {
            if (!channel.in_use) continue;
            if (only != nullptr) {
                error = "multiple capture channels are available; use a listed capture name or ID";
                return nullptr;
            }
            only = &channel;
        }
        if (only == nullptr) error = "no capture channels are available";
        return only;
    }
    CaptureChannel* match = nullptr;
    for (auto& channel : records(state_.captures)) {
        if (!channel.in_use) continue;
        if (channel.selector_id != selector && !selector_name_matches(channel.name, selector) &&
            channel.identity != selector) continue;
        if (match != nullptr) {
            error = "capture selector is ambiguous; use the listed capture ID";
            return nullptr;
        }
        match = &channel;
    }
    if (match == nullptr) error = "unknown capture channel: " + std::string(selector);
    return match;
}

OutputChannel* ChannelControls::resolve_output_locked(std::string_view selector,
                                                                    std::string& error)
{
    OutputChannel* match = nullptr;
    for (auto& output : records(state_.outputs)) {
        if (!output.in_use) continue;
        if (output.name == selector || output.identity == selector) {
            if (match != nullptr) {
                error = "output selector is ambiguous; use its listed output identity";
                return nullptr;
            }
            match = &output;
        }
    }
    if (match != nullptr) return match;
    error = "unknown output channel: " + std::string(selector);
    return nullptr;
}

bool ChannelControls::assign_app_output_locked(std::string_view app_selector,
                                              std::string_view output_selector, bool assigned,
                                              std::string& error)
{
    AppChannel* app = resolve_channel_locked(app_selector, error);
    if (app == nullptr) return false;
    OutputChannel* output = resolve_output_locked(output_selector, error);
    if (output == nullptr) return false;
    const auto index = static_cast<std::size_t>(std::find(state_.outputs.begin(), state_.outputs.end(), output)
                                               - state_.outputs.begin());
    const auto bit = 1U << index;
    auto mask = app->output_mask.load(std::memory_order_relaxed);
    if (assigned) mask |= bit;
    else mask &= ~bit;
    app->output_mask.store(mask, std::memory_order_release);
    return true;
}

bool ChannelControls::assign_input_output_locked(std::string_view scope, std::string_view selector,
                                                  std::string_view output_selector, bool assigned,
                                                  std::string& error)
{
    OutputChannel* output = resolve_output_locked(output_selector, error);
    if (output == nullptr) return false;
    const auto index = static_cast<std::size_t>(std::find(state_.outputs.begin(), state_.outputs.end(), output)
                                               - state_.outputs.begin());
    const std::uint32_t bit = 1U << index;
    std::atomic<std::uint32_t>* mask = nullptr;
    if (scope == "stream") {
        auto* stream = resolve_stream_locked(selector, error);
        if (stream != nullptr) mask = &stream->output_mask;
    } else if (scope == "capture") {
        auto* channel = resolve_capture_channel_locked(selector, error);
        if (channel != nullptr) mask = &channel->output_mask;
    } else error = "input send scope must be stream or capture";
    if (mask == nullptr) return false;
    const auto value = mask->load(std::memory_order_relaxed);
    mask->store(assigned ? (value | bit) : (value & ~bit), std::memory_order_release);
    return true;
}

} // namespace shatter::mixer
