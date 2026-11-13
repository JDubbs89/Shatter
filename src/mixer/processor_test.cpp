#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "mixer/processor.hpp"

namespace shatter::mixer {
namespace {

class ProcessorTest : public testing::Test {
protected:
    std::array<AppChannel, 1> apps;
    std::array<PlaybackStream, 2> streams;
    std::array<CaptureChannel, 1> captures;
    std::array<OutputChannel, 2> outputs;
    std::array<PlaybackStream*, 2> stream_views{&streams[0], &streams[1]};
    std::array<CaptureChannel*, 1> capture_views{&captures[0]};
    std::array<OutputChannel*, 2> output_views{&outputs[0], &outputs[1]};
    std::atomic<std::size_t> allocated_channels{1};
    std::atomic<std::size_t> allocated_streams{2};
    RuntimeProfiler profiler;
    XrunCounters xruns;
    std::array<std::atomic<std::uint64_t>, 2> ring_frames{};
    std::atomic<bool> fade_requested{false};
    Processor processor{{apps, stream_views, output_views, capture_views,
                         allocated_channels, allocated_streams, profiler, xruns,
                         ring_frames, fade_requested}};

    void SetUp() override
    {
        apps[0].allocated = true;
        apps[0].output_mask.store(3);
        apps[0].smoothed_gain.fill(1.0F);
        for (auto& stream : streams) {
            stream.in_use = true;
            stream.active.store(true);
            stream.smoothed_send_gain.fill(1.0F);
        }
        for (auto& output : outputs) output.linked.store(true);
    }

    void feed(PlaybackStream& stream, const std::array<float, 8>& samples)
    {
        RealtimeAccessGate::Guard guard(stream.process_gate);
        RuntimeProfiler::Scope profile(profiler, RuntimeProfiler::Callback::input);
        processor.process_playback(stream, samples.data(), samples.size() / channels,
                                   processor.output_generations(), profile);
    }

    std::array<float, 8> render(std::size_t output)
    {
        std::array<float, 8> samples{};
        RealtimeAccessGate::Guard guard(outputs[output].process_gate);
        RuntimeProfiler::Scope profile(profiler, RuntimeProfiler::Callback::output);
        processor.process_output(output, samples.data(), samples.size() / channels, profile);
        return samples;
    }
};

TEST_F(ProcessorTest, PlaybackAndCaptureMixIndependentlyIntoEachOutput)
{
    // Two streams sum to 0.75, app gain gives 0.375; capture adds 0.125.
    constexpr std::array<float, 8> first{1, 1, 1, 1, 1, 1, 1, 1};
    constexpr std::array<float, 8> second{0.25F, 0.25F, 0.25F, 0.25F,
                                          0.25F, 0.25F, 0.25F, 0.25F};
    streams[0].target_gain.store(0.5F);
    streams[0].smoothed_gain = 0.5F;
    apps[0].target_gain.store(0.5F);
    apps[0].smoothed_gain.fill(0.5F);
    captures[0].in_use.store(true);
    captures[0].output_mask.store(3);
    captures[0].target_gain.store(0.5F);
    captures[0].smoothed_gain = 0.5F;
    outputs[0].target_gain.store(0.5F);
    outputs[0].smoothed_gain = 0.5F;

    feed(streams[0], first);
    feed(streams[1], second);
    {
        RealtimeAccessGate::Guard guard(captures[0].process_gate);
        RuntimeProfiler::Scope profile(profiler, RuntimeProfiler::Callback::input);
        processor.process_capture(captures[0], second.data(), second.size() / channels,
                                  processor.output_generations(), profile);
    }
    const auto first_output = render(0);
    const auto second_output = render(1);

    for (const auto sample : first_output) EXPECT_FLOAT_EQ(sample, 0.25F);
    for (const auto sample : second_output) EXPECT_FLOAT_EQ(sample, 0.5F);
    EXPECT_EQ(xruns.snapshot().playback_underrun_frames, 0);
}

TEST_F(ProcessorTest, ExplicitStreamSendRetainsStreamAndAppGain)
{
    constexpr std::array<float, 8> input{1, 1, 1, 1, 1, 1, 1, 1};
    allocated_streams.store(1);
    apps[0].output_mask.store(1);
    apps[0].target_gain.store(0.25F);
    apps[0].smoothed_gain.fill(0.25F);
    streams[0].output_mask.store(2);
    streams[0].target_gain.store(0.5F);
    streams[0].smoothed_gain = 0.5F;
    streams[0].smoothed_send_gain.fill(0.5F);

    feed(streams[0], input);
    const auto app_send = render(0);
    const auto stream_send = render(1);

    for (const auto sample : app_send) EXPECT_FLOAT_EQ(sample, 0.125F);
    for (const auto sample : stream_send) EXPECT_FLOAT_EQ(sample, 0.125F);
}

TEST_F(ProcessorTest, MutingAppRampsToSilenceAndPersistsAcrossBuffers)
{
    constexpr std::array<float, 8> input{1, 1, 1, 1, 1, 1, 1, 1};
    constexpr std::array<float, 8> expected{1, 1, 0.75F, 0.75F, 0.5F, 0.5F, 0.25F, 0.25F};
    allocated_streams.store(1);
    apps[0].muted.store(true);

    feed(streams[0], input);
    const auto transition = render(0);
    feed(streams[0], input);
    const auto muted = render(0);

    EXPECT_EQ(transition, expected);
    for (const auto sample : muted) EXPECT_FLOAT_EQ(sample, 0.0F);
}

TEST_F(ProcessorTest, InputAcquiredBeforeOutputReuseIsDiscarded)
{
    constexpr std::array<float, 8> input{1, 1, 1, 1, 1, 1, 1, 1};
    allocated_streams.store(1);
    const auto generations = processor.output_generations();
    outputs[0].input_generation.fetch_add(1);

    {
        RealtimeAccessGate::Guard guard(streams[0].process_gate);
        RuntimeProfiler::Scope profile(profiler, RuntimeProfiler::Callback::input);
        processor.process_playback(streams[0], input.data(), input.size() / channels,
                                   generations, profile);
    }
    const auto reused_output = render(0);
    const auto unaffected_output = render(1);

    for (const auto sample : reused_output) EXPECT_FLOAT_EQ(sample, 0.0F);
    for (const auto sample : unaffected_output) EXPECT_FLOAT_EQ(sample, 1.0F);
}

TEST_F(ProcessorTest, ShutdownFadeContinuesAcrossBuffers)
{
    constexpr std::array<float, 8> input{1, 1, 1, 1, 1, 1, 1, 1};
    allocated_streams.store(1);
    fade_requested.store(true);

    feed(streams[0], input);
    const auto first = render(0);
    feed(streams[0], input);
    const auto second = render(0);

    EXPECT_FLOAT_EQ(first[0], 1.0F);
    EXPECT_LT(second[0], first[6]);
    EXPECT_EQ(outputs[0].shutdown_fade_frames_remaining, sample_rate / 4 - 8);

}

} // namespace
} // namespace shatter::mixer
