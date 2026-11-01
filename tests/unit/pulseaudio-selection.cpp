#include "pulseaudio_types.hpp"

#include <gtest/gtest.h>

TEST(PulseAudioSelectionTest, MatchesApplicationNameOrProcessBinary)
{
    const shatter::engine::PulsePlaybackStream stream{
        .index = 12,
        .sink_index = 4,
        .application = "Music Player",
        .process_binary = "music-player",
    };

    EXPECT_TRUE(shatter::engine::matches_pulse_playback_selector(stream, "Music Player"));
    EXPECT_TRUE(shatter::engine::matches_pulse_playback_selector(stream, "music-player"));
    EXPECT_FALSE(shatter::engine::matches_pulse_playback_selector(stream, "another-player"));
    EXPECT_FALSE(shatter::engine::matches_pulse_playback_selector(stream, ""));
}
