#pragma once

#include "shatter/engine/audio_ring.hpp"

#include <pipewire/pipewire.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace shatter::engine {

class PipeWireEngine {
public:
    PipeWireEngine() = default;
    ~PipeWireEngine();

    PipeWireEngine(const PipeWireEngine&) = delete;
    PipeWireEngine& operator=(const PipeWireEngine&) = delete;

    bool start(std::string& error);
    void stop();
    bool set_app_gain_db(float decibels, std::string& error);
    std::string list_inputs() const;
    std::string list_outputs() const;
    bool available() const noexcept;

private:
    struct NodeInfo {
        std::string media_class;
        std::string name;
        std::string description;
        std::string application;
    };
    struct LinkInfo {
        std::uint32_t output_node = PW_ID_ANY;
        std::uint32_t input_node = PW_ID_ANY;
    };

    static void on_input_state_changed(void* data, pw_stream_state old_state,
                                       pw_stream_state state, const char* error);
    static void on_output_state_changed(void* data, pw_stream_state old_state,
                                        pw_stream_state state, const char* error);
    static void on_input_process(void* data);
    static void on_output_process(void* data);
    static void on_global(void* data, std::uint32_t id, std::uint32_t permissions,
                          const char* type, std::uint32_t version, const spa_dict* props);
    static void on_global_remove(void* data, std::uint32_t id);

    void input_process();
    void output_process();
    void update_stream_state(bool input, pw_stream_state state, const char* error);
    void refresh_output_link_locked();
    void run_loop();
    void cleanup();

    pw_main_loop* main_loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    pw_stream* app_playback_sink_ = nullptr;
    pw_stream* speaker_output_ = nullptr;
    spa_hook registry_listener_{};
    spa_hook input_listener_{};
    spa_hook output_listener_{};
    pw_registry_events registry_events_{};
    pw_stream_events input_events_{};
    pw_stream_events output_events_{};
    std::thread loop_thread_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_changed_;
    std::string startup_error_;
    bool input_ready_ = false;
    bool output_ready_ = false;
    bool loop_running_ = false;
    bool available_ = false;
    mutable std::mutex nodes_mutex_;
    std::unordered_map<std::uint32_t, NodeInfo> nodes_;
    std::unordered_map<std::uint32_t, LinkInfo> links_;
    std::atomic<std::uint32_t> app_sink_node_id_{PW_ID_ANY};
    std::atomic<std::uint32_t> speaker_node_id_{PW_ID_ANY};
    std::atomic<bool> output_linked_{false};
    AudioRingBuffer audio_ring_;
    std::atomic<float> target_gain_{1.0F};
    float smoothed_gain_ = 1.0F;
};

} // namespace shatter::engine
