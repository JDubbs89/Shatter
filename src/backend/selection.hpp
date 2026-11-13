#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace shatter::mixer { class IControl; }

namespace shatter::backend {

enum class Kind { none, pipewire, pulseaudio, wasapi };
std::string_view name(Kind kind) noexcept;

// Instance identifies the server, not its devices or default endpoint. A new
// instance of the same server also requires fresh native client state.
struct Server {
    Kind kind = Kind::none;
    std::string instance;
    bool operator==(const Server&) const = default;
};

// Non-realtime, bounded probes. Do not launch audio servers or create endpoints.
Server detect_server();
std::unique_ptr<mixer::IControl> create_engine(Kind kind);

// Debounce flapping observations and enforce a minimum daemon lifetime. Time is
// supplied by the caller so selection behavior can be tested without sleeping.
class RestartPolicy {
public:
    using Clock = std::chrono::steady_clock;
    RestartPolicy(Server initial, Clock::time_point started)
        : initial_(std::move(initial)), candidate_(initial_), started_(started), changed_(started) {}

    bool observe(const Server& server, Clock::time_point now)
    {
        if (server != candidate_) { candidate_ = server; changed_ = now; }
        return candidate_ != initial_ && now - changed_ >= std::chrono::seconds(2) &&
               now - started_ >= std::chrono::seconds(5);
    }

private:
    Server initial_;
    Server candidate_;
    Clock::time_point started_;
    Clock::time_point changed_;
};

} // namespace shatter::backend
