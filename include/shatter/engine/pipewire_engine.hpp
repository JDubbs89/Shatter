#pragma once

#include "shatter/engine/audio_ring.hpp"
#include "shatter/engine/runtime_profiler.hpp"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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

    PipeWireEngine() = default;
    ~PipeWireEngine();

    PipeWireEngine(const PipeWireEngine&) = delete;
    PipeWireEngine& operator=(const PipeWireEngine&) = delete;

    bool start(std::string& error);
    void stop();
    bool set_app_gain_db(std::string_view channel_id, float decibels, std::string& error);
    bool set_stream_gain_db(std::uint32_t stream_id, float decibels, std::string& error);
    bool set_app_muted(std::string_view channel_id, bool muted, std::string& error);
    bool set_stream_muted(std::uint32_t stream_id, bool muted, std::string& error);
    bool set_capture_source(std::uint32_t source_node_id, std::string& error);
    bool set_capture_gain_db(float decibels, std::string& error);
    bool set_capture_speaker_send(bool enabled, std::string& error);
    std::string list_inputs() const;
    std::string list_outputs() const;
    bool profile_start(std::string& error);
    std::string profile_stop(std::string& error);
    bool available() const noexcept;

private:
    struct AppChannel {
        bool allocated = false;
        std::string identity;
        std::string id;
        std::string name;
        std::atomic<float> target_gain{1.0F};
        std::atomic<bool> muted{false};
        float smoothed_gain = 1.0F;
    };

    struct PlaybackStream {
        PipeWireEngine* engine = nullptr;
        bool in_use = false;
        std::atomic<bool> active{false};
        std::atomic<std::uint32_t> mixer_users{0};
        std::uint32_t node_id = PW_ID_ANY;
        std::size_t channel_index = 0;
        std::string name;
        std::string application;
        std::string sink_name;
        pw_stream* sink = nullptr;
        spa_hook listener{};
        pw_stream_events events{};
        std::atomic<float> target_gain{1.0F};
        std::atomic<bool> muted{false};
        float smoothed_gain = 1.0F;
        AudioRingBuffer audio;
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
        std::atomic<bool> speaker_send_enabled{false};
        float smoothed_gain = 1.0F;
        AudioRingBuffer audio;
    };

    struct CaptureSourceRequest {
        PipeWireEngine* engine = nullptr;
        std::uint32_t source_node_id = PW_ID_ANY;
        bool succeeded = false;
        std::string error;
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
        std::size_t channel_index = max_app_channels;
    };
    struct LinkInfo {
        std::uint32_t output_node = PW_ID_ANY;
        std::uint32_t input_node = PW_ID_ANY;
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
    static void on_global(void* data, std::uint32_t id, std::uint32_t permissions,
                          const char* type, std::uint32_t version, const spa_dict* props);
    static void on_global_remove(void* data, std::uint32_t id);

    void stream_input_process(PlaybackStream& stream);
    void capture_source_process();
    void output_process();
    void update_playback_stream_state(PlaybackStream& stream, pw_stream_state state, const char* error);
    void refresh_output_link_locked();
    void handle_playback_node(std::uint32_t node_id, NodeInfo node);
    void create_stream_sink(PlaybackStream& stream);
    bool replace_capture_source(std::uint32_t source_node_id, std::string& error);
    void destroy_capture_stream();
    void assign_stream_target(PlaybackStream& stream);
    std::size_t get_or_create_channel_locked(const NodeInfo& node);
    PlaybackStream* find_stream_locked(std::uint32_t node_id);
    AppChannel* find_channel_locked(std::string_view channel_id);
    void run_loop();
    void cleanup();

    pw_main_loop* main_loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    pw_metadata* default_metadata_ = nullptr;
    pw_stream* speaker_output_ = nullptr;
    spa_hook registry_listener_{};
    spa_hook output_listener_{};
    pw_registry_events registry_events_{};
    pw_stream_events output_events_{};
    std::thread loop_thread_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_changed_;
    std::string startup_error_;
    bool output_ready_ = false;
    bool available_ = false;
    mutable std::mutex nodes_mutex_;
    std::unordered_map<std::uint32_t, NodeInfo> nodes_;
    std::unordered_map<std::uint32_t, LinkInfo> links_;
    std::unordered_map<std::string, std::size_t> channel_by_identity_;
    std::unordered_map<std::uint32_t, std::size_t> stream_by_node_;
    std::array<AppChannel, max_app_channels> app_channels_{};
    std::array<PlaybackStream, max_playback_streams> playback_streams_{};
    CaptureChannel capture_channel_{};
    std::atomic<std::size_t> allocated_channels_{0};
    std::atomic<std::size_t> allocated_streams_{0};
    std::atomic<std::uint32_t> unrouted_playback_streams_{0};
    std::atomic<std::uint32_t> speaker_node_id_{PW_ID_ANY};
    std::atomic<bool> output_linked_{false};
    float smoothed_gain_ = 1.0F;
    RuntimeProfiler profiler_;
    std::atomic<bool> profile_running_{false};
    std::chrono::steady_clock::time_point profile_started_{};
    std::uint64_t profile_user_cpu_us_ = 0;
    std::uint64_t profile_system_cpu_us_ = 0;
};

} // namespace shatter::engine
