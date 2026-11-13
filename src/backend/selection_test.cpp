#include <gtest/gtest.h>

#include <chrono>

#include "backend/selection.hpp"

namespace shatter::backend {
namespace {
using namespace std::chrono_literals;
using Clock = RestartPolicy::Clock;

TEST(BackendSelectionTest, StableServerDoesNotRestartRegardlessOfElapsedTime)
{
    const Server server{Kind::pipewire, "one"};
    const Clock::time_point start{};
    RestartPolicy policy(server, start);
    EXPECT_FALSE(policy.observe(server, start + 1s));
    EXPECT_FALSE(policy.observe(server, start + 60s));
}

TEST(BackendSelectionTest, BackendChangesMustSettleAndRespectMinimumUptime)
{
    const Clock::time_point start{};
    RestartPolicy policy({Kind::pulseaudio, "one"}, start);
    const Server replacement{Kind::pipewire, "two"};
    EXPECT_FALSE(policy.observe(replacement, start + 1s));
    EXPECT_FALSE(policy.observe(replacement, start + 3s));
    EXPECT_TRUE(policy.observe(replacement, start + 5s));
}

TEST(BackendSelectionTest, TransientFailureAndFlappingDoNotRestart)
{
    const Clock::time_point start{};
    const Server original{Kind::pipewire, "one"};
    RestartPolicy policy(original, start);
    EXPECT_FALSE(policy.observe({}, start + 10s));
    EXPECT_FALSE(policy.observe(original, start + 11s));
    EXPECT_FALSE(policy.observe(original, start + 20s));
    EXPECT_FALSE(policy.observe({Kind::pulseaudio, "two"}, start + 21s));
    EXPECT_FALSE(policy.observe({}, start + 22s));
    EXPECT_FALSE(policy.observe(original, start + 23s));
}

TEST(BackendSelectionTest, SameBackendWithNewServerInstanceRequiresRestart)
{
    const Clock::time_point start{};
    RestartPolicy policy({Kind::pipewire, "one"}, start);
    EXPECT_FALSE(policy.observe({Kind::pipewire, "two"}, start + 10s));
    EXPECT_TRUE(policy.observe({Kind::pipewire, "two"}, start + 12s));
}

TEST(BackendSelectionTest, NoServerAndRecoveryBothRequireFreshDaemonState)
{
    const Clock::time_point start{};
    RestartPolicy loss({Kind::pipewire, "one"}, start);
    EXPECT_FALSE(loss.observe({}, start + 10s));
    EXPECT_TRUE(loss.observe({}, start + 12s));
    RestartPolicy recovery({}, start);
    EXPECT_FALSE(recovery.observe({Kind::pulseaudio, "two"}, start + 10s));
    EXPECT_TRUE(recovery.observe({Kind::pulseaudio, "two"}, start + 12s));
}
} // namespace
} // namespace shatter::backend
