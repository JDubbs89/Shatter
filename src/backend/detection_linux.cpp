#include "backend/selection.hpp"

#include <pipewire/pipewire.h>
#include <pulse/pulseaudio.h>

#include <algorithm>
#include <chrono>
#include <optional>

namespace shatter::backend {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto probe_timeout = std::chrono::milliseconds(300);

std::optional<Server> probe_pipewire()
{
    static const bool initialized = [] { pw_init(nullptr, nullptr); return true; }();
    (void)initialized;
    auto* main_loop = pw_main_loop_new(nullptr);
    if (main_loop == nullptr) return {};
    auto* loop = pw_main_loop_get_loop(main_loop);
    auto* context = pw_context_new(loop, nullptr, 0);
    auto* core = context == nullptr ? nullptr : pw_context_connect(context, nullptr, 0);
    std::optional<Server> result;
    if (core != nullptr) {
        struct Probe { bool done = false; std::optional<Server>* result; } probe{false, &result};
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.info = [](void* data, const pw_core_info* info) {
            auto& state = *static_cast<Probe*>(data);
            *state.result = Server{Kind::pipewire, std::to_string(info->cookie)};
            state.done = true;
        };
        events.error = [](void* data, std::uint32_t, int, int, const char*) {
            static_cast<Probe*>(data)->done = true;
        };
        spa_hook listener{};
        pw_core_add_listener(core, &listener, &events, &probe);
        const auto deadline = Clock::now() + probe_timeout;
        pw_loop_enter(loop);
        while (!probe.done && Clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (pw_loop_iterate(loop, static_cast<int>(std::max<std::int64_t>(1, remaining.count()))) < 0) break;
        }
        pw_loop_leave(loop);
        spa_hook_remove(&listener);
        pw_core_disconnect(core);
    }
    if (context != nullptr) pw_context_destroy(context);
    pw_main_loop_destroy(main_loop);
    return result;
}

std::optional<Server> probe_pulseaudio()
{
    auto* loop = pa_mainloop_new();
    if (loop == nullptr) return {};
    auto* context = pa_context_new(pa_mainloop_get_api(loop), "Shatter audio detection");
    std::optional<Server> result;
    if (context != nullptr) {
        struct Probe {
            bool done = false;
            std::optional<Server>* result;
            pa_operation* query = nullptr;
        } probe{false, &result, nullptr};
        pa_context_set_state_callback(context, [](pa_context* connection, void* data) {
            auto& state = *static_cast<Probe*>(data);
            switch (pa_context_get_state(connection)) {
            case PA_CONTEXT_READY:
                state.query = pa_context_get_server_info(connection, [](pa_context*, const pa_server_info* info, void* data) {
                    auto& state = *static_cast<Probe*>(data);
                    if (info != nullptr)
                        *state.result = Server{Kind::pulseaudio, std::to_string(info->cookie)};
                    state.done = true;
                }, &state);
                if (state.query == nullptr) state.done = true;
                break;
            case PA_CONTEXT_FAILED:
            case PA_CONTEXT_TERMINATED: state.done = true; break;
            default: break;
            }
        }, &probe);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0) {
            const auto deadline = Clock::now() + probe_timeout;
            while (!probe.done && Clock::now() < deadline) {
                if (pa_mainloop_prepare(loop, 25000) < 0 || pa_mainloop_poll(loop) < 0 ||
                    pa_mainloop_dispatch(loop) < 0) break;
            }
        }
        pa_context_set_state_callback(context, nullptr, nullptr);
        if (probe.query != nullptr) { pa_operation_cancel(probe.query); pa_operation_unref(probe.query); }
        pa_context_disconnect(context);
        pa_context_unref(context);
    }
    pa_mainloop_free(loop);
    return result;
}
} // namespace

Server detect_server()
{
    if (auto server = probe_pipewire()) return *server;
    if (auto server = probe_pulseaudio()) return *server;
    return {};
}

} // namespace shatter::backend
