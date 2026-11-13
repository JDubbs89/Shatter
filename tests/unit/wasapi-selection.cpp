#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "backend/wasapi/wasapi_types.hpp"

using namespace shatter::backend;

TEST(WasapiSelectionTest, MatchesPlaybackSessionsByApplicationNameOrExecutable)
{
    const std::vector<WasapiPlaybackSession> sessions = {
        {1, "Music Player", "music-player.exe", "session-1"},
        {2, "Browser", "browser.exe", "session-2"},
    };

    EXPECT_TRUE(matches_wasapi_playback_selector(sessions[0], "Music Player"));
    EXPECT_TRUE(matches_wasapi_playback_selector(sessions[0], "MUSIC-PLAYER.EXE"));
    EXPECT_FALSE(matches_wasapi_playback_selector(sessions[1], "Music Player"));
}

TEST(WasapiSelectionTest, ResolvesEndpointsByStableIdOrUniqueFriendlyName)
{
    const std::vector<WasapiPlaybackEndpoint> endpoints = {
        {"{endpoint-1}", "Studio Output", true},
        {"{endpoint-2}", "Headphones", false},
    };
    std::string resolved;
    std::string error;

    ASSERT_TRUE(resolve_wasapi_endpoint(endpoints, "{endpoint-2}", resolved, error)) << error;
    EXPECT_EQ(resolved, "{endpoint-2}");
    ASSERT_TRUE(resolve_wasapi_endpoint(endpoints, "Studio Output", resolved, error)) << error;
    EXPECT_EQ(resolved, "{endpoint-1}");
    EXPECT_FALSE(resolve_wasapi_endpoint(endpoints, "missing", resolved, error));
    EXPECT_FALSE(error.empty());

    const std::vector<WasapiPlaybackEndpoint> duplicate_names = {
        {"{endpoint-1}", "Speakers", false},
        {"{endpoint-2}", "Speakers", false},
    };
    EXPECT_FALSE(resolve_wasapi_endpoint(duplicate_names, "speakers", resolved, error));
    EXPECT_NE(error.find("ambiguous"), std::string::npos);
}
