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
    CHECK(content.starts_with("SHATTER-CONFIG 3\n"));
    CHECK(content.find("D \"shatter.output.1\" \"device:serial:headphones-2\" \"Studio Headphones\"") != std::string::npos);
    CHECK(content.find("M \"device:serial:mic-7\"") != std::string::npos);
}
