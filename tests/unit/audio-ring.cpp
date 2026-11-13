#include "mixer/audio_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

TEST(AudioRingTest, PreservesSamplesAndSilenceFillsUnderflow)
{
    shatter::mixer::AudioRingBuffer ring;
    const std::array<float, 4> input{0.25F, -0.25F, 0.75F, -0.75F};
    std::array<float, 6> output{};

    EXPECT_EQ(ring.write(input.data(), 2), 2);
    EXPECT_EQ(ring.read(output.data(), 3), 2);
    EXPECT_EQ(output, (std::array<float, 6>{0.25F, -0.25F, 0.75F, -0.75F, 0.0F, 0.0F}));
}

TEST(AudioRingTest, BoundsWritesToPreallocatedCapacity)
{
    shatter::mixer::AudioRingBuffer ring;
    std::vector<float> oversized(shatter::mixer::AudioRingBuffer::capacity_frames * 2 + 2, 1.0F);
    EXPECT_EQ(ring.write(oversized.data(), shatter::mixer::AudioRingBuffer::capacity_frames + 1),
              shatter::mixer::AudioRingBuffer::capacity_frames);
}
