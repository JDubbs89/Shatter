#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "mixer/audio_backend_test_support.hpp"
#include "mixer/engine.hpp"

namespace shatter::mixer {
namespace {
using test_detail::MemoryBackend;

template<class Backend>
concept HasNativeGain = requires(Backend& backend, std::string& error) {
    backend.set_app_gain_db("Music", 0.0F, error);
};
static_assert(!HasNativeGain<MemoryBackend>);
static_assert(MixingBackend<MemoryBackend>);
static_assert(!MixingBackend<test_detail::DiscoveryBackend>);

TEST(EngineTest, VirtualControlsAndDiscoveryUseOneConcreteBackend)
{
    auto native = std::make_unique<MemoryBackend>();
    auto* original = native.get();
    Engine<MemoryBackend> engine(std::move(native));
    IControl& control = engine;
    std::string error;

    ASSERT_TRUE(control.start(error)) << error;
    backend::IAudioBackend& discovery = control.backend();
    std::vector<backend::Endpoint> endpoints;
    ASSERT_TRUE(discovery.enumerate_endpoints(endpoints, error)) << error;
    ASSERT_TRUE(control.set_app_gain_db("Music", -6.0F, error)) << error;

    EXPECT_EQ(&discovery, original);
    EXPECT_EQ(original->starts, 1);
    ASSERT_EQ(endpoints.size(), 1);
    EXPECT_EQ(endpoints[0].id, "memory-output");
    EXPECT_EQ(endpoints[0].kind, backend::EndpointKind::playback_device);
    EXPECT_NEAR(original->apps[0].target_gain.load(), 0.5011872F, 0.000001F);
    control.stop();
    EXPECT_FALSE(discovery.available());
}

TEST(EngineTest, FailedDiscoveryDoesNotReplaceThePreviousSnapshot)
{
    Engine<MemoryBackend> engine;
    std::vector<backend::Endpoint> endpoints{{"previous", "", "", backend::EndpointKind::playback_device, {}, {}}};
    std::string error;

    EXPECT_FALSE(engine.backend().enumerate_endpoints(endpoints, error));

    ASSERT_EQ(endpoints.size(), 1);
    EXPECT_EQ(endpoints[0].id, "previous");
    EXPECT_EQ(error, "not running");
}

TEST(EngineTest, DiscoveryOnlyAdapterNeedsNoMixerOrBufferStubs)
{
    Engine<test_detail::DiscoveryBackend> engine;
    IControl& control = engine;
    std::string error;

    EXPECT_FALSE(control.set_app_gain_db("Music", -6.0F, error));
    EXPECT_EQ(error, "discovery only");
    EXPECT_FALSE(control.make_output("Monitor", false, error));
    EXPECT_EQ(error, "discovery only");
    EXPECT_FALSE(control.load_config("default", error));
    EXPECT_EQ(error, "discovery only");
    control.begin_shutdown_fade();
    control.pause_streams_for_shutdown();
}

TEST(EngineTest, InvalidGainIsRejectedWithoutChangingTheMixerTarget)
{
    constexpr float invalid[] = {-61.0F, 13.0F, std::numeric_limits<float>::infinity(),
                                  std::numeric_limits<float>::quiet_NaN()};
    for (const float value : invalid) {
        SCOPED_TRACE(value);
        auto native = std::make_unique<MemoryBackend>();
        auto* state = native.get();
        Engine<MemoryBackend> engine(std::move(native));
        std::string error;

        EXPECT_FALSE(engine.set_app_gain_db("Music", value, error));

        EXPECT_EQ(error, "gain must be between -60 dB and +12 dB");
        EXPECT_FLOAT_EQ(state->apps[0].target_gain.load(), 1.0F);
    }
}

TEST(EngineTest, FadersMuteAndRenameOperateOnSharedChannelState)
{
    auto native = std::make_unique<MemoryBackend>();
    auto* state = native.get();
    Engine<MemoryBackend> engine(std::move(native));
    std::string error;

    ASSERT_TRUE(engine.rename_app("app-music", "Player", error)) << error;
    ASSERT_TRUE(engine.set_app_muted("Player", true, error)) << error;
    ASSERT_TRUE(engine.set_fader_db("app", "Player", 100.0F, false, error)) << error;
    EXPECT_NEAR(state->apps[0].target_gain.load(), 3.9810717F, 0.000001F);
    ASSERT_TRUE(engine.set_fader_db("app", "Player", 0.0F, true, error)) << error;

    EXPECT_EQ(state->apps[0].name, "Player");
    EXPECT_TRUE(state->apps[0].muted.load());
    EXPECT_FLOAT_EQ(state->apps[0].target_gain.load(), 0.0F);
    EXPECT_NE(engine.fader_status().find("Player"), std::string::npos);
}

} // namespace
} // namespace shatter::mixer
