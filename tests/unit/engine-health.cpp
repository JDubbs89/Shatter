#include "shatter/engine/health.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("engine health distinguishes failed-open, degraded, and running")
{
    using shatter::engine::HealthState;
    using shatter::engine::health_state;

    CHECK(health_state(false, false, 0) == HealthState::failed_open);
    CHECK(health_state(true, false, 0) == HealthState::degraded);
    CHECK(health_state(true, true, 2) == HealthState::degraded);
    CHECK(health_state(true, true, 0) == HealthState::running);
}

TEST_CASE("xrun counters report events and affected frames")
{
    shatter::engine::XrunCounters counters;
    CHECK(counters.snapshot().input_overrun_events == 0);

    counters.record_input_overrun(16);
    counters.record_input_overrun(0);
    counters.record_playback_underrun(32);
    counters.record_playback_underrun(8);

    const auto xruns = counters.snapshot();
    CHECK(xruns.input_overrun_events == 1);
    CHECK(xruns.input_overrun_frames == 16);
    CHECK(xruns.playback_underrun_events == 2);
    CHECK(xruns.playback_underrun_frames == 40);
}

TEST_CASE("health state has stable user-facing names")
{
    using shatter::engine::HealthState;
    using shatter::engine::health_state_name;

    CHECK(std::string(health_state_name(HealthState::running)) == "running");
    CHECK(std::string(health_state_name(HealthState::degraded)) == "degraded");
    CHECK(std::string(health_state_name(HealthState::failed_open)) == "failed-open");
}
