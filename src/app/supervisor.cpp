#include "shatter/app/supervisor.hpp"
#include "shatter/engine/pipewire_engine.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace shatter::app {
namespace {

constexpr std::string_view kUsage =
    "Usage: shatter <start|status|stop|help>\n"
    "       shatter --list-in|--list-out|--list-all\n"
    "       shatter --set-gain <channel> <decibels>\n";
constexpr std::string_view kEngineArgument = "--engine";

std::string prepare_control_socket_path()
{
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_dir == nullptr || *runtime_dir == '\0') {
        throw std::runtime_error("XDG_RUNTIME_DIR is not set");
    }

    const std::string directory = std::string(runtime_dir) + "/shatter";
    const std::string path = directory + "/control.sock";
    if (path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        throw std::runtime_error("XDG_RUNTIME_DIR path is too long for a Unix socket");
    }

    if (mkdir(directory.c_str(), 0700) < 0 && errno != EEXIST) {
        throw std::runtime_error("cannot create runtime directory: " + std::string(std::strerror(errno)));
    }

    struct stat info {};
    if (lstat(directory.c_str(), &info) < 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 0077) != 0) {
        throw std::runtime_error("runtime directory must be a private directory owned by the current user");
    }

    return path;
}

int connect_to_engine(const std::string& path)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::runtime_error("cannot create control socket: " + std::string(std::strerror(errno)));
    }

    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (connect(fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) < 0) {
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }

    return fd;
}

std::string request(std::string_view command)
{
    const int fd = connect_to_engine(prepare_control_socket_path());
    if (fd < 0) {
        return {};
    }

    const timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    const std::string message = std::string(command) + "\n";
    std::size_t sent = 0;
    while (sent < message.size()) {
        const ssize_t result = send(fd, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
        if (result <= 0) {
            close(fd);
            return {};
        }
        sent += static_cast<std::size_t>(result);
    }

    std::string response;
    std::array<char, 4096> buffer{};
    while (response.size() < 65536) {
        const ssize_t received = recv(fd, buffer.data(), buffer.size(), 0);
        if (received <= 0) {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(received));
        if (response.ends_with(".\n")) {
            response.resize(response.size() - 2);
            break;
        }
    }
    close(fd);
    return response;
}

bool is_running()
{
    return request("PING").starts_with("OK running");
}

int start_engine(const char* executable)
{
    if (is_running()) {
        std::cout << "Shatter engine is already running.\n";
        return 0;
    }

    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "Could not start Shatter engine: " << std::strerror(errno) << '\n';
        return 1;
    }
    if (child == 0) {
        if (setsid() < 0) {
            _exit(126);
        }

        const int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) {
                close(null_fd);
            }
        }

        char* const arguments[] = {const_cast<char*>(executable), const_cast<char*>(kEngineArgument.data()), nullptr};
        execvp(executable, arguments);
        _exit(127);
    }

    for (int attempt = 0; attempt < 400; ++attempt) {
        if (is_running()) {
            std::cout << "Shatter engine started.\n";
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::cerr << "Shatter engine did not become ready. Check that XDG_RUNTIME_DIR is available.\n";
    return 1;
}

int serve()
{
    const std::string path = prepare_control_socket_path();
    struct stat existing {};
    if (lstat(path.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) || existing.st_uid != getuid()) {
            std::cerr << "Refusing to replace an unexpected control socket path.\n";
            return 1;
        }

        const int active_fd = connect_to_engine(path);
        if (active_fd >= 0) {
            close(active_fd);
            std::cerr << "A Shatter engine is already listening.\n";
            return 1;
        }
        if (errno != ECONNREFUSED && errno != ENOENT) {
            std::cerr << "Could not verify existing control socket: " << std::strerror(errno) << '\n';
            return 1;
        }
        if (unlink(path.c_str()) < 0) {
            std::cerr << "Could not remove stale control socket: " << std::strerror(errno) << '\n';
            return 1;
        }
    }

    shatter::engine::PipeWireEngine audio_engine;
    std::string audio_error;
    audio_engine.start(audio_error);

    const int server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd < 0) {
        std::cerr << "Could not create control socket: " << std::strerror(errno) << '\n';
        return 1;
    }

    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    if (bind(server_fd, reinterpret_cast<const struct sockaddr*>(&address), sizeof(address)) < 0 ||
        chmod(path.c_str(), 0600) < 0 || listen(server_fd, 8) < 0) {
        std::cerr << "Could not listen on control socket: " << std::strerror(errno) << '\n';
        close(server_fd);
        unlink(path.c_str());
        return 1;
    }

    bool running = true;
    while (running) {
        const int client_fd = accept4(server_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        std::array<char, 64> command{};
        const ssize_t received = recv(client_fd, command.data(), command.size() - 1, 0);
        std::string reply = "ERR invalid command\n";
        if (received > 0) {
            std::string_view input(command.data(), static_cast<std::size_t>(received));
            if (!input.empty() && input.back() == '\n') {
                input.remove_suffix(1);
            }
            if (input == "PING") {
                if (audio_engine.available()) {
                    reply = "OK running audio-ready\n";
                } else {
                    const std::string reason = audio_error.empty()
                        ? "speaker output is disconnected" : audio_error;
                    reply = "OK running audio-unavailable: " + reason + "\n";
                }
            } else if (input == "STOP") {
                reply = "OK stopping\n";
                running = false;
            } else if (input == "LIST-IN") {
                reply = audio_engine.list_inputs();
            } else if (input == "LIST-OUT") {
                reply = audio_engine.list_outputs();
            } else if (input == "LIST-ALL") {
                reply = audio_engine.list_inputs() + "\n" + audio_engine.list_outputs();
            } else if (input.starts_with("GAIN ")) {
                std::istringstream parsed{std::string(input)};
                std::string action;
                std::string channel;
                float decibels = 0.0F;
                std::string extra;
                if (!(parsed >> action >> channel >> decibels) || (parsed >> extra)) {
                    reply = "ERR expected GAIN <channel> <decibels>\n";
                } else if (channel != "app-playback") {
                    reply = "ERR unknown input channel: " + channel + "\n";
                } else {
                    std::string gain_error;
                    if (audio_engine.set_app_gain_db(decibels, gain_error)) {
                        reply = "OK app-playback gain set to " + std::to_string(decibels) + " dB\n";
                    } else {
                        reply = "ERR " + gain_error + "\n";
                    }
                }
            }
        }
        reply += ".\n";
        std::size_t sent = 0;
        while (sent < reply.size()) {
            const ssize_t result = send(client_fd, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
            if (result <= 0) {
                break;
            }
            sent += static_cast<std::size_t>(result);
        }
        close(client_fd);
    }

    close(server_fd);
    unlink(path.c_str());
    return running ? 1 : 0;
}

} // namespace

int run_engine()
{
    try {
        return serve();
    } catch (const std::exception& error) {
        std::cerr << "Shatter engine: " << error.what() << '\n';
        return 1;
    }
}

int run_cli(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == kEngineArgument) {
        return run_engine();
    }
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "help")) {
        std::cout << kUsage;
        return 0;
    }

    try {
        if (argc == 2) {
            const std::string_view command(argv[1]);
            std::string_view request_command;
            if (command == "--list-in") {
                request_command = "LIST-IN";
            } else if (command == "--list-out") {
                request_command = "LIST-OUT";
            } else if (command == "--list-all") {
                request_command = "LIST-ALL";
            }
            if (!request_command.empty()) {
                const std::string response = request(request_command);
                if (response.empty()) {
                    std::cerr << "Shatter engine is unavailable; run `shatter start` first.\n";
                    return 1;
                }
                if (response.starts_with("ERR ")) {
                    std::cerr << response;
                    return 1;
                }
                std::cout << response;
                return 0;
            }
        }

        if (argc == 4 && std::string_view(argv[1]) == "--set-gain") {
            float decibels = 0.0F;
            const std::string_view value(argv[3]);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), decibels);
            if (error != std::errc{} || end != value.data() + value.size()) {
                std::cerr << "Gain must be a number in dB.\n";
                return 2;
            }
            const std::string command = "GAIN " + std::string(argv[2]) + " " + std::to_string(decibels);
            const std::string response = request(command);
            if (response.empty()) {
                std::cerr << "Shatter engine is unavailable; run `shatter start` first.\n";
                return 1;
            }
            if (response.starts_with("ERR ")) {
                std::cerr << response;
                return 1;
            }
            std::cout << response;
            return 0;
        }

        if (argc != 2) {
            std::cerr << kUsage;
            return 2;
        }

        const std::string_view command(argv[1]);
        if (command == "start") {
            return start_engine(argv[0]);
        }
        if (command == "status") {
            const std::string response = request("PING");
            if (response.starts_with("OK running audio-ready")) {
                std::cout << "Shatter engine is running; PipeWire audio is ready.\n";
            } else if (response.starts_with("OK running audio-unavailable:")) {
                std::cout << "Shatter engine is running; " << response.substr(std::string("OK running ").size());
            } else {
                std::cout << "Shatter engine is stopped.\n";
            }
            return 0;
        }
        if (command == "stop") {
            const std::string response = request("STOP");
            if (!response.starts_with("OK stopping")) {
                std::cout << "Shatter engine is already stopped.\n";
                return 0;
            }
            std::cout << "Shatter engine stopped.\n";
            return 0;
        }
    } catch (const std::exception& error) {
        std::cerr << "shatter: " << error.what() << '\n';
        return 1;
    }

    std::cerr << kUsage;
    return 2;
}

} // namespace shatter::app
