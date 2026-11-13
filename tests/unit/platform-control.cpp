#include "control/platform/service.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <cstdlib>
#include <sys/stat.h>
#endif

TEST(PlatformControlTest, ControlTransportRoundTripsAndReportsTerminalWidth)
{
#if !defined(_WIN32)
    const auto runtime_dir = std::filesystem::temp_directory_path() /
        ("shatter-platform-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(runtime_dir);
    std::filesystem::permissions(runtime_dir, std::filesystem::perms::owner_all);
    if (setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1) != 0) {
        FAIL() << "could not set a test runtime directory";
    }
#endif

    std::string error;
    auto server = shatter::control::platform::listen_for_control(error);
    if (!server) {
#if defined(_WIN32)
        EXPECT_TRUE(shatter::control::platform::request_control("PING").starts_with("OK running"));
        return;
#endif
        FAIL() << "could not create control listener: " << error;
    }

    std::string server_error;
    std::thread worker([&] {
        auto connection = server->accept(server_error);
        if (!connection) return;
        std::string message;
        if (!connection->receive(message, server_error)) return;
        if (message != "PING") {
            server_error = "control request did not arrive intact";
            return;
        }
        connection->send("OK platform transport passed\n.\n", server_error);
    });

    const std::string response = shatter::control::platform::request_control("PING");
    worker.join();
    EXPECT_EQ(response, "OK platform transport passed\n");
    EXPECT_TRUE(server_error.empty()) << server_error;
    EXPECT_GT(shatter::control::platform::terminal_width(), 0);

#if !defined(_WIN32)
    server.reset();
    std::filesystem::remove_all(runtime_dir);
#endif
}
