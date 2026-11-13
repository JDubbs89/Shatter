#pragma once

#include "backend/iaudio_backend.hpp"
#include "mixer/processor.hpp"
#include "mixer/channel_controls.hpp"
#include "mixer/audio_engine.hpp"
#include "mixer/health.hpp"
#include "mixer/realtime_access_gate.hpp"
#include "mixer/runtime_profiler.hpp"
#include "mixer/meter/meter_bus.hpp"
#include "mixer/state/store.hpp"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <pipewire/proxy.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace shatter::backend {

namespace meter = mixer::meter;
namespace state = mixer::state;
using mixer::RealtimeAccessGate;
using mixer::RuntimeProfiler;
using mixer::XrunCounters;

class PipeWireBackend final : public IAudioBackend {
    struct PlaybackStream;
    struct CaptureChannel;
    struct OutputChannel;

public:
    static constexpr std::size_t max_app_channels = mixer::max_app_channels;
    static constexpr std::size_t max_playback_streams = mixer::max_playback_streams;
    static constexpr std::size_t max_output_channels = mixer::max_output_channels;
    static constexpr std::size_t max_capture_channels = mixer::max_capture_channels;

    PipeWireBackend();
    ~PipeWireBackend() override;

    PipeWireBackend(const PipeWireBackend&) = delete;
    PipeWireBackend& operator=(const PipeWireBackend&) = delete;

    // Concrete callback operations used by AudioEngine<PipeWireBackend>.
    // The native token remains private to this adapter, outside the mixer seam.
    struct InputBuffer {
        pw_buffer* native = nullptr;
        const float* data = nullptr;
        std::size_t frame_count = 0;
        explicit operator bool() const noexcept { return native != nullptr; }
        const float* samples() const noexcept { return data; }
        std::size_t frames() const noexcept { return frame_count; }
    };
    struct OutputBuffer {
        pw_buffer* native = nullptr;
        float* data = nullptr;
        std::size_t frame_count = 0;
        explicit operator bool() const noexcept { return native != nullptr; }
        float* samples() const noexcept { return data; }
        std::size_t frames() const noexcept { return frame_count; }
    };
    InputBuffer acquire_input(PlaybackStream& stream) noexcept;
    InputBuffer acquire_input(CaptureChannel& channel) noexcept;
    void release_input(PlaybackStream& stream, InputBuffer buffer) noexcept;
    void release_input(CaptureChannel& channel, InputBuffer buffer) noexcept;
    OutputBuffer acquire_output(OutputChannel& output) noexcept;
    void submit_output(OutputChannel& output, OutputBuffer buffer) noexcept;
    std::size_t output_index(const OutputChannel& output) const noexcept;
    void observe_output_timing(OutputChannel& output, const OutputBuffer& buffer,
                               RuntimeProfiler::Scope& profile) noexcept;

    bool enumerate_endpoints(std::vector<Endpoint>& endpoints, std::string& error) const override;
    static std::string unsupported_operation() { return "PipeWire audio is unavailable"; }
    mixer::ProcessingState processing_state() noexcept;
    mixer::ChannelControls channel_controls() noexcept { return {processing_state(), nodes_mutex_}; }

    bool start(std::string& error) override;
    void stop() override;
    bool make_output(std::string name, bool capture_source, std::string& error);
    bool delete_output(std::string_view selector, std::string& error);
    bool assign_output_destination(std::string_view output_selector,
                                   std::string_view destination_selector,
                                   bool assigned, std::string& error);
    bool assign_app_output(std::string_view app_selector, std::string_view output_selector, bool assigned,
                           std::string& error);
    bool assign_input_output(std::string_view scope, std::string_view input_selector,
                             std::string_view output_selector, bool assigned,
                             std::string& error);
    bool load_config(std::string_view name, std::string& error);
    bool save_config(std::string_view name, std::string& error);
    bool load_default_config(std::string& name, std::string& error);
    std::string list_inputs(std::size_t terminal_width = 100) const;
    std::string list_outputs(std::size_t terminal_width = 100) const;
    std::string health_status() const;
    bool profile_start(std::string& error);
    std::string profile_stop(std::string& error);
    bool available() const noexcept override;
    void pause_streams_for_shutdown() noexcept;

private:
    using AppChannel = mixer::AppChannel;

    struct PlaybackStream : mixer::PlaybackStream {
        PipeWireBackend* backend = nullptr;
        std::uint32_t node_id = PW_ID_ANY;
        std::string sink_name;
        pw_stream* sink = nullptr;
        spa_hook listener{};
        pw_stream_events events{};
    };

    using PlaybackRouteState = mixer::PlaybackRouteState;

    struct CaptureChannel : mixer::CaptureChannel {
        PipeWireBackend* backend = nullptr;
        pw_stream* source_stream = nullptr;
        spa_hook listener{};
        pw_stream_events events{};
        std::uint32_t source_node_id = PW_ID_ANY;
    };

    struct OutputChannel : mixer::OutputChannel {
        PipeWireBackend* backend = nullptr;
        std::uint32_t node_id = PW_ID_ANY;
        std::array<std::uint32_t, 8> sink_node_ids{};
        std::size_t sink_count = 0;
        std::array<std::uint32_t, 8> assigned_sink_node_ids{};
        std::array<std::string, 8> assigned_sink_names{};
        std::array<std::string, 8> assigned_sink_identities{};
        std::size_t assigned_sink_count = 0;
        pw_stream* stream = nullptr;
        spa_hook listener{};
        pw_stream_events events{};
    };

    struct OutputRequest {
        PipeWireBackend* backend = nullptr;
        enum class Action { make, remove, assign_sink, assign_app, assign_input, load_config } action = Action::make;
        std::string first;
        std::string second;
        state::Show show;
        bool assigned = false;
        bool capture_source = false;
        bool succeeded = false;
        bool completed = false;
        std::string error;
        std::mutex completion_mutex;
        std::condition_variable completion_cv;
    };
    struct NodeInfo {
        std::uint32_t node_id = PW_ID_ANY;
        std::string media_class;
        std::string name;
        std::string description;
        std::string application;
        std::string application_id;
        std::string process_binary;
        std::string identity;
        std::string device_identity;
        std::size_t channel_index = max_app_channels;
    };
    struct LinkInfo {
        std::uint32_t output_node = PW_ID_ANY;
        std::uint32_t input_node = PW_ID_ANY;
    };
    struct PortInfo {
        std::uint32_t node_id = PW_ID_ANY;
        std::uint32_t port_id = PW_ID_ANY;
        std::string name;
        bool input = false;
    };
    struct ManagedLink {
        std::uint32_t output_node = PW_ID_ANY;
        std::uint32_t sink_node = PW_ID_ANY;
        pw_proxy* proxy = nullptr;
    };

    static void on_output_state_changed(void* data, pw_stream_state old_state,
                                        pw_stream_state state, const char* error);
    static void on_stream_input_state_changed(void* data, pw_stream_state old_state,
                                              pw_stream_state state, const char* error);
    static void on_output_process(void* data);
    static void on_stream_input_process(void* data);
    static void on_capture_source_state_changed(void* data, pw_stream_state old_state,
                                                pw_stream_state state, const char* error);
    static void on_capture_source_process(void* data);
    static int on_output_request(spa_loop* loop, bool async, std::uint32_t sequence,
                                 const void* data, std::size_t size, void* user_data);
    static void on_global(void* data, std::uint32_t id, std::uint32_t permissions,
                          const char* type, std::uint32_t version, const spa_dict* props);
    static void on_global_remove(void* data, std::uint32_t id);

    void stream_input_process(PlaybackStream& stream);
    void capture_source_process(CaptureChannel& channel);
    void output_process(OutputChannel& output);
    void update_playback_stream_state(PlaybackStream& stream, pw_stream_state state, const char* error);
    void refresh_output_link_locked();
    bool invoke_output_request(OutputRequest& request, std::string& error);
    bool make_output_locked(std::string name, bool capture_source, std::string& error);
    bool make_output_at_locked(std::string name, std::size_t slot, bool capture_source, std::string& error);
    bool delete_output_locked(std::string_view selector, std::string& error);
    bool apply_config_locked(state::Show show, std::string& error);
    std::uint32_t output_mask_for_identities(const std::vector<std::string>& identities) const;
    void restore_output_routes_locked(OutputChannel& output);
    static bool is_output_destination(const NodeInfo& node);
    static std::string output_destination_name(const NodeInfo& node);
    static std::string capture_source_name(const NodeInfo& node);
    static std::string output_destination_identity(const NodeInfo& node);
    static std::string output_destination_selector(const NodeInfo& node);
    bool assign_output_sink_locked(std::string_view output_selector, std::string_view sink_selector,
                                   bool assigned, std::string& error);
    bool assign_app_output_locked(std::string_view app_selector, std::string_view output_selector,
                                  bool assigned, std::string& error);
    OutputChannel* resolve_output_locked(std::string_view selector, std::string& error);
    bool create_output_stream(OutputChannel& output, bool autoconnect, std::string& error);
    void destroy_output_stream(OutputChannel& output);
    void handle_playback_node(std::uint32_t node_id, NodeInfo node);
    void create_stream_sink(PlaybackStream& stream);
    bool add_capture_channel(const NodeInfo& source, std::string& error);
    void destroy_capture_channel(CaptureChannel& channel, bool forget = true);
    CaptureChannel* resolve_capture_channel_locked(std::string_view selector, std::string& error);
    void assign_stream_target(PlaybackStream& stream);
    std::size_t get_or_create_channel_locked(const NodeInfo& node);
    PlaybackStream* find_stream_locked(std::uint32_t node_id);
    AppChannel* find_channel_locked(std::string_view channel_id);
    AppChannel* resolve_channel_locked(std::string_view selector, std::string& error);
    PlaybackStream* resolve_stream_locked(std::string_view selector, std::string& error);
    std::string playback_route_key(std::string_view app_identity, std::string_view original_name) const;
    bool sink_binding_matches(const OutputChannel& output, std::size_t index, const NodeInfo& sink) const;
    void run_loop();
    void cleanup();

    pw_main_loop* main_loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    pw_metadata* default_metadata_ = nullptr;
    spa_hook registry_listener_{};
    pw_registry_events registry_events_{};
    std::thread loop_thread_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_changed_;
    std::string startup_error_;
    bool output_ready_ = false;
    bool available_ = false;
    mutable std::mutex nodes_mutex_;
    std::unordered_map<std::uint32_t, NodeInfo> nodes_;
    std::unordered_map<std::uint32_t, LinkInfo> links_;
    std::unordered_map<std::uint32_t, PortInfo> ports_;
    std::vector<ManagedLink> managed_links_;
    std::unordered_map<std::string, std::size_t> channel_by_identity_;
    std::unordered_map<std::uint32_t, std::size_t> stream_by_node_;
    std::unordered_map<std::string, PlaybackRouteState> playback_route_state_;
    std::array<AppChannel, max_app_channels> app_channels_{};
    std::array<PlaybackStream, max_playback_streams> playback_streams_{};
    std::array<OutputChannel, max_output_channels> output_channels_{};
    state::Store state_store_;
    state::Show loaded_show_;
    std::array<CaptureChannel, max_capture_channels> capture_channels_{};
    std::atomic<std::size_t> allocated_channels_{0};
    std::atomic<std::size_t> allocated_streams_{0};
    std::uint64_t next_stream_selector_id_ = 1;
    std::atomic<std::uint32_t> unrouted_playback_streams_{0};
    std::atomic<std::uint32_t> speaker_node_id_{PW_ID_ANY};
    std::atomic<bool> output_linked_{false};
    float smoothed_gain_ = 1.0F;
    RuntimeProfiler profiler_;
    XrunCounters xruns_;
    std::array<std::atomic<std::uint64_t>, max_output_channels> latest_input_ring_frames_{};
    std::atomic<bool> shutdown_fade_requested_{false};
    std::atomic<bool> profile_running_{false};
    std::chrono::steady_clock::time_point profile_started_{};
    std::uint64_t profile_user_cpu_us_ = 0;
    std::uint64_t profile_system_cpu_us_ = 0;

    // Non-owning views of the portable base records; bound before callbacks start.
    std::array<mixer::PlaybackStream*, max_playback_streams> mixer_streams_{};
    std::array<mixer::OutputChannel*, max_output_channels> mixer_outputs_{};
    std::array<mixer::CaptureChannel*, max_capture_channels> mixer_captures_{};
    mixer::Processor processor_;
    mixer::AudioEngine<PipeWireBackend> audio_engine_;
};

} // namespace shatter::backend
