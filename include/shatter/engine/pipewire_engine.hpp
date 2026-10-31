#pragma once

#include "shatter/engine/audio_ring.hpp"
#include "shatter/engine/runtime_profiler.hpp"
#include "shatter/state/store.hpp"

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

namespace shatter::engine {

class PipeWireEngine {
public:
    static constexpr std::size_t max_app_channels = 1024;
    static constexpr std::size_t max_playback_streams = 128;
    static constexpr std::size_t max_output_channels = 8;

    PipeWireEngine() = default;
    ~PipeWireEngine();

    PipeWireEngine(const PipeWireEngine&) = delete;
    PipeWireEngine& operator=(const PipeWireEngine&) = delete;

    bool start(std::string& error);
    void stop();
    bool set_app_gain_db(std::string_view channel_id, float decibels, std::string& error);
    bool set_stream_gain_db(std::string_view stream_selector, float decibels, std::string& error);
    bool set_app_muted(std::string_view channel_id, bool muted, std::string& error);
    bool set_stream_muted(std::string_view stream_selector, bool muted, std::string& error);
    bool rename_app(std::string_view current_name, std::string new_name, std::string& error);
    bool rename_stream(std::string_view current_name, std::string new_name, std::string& error);
    bool set_capture_source(std::uint32_t source_node_id, std::string& error);
    bool set_capture_gain_db(float decibels, std::string& error);
    bool set_capture_muted(bool muted, std::string& error);
    bool adjust_fader_db(std::string_view scope, std::string_view selector, float delta_db,
                         bool minimum, std::string& error);
    bool set_capture_speaker_send(bool enabled, std::string& error);
    bool set_output_gain_db(std::string_view output_selector, float decibels, std::string& error);
    bool set_output_muted(std::string_view output_selector, bool muted, std::string& error);
    bool make_output(std::string name, std::string& error);
    bool delete_output(std::string_view selector, std::string& error);
    bool assign_output_sink(std::string_view output_selector, std::string_view sink_selector, bool assigned,
                            std::string& error);
    bool assign_app_output(std::string_view app_selector, std::string_view output_selector, bool assigned,
                           std::string& error);
    bool assign_input_output(std::string_view scope, std::string_view input_selector,
                             std::string_view output_selector, bool assigned, std::string& error);
    bool load_config(std::string_view name, std::string& error);
    bool save_config(std::string_view name, std::string& error);
    bool load_default_config(std::string& name, std::string& error);
    std::string list_inputs(std::size_t terminal_width = 100) const;
    std::string list_outputs(std::size_t terminal_width = 100) const;
    std::string fader_status() const;
    bool profile_start(std::string& error);
    std::string profile_stop(std::string& error);
    bool available() const noexcept;
    void begin_shutdown_fade() noexcept;
    void pause_streams_for_shutdown() noexcept;

private:
    struct AppChannel {
        bool allocated = false;
        std::string identity;
        std::string id;
        std::string name;
        std::atomic<float> target_gain{1.0F};
        std::atomic<bool> muted{false};
        std::atomic<std::uint32_t> output_mask{1};
        std::array<float, max_output_channels> smoothed_gain{};
    };

    struct PlaybackStream {
        PipeWireEngine* engine = nullptr;
        bool in_use = false;
        std::atomic<bool> active{false};
        std::atomic<std::uint32_t> mixer_users{0};
        std::uint32_t node_id = PW_ID_ANY;
        std::size_t channel_index = 0;
        std::string name;
        std::string original_name;
        std::string application;
        std::string sink_name;
        pw_stream* sink = nullptr;
        spa_hook listener{};
        pw_stream_events events{};
        std::atomic<float> target_gain{1.0F};
        std::atomic<bool> muted{false};
        std::atomic<std::uint32_t> output_mask{0};
        float smoothed_gain = 1.0F;
        std::array<float, max_output_channels> smoothed_send_gain{};
        std::array<AudioRingBuffer, max_output_channels> audio;
        std::array<AudioRingBuffer, max_output_channels> send_audio;
    };

    struct PlaybackRouteState {
        std::string name;
        float gain = 1.0F;
        bool muted = false;
        std::uint32_t output_mask = 0;
    };

    struct CaptureChannel {
        pw_stream* source_stream = nullptr;
        spa_hook listener{};
        pw_stream_events events{};
        std::uint32_t source_node_id = PW_ID_ANY;
        std::atomic<bool> active{false};
        std::atomic<std::uint32_t> mixer_users{0};
        std::atomic<float> target_gain{1.0F};
        std::atomic<bool> muted{false};
        std::atomic<std::uint32_t> output_mask{0};
        float smoothed_gain = 1.0F;
        std::array<AudioRingBuffer, max_output_channels> audio;
    };

    struct OutputChannel {
        PipeWireEngine* engine = nullptr;
        bool in_use = false;
        std::atomic<bool> active{false};
        std::atomic<bool> linked{false};
        std::atomic<std::uint32_t> mixer_users{0};
        std::atomic<std::uint32_t> input_users{0};
        std::string name;
        std::string node_name;
        std::atomic<float> target_gain{1.0F};
        std::atomic<bool> muted{false};
        float smoothed_gain = 1.0F;
        std::uint32_t node_id = PW_ID_ANY;
        std::array<std::uint32_t, 8> sink_node_ids{};
        std::size_t sink_count = 0;
        std::array<std::uint32_t, 8> assigned_sink_node_ids{};
        std::array<std::string, 8> assigned_sink_names{};
        std::array<std::string, 8> assigned_sink_identities{};
        std::size_t assigned_sink_count = 0;
        float shutdown_fade_gain = 1.0F;
        std::uint32_t shutdown_fade_frames_remaining = 0;
        pw_stream* stream = nullptr;
        spa_hook listener{};
        pw_stream_events events{};
    };

    struct CaptureSourceRequest {
        PipeWireEngine* engine = nullptr;
        std::uint32_t source_node_id = PW_ID_ANY;
        bool succeeded = false;
        std::string error;
    };
    struct OutputRequest {
        PipeWireEngine* engine = nullptr;
        enum class Action { make, remove, assign_sink, assign_app, assign_input } action = Action::make;
        std::string first;
        std::string second;
        bool assigned = false;
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
    static void on_input_process(void* data);
    static void on_output_process(void* data);
    static void on_stream_input_process(void* data);
    static void on_capture_source_state_changed(void* data, pw_stream_state old_state,
                                                pw_stream_state state, const char* error);
    static void on_capture_source_process(void* data);
    static int on_capture_source_request(spa_loop* loop, bool async, std::uint32_t sequence,
                                         const void* data, std::size_t size, void* user_data);
    static int on_output_request(spa_loop* loop, bool async, std::uint32_t sequence,
                                 const void* data, std::size_t size, void* user_data);
    static void on_global(void* data, std::uint32_t id, std::uint32_t permissions,
                          const char* type, std::uint32_t version, const spa_dict* props);
    static void on_global_remove(void* data, std::uint32_t id);

    void stream_input_process(PlaybackStream& stream);
    void capture_source_process();
    void output_process(OutputChannel& output);
    void update_playback_stream_state(PlaybackStream& stream, pw_stream_state state, const char* error);
    void refresh_output_link_locked();
    bool invoke_output_request(OutputRequest& request, std::string& error);
    bool make_output_locked(std::string name, std::string& error);
    bool delete_output_locked(std::string_view selector, std::string& error);
    bool assign_output_sink_locked(std::string_view output_selector, std::string_view sink_selector,
                                   bool assigned, std::string& error);
    bool assign_app_output_locked(std::string_view app_selector, std::string_view output_selector,
                                  bool assigned, std::string& error);
    OutputChannel* resolve_output_locked(std::string_view selector, std::string& error);
    bool create_output_stream(OutputChannel& output, bool autoconnect, std::string& error);
    void destroy_output_stream(OutputChannel& output);
    void handle_playback_node(std::uint32_t node_id, NodeInfo node);
    void create_stream_sink(PlaybackStream& stream);
    bool replace_capture_source(std::uint32_t source_node_id, std::string& error);
    void destroy_capture_stream();
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
    CaptureChannel capture_channel_{};
    std::atomic<std::size_t> allocated_channels_{0};
    std::atomic<std::size_t> allocated_streams_{0};
    std::atomic<std::uint32_t> unrouted_playback_streams_{0};
    std::atomic<std::uint32_t> speaker_node_id_{PW_ID_ANY};
    std::atomic<bool> output_linked_{false};
    float smoothed_gain_ = 1.0F;
    RuntimeProfiler profiler_;
    std::atomic<bool> shutdown_fade_requested_{false};
    std::atomic<bool> profile_running_{false};
    std::chrono::steady_clock::time_point profile_started_{};
    std::uint64_t profile_user_cpu_us_ = 0;
    std::uint64_t profile_system_cpu_us_ = 0;
};

} // namespace shatter::engine
