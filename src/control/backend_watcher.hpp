#pragma once

#include "backend/selection.hpp"

#include <atomic>
#include <thread>

namespace shatter::control {

// Owns only detection state, never the engine. Shutdown cannot deadlock waiting
// for a response from the control thread that is joining this watcher.
class BackendWatcher {
public:
    explicit BackendWatcher(backend::Server initial);
    bool restart_requested() const noexcept { return requested_.load(std::memory_order_acquire); }
    void stop() noexcept { worker_.request_stop(); }

private:
    std::atomic<bool> requested_{false};
    std::jthread worker_;
};

} // namespace shatter::control
