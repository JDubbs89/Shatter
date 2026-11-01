#include "shatter/engine/dsp.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

bool nearly_equal(float actual, float expected)
{
    return std::abs(actual - expected) < 0.0001F;
}

void require_samples(const float* actual, const float* expected, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index) {
            EXPECT_TRUE(nearly_equal(actual[index], expected[index])) << "sample " << index;
    }
}

} // namespace

TEST(DspTest, PlaybackStreamGainRampsScaleAudio)
{
    std::array<float, 8> playback{1.0F, -1.0F, 1.0F, -1.0F,
                                  1.0F, -1.0F, 1.0F, -1.0F};
    constexpr std::array<float, 8> expected_playback{
        0.5F, -0.5F, 0.625F, -0.625F, 0.75F, -0.75F, 0.875F, -0.875F};
    shatter::engine::dsp::scale_ramp(playback.data(), 4, 2, {0.5F, 0.125F});
    require_samples(playback.data(), expected_playback.data(), playback.size());
}

TEST(DspTest, PlaybackSendsApplyAppAndStreamGainRamps)
{
    constexpr std::array<float, 4> send_input{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> send_output{};
    constexpr std::array<float, 4> expected_send{0.5F, 0.625F, 0.75F, 0.875F};
    shatter::engine::dsp::accumulate_with_gain_ramps(
        send_output.data(), send_input.data(), 4, 1, {0.5F, 0.125F}, {1.0F, 0.0F});
    require_samples(send_output.data(), expected_send.data(), send_output.size());
}

TEST(DspTest, CaptureAudioAccumulatesIntoOutputMix)
{
    constexpr std::array<float, 4> capture_input{0.25F, -0.25F, 0.5F, -0.5F};
    std::array<float, 4> capture_output{1.0F, 1.0F, 1.0F, 1.0F};
    constexpr std::array<float, 4> expected_capture{1.25F, 0.75F, 1.5F, 0.5F};
    shatter::engine::dsp::accumulate_ramp(capture_output.data(), capture_input.data(),
                                         2, 2, {1.0F, 0.0F});
    require_samples(capture_output.data(), expected_capture.data(), capture_output.size());
}

TEST(DspTest, ShutdownFadePreservesGainAndRemainingFrameState)
{
    std::array<float, 4> faded{1.0F, -1.0F, 1.0F, -1.0F};
    float fade_gain = 1.0F;
    std::uint32_t remaining = 4;
    constexpr std::array<float, 4> expected_fade{1.0F, -1.0F, 0.75F, -0.75F};
    shatter::engine::dsp::apply_fade(faded.data(), 2, 2, fade_gain, remaining, 4);
    require_samples(faded.data(), expected_fade.data(), faded.size());
    EXPECT_TRUE(nearly_equal(fade_gain, 0.5F));
    EXPECT_EQ(remaining, 2);
}
