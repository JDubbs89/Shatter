#include "control/backend_watcher.hpp"
#include "control/platform/service.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace shatter::control {

BackendWatcher::BackendWatcher(backend::Server initial)
    : worker_([this, initial = std::move(initial)](std::stop_token stop) {
        backend::RestartPolicy policy(initial, backend::RestartPolicy::Clock::now());
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            wake.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
            if (stop.stop_requested()) break;
            const auto observed = backend::detect_server();
            if (stop.stop_requested()) break;
            if (policy.observe(observed, backend::RestartPolicy::Clock::now())) {
                requested_.store(true, std::memory_order_release);
                std::string error;
                platform::notify_control("BACKEND-RESTART", error);
                // Retry notifications if accept() was temporarily busy.
            }
        }
    })
{
}

} // namespace shatter::control
