#include "shatter/state/store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() / "shatter-state-store-test")
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

TEST_CASE("version two shows load with their existing device binding meaning", "[state-store]")
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

    shatter::state::Store store(directory.path());
    shatter::state::Show show;
    std::string error;
    REQUIRE(store.load("legacy", show, error));
    REQUIRE(show.outputs.size() == 1);
    REQUIRE(show.outputs[0].devices.size() == 1);
    CHECK(show.outputs[0].devices[0].identity == "serial-42");
    CHECK(show.apps[0].identity == "app-id:org.example.Player");
    CHECK(show.capture_output_identities == std::vector<std::string>{"shatter.output.1"});
}

TEST_CASE("version one shows remain readable without acquiring newer routing state", "[state-store]")
{
    TemporaryDirectory directory;
    {
        std::ofstream file(directory.path() / "legacy-v1.conf");
        file << "SHATTER-CONFIG 1\n"
             << "A \"app-name:org.example.Player\" \"Player\" -3 0\n"
             << "S \"app-name:org.example.Player\" \"Music\" \"Music\" -6 1\n";
    }

    shatter::state::Store store(directory.path());
    shatter::state::Show show;
    std::string error;
    REQUIRE(store.load("legacy-v1", show, error));
    REQUIRE(show.apps.size() == 1);
    REQUIRE(show.streams.size() == 1);
    CHECK_FALSE(show.apps[0].has_output_sends);
    CHECK_FALSE(show.streams[0].has_output_sends);
    CHECK(show.capture_source_identity.empty());
}

TEST_CASE("shows save backend-neutral device and capture identities", "[state-store]")
{
    TemporaryDirectory directory;
    shatter::state::Store store(directory.path());
    shatter::state::Show show;
    show.capture_source_identity = "device:serial:mic-7";
    show.outputs.push_back({1, "shatter.output.1", "Monitor",
        {{"device:serial:headphones-2", "Studio Headphones"}}});
    std::string error;

    REQUIRE(store.save("portable", show, error));
    shatter::state::Show loaded;
    REQUIRE(store.load("portable", loaded, error));
    CHECK(loaded.capture_source_identity == "device:serial:mic-7");
    REQUIRE(loaded.outputs.size() == 1);
    REQUIRE(loaded.outputs[0].devices.size() == 1);
    CHECK(loaded.outputs[0].devices[0].identity == "device:serial:headphones-2");

    std::ifstream file(directory.path() / "portable.conf");
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(content.starts_with("SHATTER-CONFIG 6\n"));
    CHECK(content.find("D \"shatter.output.1\" \"device:serial:headphones-2\" \"Studio Headphones\"") != std::string::npos);
    CHECK(content.find("M \"device:serial:mic-7\"") != std::string::npos);
}

TEST_CASE("show control state round trips for output and capture channels", "[state-store]")
{
    TemporaryDirectory directory;
    shatter::state::Store store(directory.path());
    shatter::state::Show show;
    show.outputs.push_back({2, "shatter.output.2", "Headphones", {}, -9.5F, true});
    show.capture_gain_db = -18.0F;
    show.capture_muted = true;
    show.capture_output_identities = {"shatter.output.2"};
    show.has_capture_sends = true;
    std::string error;

    REQUIRE(store.save("controls", show, error));
    shatter::state::Show loaded;
    REQUIRE(store.load("controls", loaded, error));
    REQUIRE(loaded.outputs.size() == 1);
    CHECK(loaded.outputs[0].gain_db == -9.5F);
    CHECK(loaded.outputs[0].muted);
    CHECK(loaded.capture_gain_db == -18.0F);
    CHECK(loaded.capture_muted);
    CHECK(loaded.capture_output_identities == std::vector<std::string>{"shatter.output.2"});
}

TEST_CASE("version three shows migrate with neutral channel controls", "[state-store]")
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
    shatter::state::Store store(directory.path());
    shatter::state::Show loaded;
    std::string error;
    REQUIRE(store.load("v3", loaded, error));
    REQUIRE(loaded.outputs.size() == 1);
    CHECK(loaded.outputs[0].gain_db == 0.0F);
    CHECK_FALSE(loaded.outputs[0].muted);
    CHECK(loaded.capture_gain_db == 0.0F);
    CHECK_FALSE(loaded.capture_muted);
    CHECK(loaded.capture_source_identity == "device:serial:mic");
}

TEST_CASE("invalid show load leaves the caller's current state untouched", "[state-store]")
{
    TemporaryDirectory directory;
    {
        std::ofstream file(directory.path() / "invalid.conf");
        file << "SHATTER-CONFIG 4\n"
             << "O 1 \"shatter.output.1\" \"Monitor\" 99 0\n";
    }
    shatter::state::Store store(directory.path());
    shatter::state::Show current;
    current.capture_source_identity = "keep-me";
    current.capture_gain_db = -4.0F;
    std::string error;
    CHECK_FALSE(store.load("invalid", current, error));
    CHECK_FALSE(error.empty());
    CHECK(current.capture_source_identity == "keep-me");
    CHECK(current.capture_gain_db == -4.0F);
}
