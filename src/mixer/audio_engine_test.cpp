#include <gtest/gtest.h>

#include <array>
#include <memory>

#include "mixer/audio_backend_test_support.hpp"

namespace shatter::mixer {
namespace {
using test_detail::MemoryBackend;

static_assert(InputBackend<MemoryBackend, PlaybackStream>);
static_assert(InputBackend<MemoryBackend, CaptureChannel>);
static_assert(OutputBackend<MemoryBackend, OutputChannel>);
static_assert(!InputBackend<test_detail::DiscoveryBackend, PlaybackStream>);
static_assert(!OutputBackend<test_detail::DiscoveryBackend, OutputChannel>);

struct ThrowingInputBackend {
    MemoryBackend::InputBuffer acquire_input(PlaybackStream&);
    void release_input(PlaybackStream&, MemoryBackend::InputBuffer) noexcept;
};
static_assert(!InputBackend<ThrowingInputBackend, PlaybackStream>);

TEST(AudioEngineTest, ConcreteAdapterFeedsSharedMixerAndIndependentOutputs)
{
    auto backend = std::make_unique<MemoryBackend>();
    backend->apps[0].target_gain.store(0.5F);
    backend->apps[0].smoothed_gain.fill(0.5F);
    backend->streams[0].target_gain.store(0.5F);
    backend->streams[0].smoothed_gain = 0.5F;
    backend->outputs[0].target_gain.store(0.5F);
    backend->outputs[0].smoothed_gain = 0.5F;

    backend->audio.process_playback(backend->streams[0]);
    backend->audio.process_output(backend->outputs[0]);
    const auto first = backend->output;
    backend->audio.process_output(backend->outputs[1]);
    const auto second = backend->output;

    for (const auto sample : first) EXPECT_FLOAT_EQ(sample, 0.125F);
    for (const auto sample : second) EXPECT_FLOAT_EQ(sample, 0.25F);
    EXPECT_EQ(backend->input_acquisitions, 1);
    EXPECT_EQ(backend->input_releases, 1);
    EXPECT_EQ(backend->output_submissions, 2);
}

TEST(AudioEngineTest, CaptureCallbackUsesTheSameProcessingSeam)
{
    auto backend = std::make_unique<MemoryBackend>();
    backend->apps[0].output_mask.store(0);
    backend->captures[0].in_use.store(true);
    backend->captures[0].output_mask.store(1);
    backend->captures[0].target_gain.store(0.25F);
    backend->captures[0].smoothed_gain = 0.25F;

    backend->audio.process_capture(backend->captures[0]);
    backend->audio.process_output(backend->outputs[0]);

    for (const auto sample : backend->output) EXPECT_FLOAT_EQ(sample, 0.25F);
    EXPECT_EQ(backend->input_releases, 1);
    EXPECT_EQ(backend->output_submissions, 1);
}

TEST(AudioEngineTest, AcquiredInputIsReleasedEvenWhenItsSamplesAreUnusable)
{
    struct Case { bool available; bool valid; int releases; };
    constexpr std::array cases{Case{false, false, 0}, Case{true, false, 1}, Case{true, true, 1}};
    for (const auto& tc : cases) {
        SCOPED_TRACE(testing::Message() << "available=" << tc.available << ", valid=" << tc.valid);
        auto backend = std::make_unique<MemoryBackend>();
        backend->input_available = tc.available;
        backend->input_valid = tc.valid;

        backend->audio.process_playback(backend->streams[0]);

        EXPECT_EQ(backend->input_releases, tc.releases);
        EXPECT_EQ(backend->streams[0].audio[0].available_frames(), tc.available && tc.valid ? 4 : 0);
    }
}

TEST(AudioEngineTest, AcquiredOutputIsSubmittedEvenWhenItsSamplesAreUnusable)
{
    struct Case { bool available; bool valid; int submissions; };
    constexpr std::array cases{Case{false, false, 0}, Case{true, false, 1}, Case{true, true, 1}};
    for (const auto& tc : cases) {
        SCOPED_TRACE(testing::Message() << "available=" << tc.available << ", valid=" << tc.valid);
        auto backend = std::make_unique<MemoryBackend>();
        backend->output.fill(0.5F);
        backend->output_available = tc.available;
        backend->output_valid = tc.valid;

        backend->audio.process_output(backend->outputs[0]);

        EXPECT_EQ(backend->output_submissions, tc.submissions);
        for (const auto sample : backend->output)
            EXPECT_FLOAT_EQ(sample, tc.available && tc.valid ? 0.0F : 0.5F);
    }
}

TEST(AudioEngineTest, ClosedGatesPreventNativeBufferAcquisition)
{
    auto backend = std::make_unique<MemoryBackend>();
    backend->streams[0].process_gate.close();
    backend->captures[0].process_gate.close();
    backend->outputs[0].process_gate.close();

    backend->audio.process_playback(backend->streams[0]);
    backend->audio.process_capture(backend->captures[0]);
    backend->audio.process_output(backend->outputs[0]);

    EXPECT_EQ(backend->input_acquisitions, 0);
    EXPECT_EQ(backend->input_releases, 0);
    EXPECT_EQ(backend->output_acquisitions, 0);
    EXPECT_EQ(backend->output_submissions, 0);
}

TEST(AudioEngineTest, SnapshotPrecedesNativeAcquisitionAndProtectsReusedOutput)
{
    auto backend = std::make_unique<MemoryBackend>();
    backend->reuse_output_on_acquire = true;

    backend->audio.process_playback(backend->streams[0]);
    backend->audio.process_output(backend->outputs[0]);
    const auto reused = backend->output;
    backend->audio.process_output(backend->outputs[1]);
    const auto unaffected = backend->output;

    for (const auto sample : reused) EXPECT_FLOAT_EQ(sample, 0.0F);
    for (const auto sample : unaffected) EXPECT_FLOAT_EQ(sample, 1.0F);
    EXPECT_EQ(backend->input_releases, 1);
}

} // namespace
} // namespace shatter::mixer
