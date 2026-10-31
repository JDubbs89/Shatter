#include "shatter/platform/platform.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <cstdlib>
#include <sys/stat.h>
#endif

int main()
{
#if !defined(_WIN32)
    const auto runtime_dir = std::filesystem::temp_directory_path() /
        ("shatter-platform-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(runtime_dir);
    std::filesystem::permissions(runtime_dir, std::filesystem::perms::owner_all);
    if (setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1) != 0) {
        std::cerr << "could not set a test runtime directory\n";
        return 1;
    }
#endif

    std::string error;
    auto server = shatter::platform::listen_for_control(error);
    if (!server) {
#if defined(_WIN32)
        if (shatter::platform::request_control("PING").starts_with("OK running")) return 0;
#endif
        std::cerr << "could not create control listener: " << error << '\n';
        return 1;
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

    const std::string response = shatter::platform::request_control("PING");
    worker.join();
    if (response != "OK platform transport passed\n") {
        std::cerr << "unexpected control response: " << response << '\n';
        return 1;
    }
    if (!server_error.empty()) {
        std::cerr << "control transport failed: " << server_error << '\n';
        return 1;
    }
    if (shatter::platform::terminal_width() == 0) {
        std::cerr << "platform reported a zero terminal width\n";
        return 1;
    }

#if !defined(_WIN32)
    server.reset();
    std::filesystem::remove_all(runtime_dir);
#endif
    return 0;
}
