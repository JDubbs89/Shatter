#include "mixer/state/store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("shatter-state-store-test-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST(StateStoreTest, VersionTwoShowsLoadWithExistingDeviceBindingMeaning)
{
    TemporaryDirectory directory;
    {
        std::ofstream file(directory.path() / "legacy.conf");
        file << "SHATTER-CONFIG 2\n"
             << "O 1 \"shatter.output.1\" \"Headphones\"\n"
             << "B \"shatter.output.1\" \"serial-42\" \"Studio Headphones\"\n"
             << "A \"app-id:org.example.Player\" \"Player\" -3 0 1 \"shatter.output.1\"\n"
             << "C 1 \"shatter.output.1\"\n";
    }

    shatter::mixer::state::Store store(directory.path());
    shatter::mixer::state::Show show;
    std::string error;
    ASSERT_TRUE(store.load("legacy", show, error)) << error;
    ASSERT_EQ(show.outputs.size(), 1);
    ASSERT_EQ(show.outputs[0].devices.size(), 1);
    EXPECT_EQ(show.outputs[0].devices[0].identity, "serial-42");
    EXPECT_EQ(show.apps[0].identity, "app-id:org.example.Player");
    EXPECT_EQ(show.capture_output_identities, (std::vector<std::string>{"shatter.output.1"}));
}

TEST(StateStoreTest, VersionOneShowsRemainReadableWithoutNewerRoutingState)
{
    TemporaryDirectory directory;
    {
        std::ofstream file(directory.path() / "legacy-v1.conf");
        file << "SHATTER-CONFIG 1\n"
             << "A \"app-name:org.example.Player\" \"Player\" -3 0\n"
             << "S \"app-name:org.example.Player\" \"Music\" \"Music\" -6 1\n";
    }

    shatter::mixer::state::Store store(directory.path());
    shatter::mixer::state::Show show;
    std::string error;
    ASSERT_TRUE(store.load("legacy-v1", show, error)) << error;
    ASSERT_EQ(show.apps.size(), 1);
    ASSERT_EQ(show.streams.size(), 1);
    EXPECT_FALSE(show.apps[0].has_output_sends);
    EXPECT_FALSE(show.streams[0].has_output_sends);
    EXPECT_TRUE(show.capture_source_identity.empty());
}

TEST(StateStoreTest, ShowsSaveBackendNeutralDeviceAndCaptureIdentities)
{
    TemporaryDirectory directory;
    shatter::mixer::state::Store store(directory.path());
    shatter::mixer::state::Show show;
    show.capture_source_identity = "device:serial:mic-7";
    show.outputs.push_back({1, "shatter.output.1", "Monitor",
        {{"device:serial:headphones-2", "Studio Headphones"}}});
    std::string error;

    ASSERT_TRUE(store.save("portable", show, error)) << error;
    shatter::mixer::state::Show loaded;
    ASSERT_TRUE(store.load("portable", loaded, error)) << error;
    EXPECT_EQ(loaded.capture_source_identity, "device:serial:mic-7");
    ASSERT_EQ(loaded.outputs.size(), 1);
    ASSERT_EQ(loaded.outputs[0].devices.size(), 1);
    EXPECT_EQ(loaded.outputs[0].devices[0].identity, "device:serial:headphones-2");

    std::ifstream file(directory.path() / "portable.conf");
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_TRUE(content.starts_with("SHATTER-CONFIG 6\n"));
    EXPECT_NE(content.find("D \"shatter.output.1\" \"device:serial:headphones-2\" \"Studio Headphones\""), std::string::npos);
    EXPECT_NE(content.find("M \"device:serial:mic-7\""), std::string::npos);
}

TEST(StateStoreTest, ShowControlStateRoundTripsForOutputAndCaptureChannels)
{
    TemporaryDirectory directory;
    shatter::mixer::state::Store store(directory.path());
    shatter::mixer::state::Show show;
    show.outputs.push_back({2, "shatter.output.2", "Headphones", {}, -9.5F, true});
    show.capture_gain_db = -18.0F;
    show.capture_muted = true;
    show.capture_output_identities = {"shatter.output.2"};
    show.has_capture_sends = true;
    std::string error;

    ASSERT_TRUE(store.save("controls", show, error)) << error;
    shatter::mixer::state::Show loaded;
    ASSERT_TRUE(store.load("controls", loaded, error)) << error;
    ASSERT_EQ(loaded.outputs.size(), 1);
    EXPECT_EQ(loaded.outputs[0].gain_db, -9.5F);
    EXPECT_TRUE(loaded.outputs[0].muted);
    EXPECT_EQ(loaded.capture_gain_db, -18.0F);
    EXPECT_TRUE(loaded.capture_muted);
    EXPECT_EQ(loaded.capture_output_identities, (std::vector<std::string>{"shatter.output.2"}));
}

TEST(StateStoreTest, VersionThreeShowsMigrateWithNeutralChannelControls)
{
    TemporaryDirectory directory;
    {
        std::ofstream file(directory.path() / "v3.conf");
        file << "SHATTER-CONFIG 3\n"
             << "O 1 \"shatter.output.1\" \"Monitor\"\n"
             << "D \"shatter.output.1\" \"device:serial:42\" \"Monitor Device\"\n"
             << "C 1 \"shatter.output.1\"\n"
             << "M \"device:serial:mic\"\n";
    }
    shatter::mixer::state::Store store(directory.path());
    shatter::mixer::state::Show loaded;
    std::string error;
    ASSERT_TRUE(store.load("v3", loaded, error)) << error;
    ASSERT_EQ(loaded.outputs.size(), 1);
    EXPECT_EQ(loaded.outputs[0].gain_db, 0.0F);
    EXPECT_FALSE(loaded.outputs[0].muted);
    EXPECT_EQ(loaded.capture_gain_db, 0.0F);
    EXPECT_FALSE(loaded.capture_muted);
    EXPECT_EQ(loaded.capture_source_identity, "device:serial:mic");
}

TEST(StateStoreTest, InvalidShowLoadLeavesCallersCurrentStateUntouched)
{
    TemporaryDirectory directory;
    {
        std::ofstream file(directory.path() / "invalid.conf");
        file << "SHATTER-CONFIG 4\n"
             << "O 1 \"shatter.output.1\" \"Monitor\" 99 0\n";
    }
    shatter::mixer::state::Store store(directory.path());
    shatter::mixer::state::Show current;
    current.capture_source_identity = "keep-me";
    current.capture_gain_db = -4.0F;
    std::string error;
    EXPECT_FALSE(store.load("invalid", current, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(current.capture_source_identity, "keep-me");
    EXPECT_EQ(current.capture_gain_db, -4.0F);
}
