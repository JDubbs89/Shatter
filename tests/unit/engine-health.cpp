#include "mixer/health.hpp"
#include "mixer/identifiers.hpp"

#include <gtest/gtest.h>

#include <string>

TEST(EngineHealthTest, DistinguishesFailedOpenDegradedAndRunning)
{
    using shatter::mixer::HealthState;
    using shatter::mixer::health_state;

    EXPECT_EQ(health_state(false, false, 0), HealthState::failed_open);
    EXPECT_EQ(health_state(true, false, 0), HealthState::degraded);
    EXPECT_EQ(health_state(true, true, 2), HealthState::degraded);
    EXPECT_EQ(health_state(true, true, 0), HealthState::running);
}

TEST(EngineHealthTest, XrunCountersReportEventsAndAffectedFrames)
{
    shatter::mixer::XrunCounters counters;
    EXPECT_EQ(counters.snapshot().input_overrun_events, 0);

    counters.record_input_overrun(16);
    counters.record_input_overrun(0);
    counters.record_playback_underrun(32);
    counters.record_playback_underrun(8);

    const auto xruns = counters.snapshot();
    EXPECT_EQ(xruns.input_overrun_events, 1);
    EXPECT_EQ(xruns.input_overrun_frames, 16);
    EXPECT_EQ(xruns.playback_underrun_events, 2);
    EXPECT_EQ(xruns.playback_underrun_frames, 40);
}

TEST(EngineHealthTest, HealthStateHasStableUserFacingNames)
{
    using shatter::mixer::HealthState;
    using shatter::mixer::health_state_name;

    EXPECT_EQ(std::string(health_state_name(HealthState::running)), "running");
    EXPECT_EQ(std::string(health_state_name(HealthState::degraded)), "degraded");
    EXPECT_EQ(std::string(health_state_name(HealthState::failed_open)), "failed-open");
}

TEST(EngineHealthTest, ApplicationInputCaptureIdentitiesIncludeApplicationAndNode)
{
    using shatter::mixer::stable_application_input_identity;

    const auto discord = stable_application_input_identity(
        "com.discordapp.Discord", "discord", "Discord", "Discord Microphone Input Capture");
    EXPECT_EQ(discord, "app-input:com.discordapp.Discord:Discord Microphone Input Capture");
    EXPECT_NE(discord, stable_application_input_identity(
        "org.example.game", "game", "Game", "Discord Microphone Input Capture"));
    EXPECT_EQ(stable_application_input_identity({}, {}, {}, "capture"), "");
}
