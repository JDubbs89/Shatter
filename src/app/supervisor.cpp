#include "shatter/app/supervisor.hpp"
#include "shatter/engine/pipewire_engine.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace shatter::app {
namespace {

constexpr std::string_view kUsage =
    "Usage: shatter start [engine|profile]\n"
    "       shatter status [engine]\n"
    "       shatter stop [engine|profile]\n"
    "       shatter list <in|out|all>\n"
    "       shatter set gain <app|stream> <name-or-id> <decibels>\n"
    "       shatter set gain capture <decibels>\n"
    "       shatter set mute <app|stream> <name-or-id> <on|off>\n"
    "       shatter set capture <source|send> <value>\n"
    "       shatter rename <app|stream> <old-name> <new-name>\n"
    "       shatter make out <name>\n"
    "       shatter delete out <name-or-pw-node-id>\n"
    "       shatter assign out <output-name-or-id> <sink-name-or-id>\n"
    "       shatter unassign out <output-name-or-id> <sink-name-or-id>\n"
    "       shatter assign app <app-name-or-id> <output-name-or-id>\n"
    "       shatter unassign app <app-name-or-id> <output-name-or-id>\n"
    "       shatter save show <name>\n"
    "       shatter load show <name>\n"
    "       shatter help\n";
constexpr std::string_view kEngineArgument = "--engine";
constexpr std::string_view kStartupBanner = R"( ______     __  __     ______     ______   ______   ______     ______
╱╲  ___╲   ╱╲ ╲_╲ ╲   ╱╲  __ ╲   ╱╲__  _╲ ╱╲__  _╲ ╱╲  ___╲   ╱╲  == ╲
╲ ╲___  ╲  ╲ ╲  __ ╲  ╲ ╲  __ ╲  ╲╱_╱╲ ╲╱ ╲╱_╱╲ ╲╱ ╲ ╲  __╲   ╲ ╲  __<
 ╲╱╲_____╲  ╲ ╲_╲ ╲_╲  ╲ ╲_╲ ╲_╲    ╲ ╲_╲    ╲ ╲_╲  ╲ ╲_____╲  ╲ ╲_╲ ╲_╲
  ╲╱_____╱   ╲╱_╱╲╱_╱   ╲╱_╱╲╱_╱     ╲╱_╱     ╲╱_╱   ╲╱_____╱   ╲╱_╱ ╱_╱
                                                                        )";

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

void trace_output_command(std::string_view stage, std::string_view detail)
{
    if (std::getenv("SHATTER_TRACE_OUTPUT") == nullptr) return;
    const char* path = std::getenv("SHATTER_TRACE_OUTPUT_FILE");
    if (path == nullptr || *path == '\0') path = "/tmp/shatter-output-trace.log";
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    dprintf(fd, "[output-trace] pid=%ld stage=%.*s detail=%.*s\n", static_cast<long>(getpid()),
            static_cast<int>(stage.size()), stage.data(), static_cast<int>(detail.size()), detail.data());
    ::close(fd);
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
            std::cout << kStartupBanner << "\nShatter engine started.\n";
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
    std::string default_config_error;
    audio_engine.load_config("default", default_config_error);

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

        std::array<char, 512> command{};
        const ssize_t received = recv(client_fd, command.data(), command.size() - 1, 0);
        std::string reply = "ERR invalid command\n";
        if (received > 0) {
            std::string_view input(command.data(), static_cast<std::size_t>(received));
            if (!input.empty() && input.back() == '\n') {
                input.remove_suffix(1);
            }
            if (input.starts_with("OUTPUT-") || input.starts_with("APP-OUTPUT-"))
                trace_output_command("server.command", input);
            if (input == "PING") {
                if (audio_engine.available()) {
                    reply = "OK running audio-ready\n";
                } else {
                    const std::string reason = audio_error.empty()
                        ? "speaker output is disconnected" : audio_error;
                    reply = "OK running audio-unavailable: " + reason + "\n";
                }
            } else if (input == "STOP") {
                std::string save_error;
                if (!audio_engine.save_config("default", save_error)) {
                    reply = "ERR could not save default configuration: " + save_error + "\n";
                } else reply = "OK stopping\n";
                running = false;
            } else if (input.starts_with("CONFIG-SAVE ") || input.starts_with("CONFIG-LOAD ")) {
                std::istringstream parsed{std::string(input)};
                std::string action, name, extra, config_error;
                parsed >> action >> std::quoted(name);
                const bool valid = !name.empty() && !(parsed >> extra);
                const bool succeeded = valid && (action == "CONFIG-SAVE"
                    ? audio_engine.save_config(name, config_error)
                    : audio_engine.load_config(name, config_error));
                if (!valid) reply = "ERR expected CONFIG-SAVE|CONFIG-LOAD <name>\n";
                else if (!succeeded) reply = "ERR " + config_error + "\n";
                else reply = "OK configuration " + name + (action == "CONFIG-SAVE" ? " saved\n" : " loaded\n");
            } else if (input == "LIST-IN") {
                reply = audio_engine.list_inputs();
            } else if (input.starts_with("LIST-IN ")) {
                std::uint32_t width = 100;
                const auto value = input.substr(8);
                const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), width);
                if (parse_error != std::errc{} || end != value.data() + value.size()) reply = "ERR invalid terminal width\n";
                else reply = audio_engine.list_inputs(width);
            } else if (input.starts_with("LIST-OUT ") || input.starts_with("LIST-ALL ")) {
                const bool all = input.starts_with("LIST-ALL ");
                constexpr std::size_t prefix = 9;
                std::uint32_t width = 100;
                const auto value = input.substr(prefix);
                const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), width);
                if (parse_error != std::errc{} || end != value.data() + value.size()) reply = "ERR invalid terminal width\n";
                else if (all) reply = audio_engine.list_inputs(width) + "\n" + audio_engine.list_outputs(width);
                else reply = audio_engine.list_outputs(width);
            } else if (input == "LIST-OUT") {
                reply = audio_engine.list_outputs();
            } else if (input == "LIST-ALL") {
                reply = audio_engine.list_inputs() + "\n" + audio_engine.list_outputs();
            } else if (input == "PROFILE-START") {
                std::string profile_error;
                if (audio_engine.profile_start(profile_error)) {
                    reply = "OK profiling started\n";
                } else {
                    reply = "ERR " + profile_error + "\n";
                }
            } else if (input == "PROFILE-STOP") {
                std::string profile_error;
                const std::string report = audio_engine.profile_stop(profile_error);
                reply = report.empty() ? "ERR " + profile_error + "\n" : report;
            } else if (input.starts_with("RENAME ")) {
                std::istringstream parsed{std::string(input)};
                std::string action;
                std::string scope;
                std::string old_name;
                std::string new_name;
                std::string extra;
                std::string rename_error;
                if (!(parsed >> action >> scope >> std::quoted(old_name) >> std::quoted(new_name)) ||
                    (parsed >> extra)) {
                    reply = "ERR expected RENAME <app|stream> <old-name> <new-name>\n";
                } else if (scope == "APP" && audio_engine.rename_app(old_name, new_name, rename_error)) {
                    reply = "OK app channel renamed to " + new_name + "\n";
                } else if (scope == "STREAM" && audio_engine.rename_stream(old_name, new_name, rename_error)) {
                    reply = "OK playback stream renamed to " + new_name + "\n";
                } else if (scope != "APP" && scope != "STREAM") {
                    reply = "ERR rename scope must be app or stream\n";
                } else {
                    reply = "ERR " + rename_error + "\n";
                }
            } else if (input.starts_with("OUTPUT-MAKE ")) {
                std::istringstream parsed{std::string(input)};
                std::string action, name, extra, output_error;
                if (!(parsed >> action >> std::quoted(name)) || (parsed >> extra)) reply = "ERR expected OUTPUT-MAKE <name>\n";
                else if (audio_engine.make_output(name, output_error)) reply = "OK output channel created: " + name + "\n";
                else reply = "ERR " + output_error + "\n";
            } else if (input.starts_with("OUTPUT-DELETE ")) {
                std::istringstream parsed{std::string(input)};
                std::string action, selector, extra, output_error;
                if (!(parsed >> action >> std::quoted(selector)) || (parsed >> extra)) reply = "ERR expected OUTPUT-DELETE <name-or-id>\n";
                else if (audio_engine.delete_output(selector, output_error)) reply = "OK output channel deleted: " + selector + "\n";
                else reply = "ERR " + output_error + "\n";
            } else if (input.starts_with("OUTPUT-SINK-")) {
                std::istringstream parsed{std::string(input)};
                std::string action, output, sink, extra, output_error;
                if (!(parsed >> action >> std::quoted(output) >> std::quoted(sink)) || (parsed >> extra)) reply = "ERR expected OUTPUT-SINK <output> <sink>\n";
                else if (audio_engine.assign_output_sink(output, sink, action == "OUTPUT-SINK-ASSIGN", output_error))
                    reply = std::string("OK output sink ") + (action == "OUTPUT-SINK-ASSIGN" ? "assigned\n" : "unassigned\n");
                else reply = "ERR " + output_error + "\n";
            } else if (input.starts_with("APP-OUTPUT-")) {
                std::istringstream parsed{std::string(input)};
                std::string action, app, output, extra, output_error;
                if (!(parsed >> action >> std::quoted(app) >> std::quoted(output)) || (parsed >> extra)) reply = "ERR expected APP-OUTPUT <app> <output>\n";
                else if (audio_engine.assign_app_output(app, output, action == "APP-OUTPUT-ASSIGN", output_error))
                    reply = std::string("OK app channel output ") + (action == "APP-OUTPUT-ASSIGN" ? "assigned\n" : "unassigned\n");
                else reply = "ERR " + output_error + "\n";
            } else if (input.starts_with("CAPTURE-SOURCE ")) {
                std::string value(input.substr(std::string_view("CAPTURE-SOURCE ").size()));
                std::uint32_t source_id = PW_ID_ANY;
                const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), source_id);
                std::string capture_error;
                if (parse_error != std::errc{} || end != value.data() + value.size()) {
                    reply = "ERR capture source id must be a PipeWire node id\n";
                } else if (audio_engine.set_capture_source(source_id, capture_error)) {
                    reply = "OK capture source set to " + value + "; speaker send off\n";
                } else {
                    reply = "ERR " + capture_error + "\n";
                }
            } else if (input.starts_with("CAPTURE-GAIN ")) {
                std::istringstream parsed{std::string(input)};
                std::string action;
                float decibels = 0.0F;
                std::string extra;
                std::string capture_error;
                if (!(parsed >> action >> decibels) || (parsed >> extra)) {
                    reply = "ERR expected CAPTURE-GAIN <decibels>\n";
                } else if (audio_engine.set_capture_gain_db(decibels, capture_error)) {
                    reply = "OK capture channel gain set to " + std::to_string(decibels) + " dB\n";
                } else {
                    reply = "ERR " + capture_error + "\n";
                }
            } else if (input.starts_with("CAPTURE-SEND ")) {
                const std::string_view value = input.substr(std::string_view("CAPTURE-SEND ").size());
                std::string capture_error;
                if (value != "on" && value != "off") {
                    reply = "ERR expected CAPTURE-SEND <on|off>\n";
                } else if (audio_engine.set_capture_speaker_send(value == "on", capture_error)) {
                    reply = std::string("OK capture speaker send ") + std::string(value) + "\n";
                } else {
                    reply = "ERR " + capture_error + "\n";
                }
            } else if (input.starts_with("GAIN ")) {
                std::istringstream parsed{std::string(input)};
                std::string action;
                std::string scope;
                std::string identifier;
                float decibels = 0.0F;
                std::string extra;
                if (!(parsed >> action >> scope >> std::quoted(identifier) >> decibels) || (parsed >> extra)) {
                    reply = "ERR expected GAIN <app|stream> <name-or-id> <decibels>\n";
                } else if (scope == "APP") {
                    std::string gain_error;
                    if (audio_engine.set_app_gain_db(identifier, decibels, gain_error)) {
                        reply = "OK app channel " + identifier + " gain set to " + std::to_string(decibels) + " dB\n";
                    } else {
                        reply = "ERR " + gain_error + "\n";
                    }
                } else if (scope == "STREAM") {
                    std::string gain_error;
                    if (audio_engine.set_stream_gain_db(identifier, decibels, gain_error)) {
                        reply = "OK playback stream " + identifier + " gain set to " + std::to_string(decibels) + " dB\n";
                    } else {
                        reply = "ERR " + gain_error + "\n";
                    }
                } else {
                    reply = "ERR gain scope must be app or stream\n";
                }
            } else if (input.starts_with("MUTE ")) {
                std::istringstream parsed{std::string(input)};
                std::string action;
                std::string scope;
                std::string identifier;
                std::string value;
                std::string extra;
                bool muted = false;
                if (!(parsed >> action >> scope >> std::quoted(identifier) >> value) || (parsed >> extra) ||
                    (value != "on" && value != "off")) {
                    reply = "ERR expected MUTE <app|stream> <id> <on|off>\n";
                } else if (scope == "APP") {
                    std::string mute_error;
                    muted = value == "on";
                    if (audio_engine.set_app_muted(identifier, muted, mute_error)) {
                        reply = "OK app channel " + identifier + (muted ? " muted\n" : " unmuted\n");
                    } else {
                        reply = "ERR " + mute_error + "\n";
                    }
                } else if (scope == "STREAM") {
                    std::string mute_error;
                    muted = value == "on";
                    if (audio_engine.set_stream_muted(identifier, muted, mute_error)) {
                        reply = "OK playback stream " + identifier + (muted ? " muted\n" : " unmuted\n");
                    } else {
                        reply = "ERR " + mute_error + "\n";
                    }
                } else {
                    reply = "ERR mute scope must be app or stream\n";
                }
            }
        }
        if (received > 0 && (std::string_view(command.data()).starts_with("OUTPUT-") ||
                             std::string_view(command.data()).starts_with("APP-OUTPUT-")))
            trace_output_command("server.reply", reply);
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

    // Normalize the action/target CLI to the existing command handlers below.
    // The dashed spellings remain accepted as compatibility aliases.
    std::vector<std::string> normalized{argv[0]};
    const auto arg = [argc, argv](int index) -> std::string_view {
        return index < argc ? std::string_view(argv[index]) : std::string_view{};
    };
    if (arg(1) == "list" && argc == 3) {
        const std::string_view listing = arg(2);
        if (listing == "in") normalized.emplace_back("--list-in");
        else if (listing == "out") normalized.emplace_back("--list-out");
        else if (listing == "all") normalized.emplace_back("--list-all");
    } else if ((arg(1) == "start" || arg(1) == "stop") && argc == 3 && arg(2) == "profile") {
        normalized.emplace_back(arg(1) == "start" ? "--profile-start" : "--profile-stop");
    } else if ((arg(1) == "start" || arg(1) == "stop" || arg(1) == "status") &&
               argc == 3 && arg(2) == "engine") {
        normalized.emplace_back(arg(1));
    } else if (arg(1) == "set" && arg(2) == "gain" && arg(3) == "capture" && argc == 5) {
        normalized.emplace_back("--set-capture-gain");
        normalized.emplace_back(argv[4]);
    } else if (arg(1) == "set" && arg(2) == "gain" && argc == 6) {
        if (arg(3) == "app" || arg(3) == "stream") {
            normalized.emplace_back("--set-gain");
            normalized.emplace_back(argv[3]);
            normalized.emplace_back(argv[4]);
            normalized.emplace_back(argv[5]);
        }
    } else if (arg(1) == "set" && arg(2) == "mute" && argc == 6) {
        normalized.emplace_back("--set-mute");
        normalized.emplace_back(argv[3]);
        normalized.emplace_back(argv[4]);
        normalized.emplace_back(argv[5]);
    } else if (arg(1) == "set" && arg(2) == "capture" && argc == 5) {
        if (arg(3) == "source") {
            normalized.emplace_back("--set-capture-source");
            normalized.emplace_back(argv[4]);
        } else if (arg(3) == "send") {
            normalized.emplace_back("--set-capture-send");
            normalized.emplace_back(argv[4]);
        }
    } else if (arg(1) == "rename" && argc == 5) {
        normalized.emplace_back("--rename");
        normalized.emplace_back(argv[2]);
        normalized.emplace_back(argv[3]);
        normalized.emplace_back(argv[4]);
    } else if (arg(1) == "make" && arg(2) == "out" && argc == 4) {
        normalized.emplace_back("--make-out");
        normalized.emplace_back(argv[3]);
    } else if (arg(1) == "delete" && arg(2) == "out" && argc == 4) {
        normalized.emplace_back("--delete-out");
        normalized.emplace_back(argv[3]);
    } else if ((arg(1) == "assign" || arg(1) == "unassign") && argc == 5 && arg(2) == "out") {
        normalized.emplace_back(arg(1) == "assign" ? "--assign-output" : "--unassign-output");
        normalized.emplace_back(argv[3]);
        normalized.emplace_back(argv[4]);
    } else if ((arg(1) == "assign" || arg(1) == "unassign") && argc == 5 && arg(2) == "app") {
        normalized.emplace_back(arg(1) == "assign" ? "--assign-app-output" : "--unassign-app-output");
        normalized.emplace_back(argv[3]);
        normalized.emplace_back(argv[4]);
    } else if ((arg(1) == "save" || arg(1) == "load") && argc == 4 && arg(2) == "show") {
        normalized.emplace_back(arg(1) == "save" ? "--config-save" : "--config-load");
        normalized.emplace_back(argv[3]);
    }
    if (normalized.size() > 1) {
        std::vector<char*> normalized_argv;
        normalized_argv.reserve(normalized.size());
        for (std::string& value : normalized) normalized_argv.push_back(value.data());
        return run_cli(static_cast<int>(normalized_argv.size()), normalized_argv.data());
    }

    try {
        if (argc == 3 && (std::string_view(argv[1]) == "--make-out" ||
                          std::string_view(argv[1]) == "--delete-out")) {
            std::ostringstream command;
            command << (std::string_view(argv[1]) == "--make-out" ? "OUTPUT-MAKE " : "OUTPUT-DELETE ")
                    << std::quoted(argv[2]);
            const std::string response = request(command.str());
            if (response.empty()) { std::cerr << "Shatter engine is unavailable; run `shatter start` first.\n"; return 1; }
            if (response.starts_with("ERR ")) { std::cerr << response; return 1; }
            std::cout << response;
            return 0;
        }
        if (argc == 4 && (std::string_view(argv[1]) == "--assign-output" ||
                          std::string_view(argv[1]) == "--unassign-output" ||
                          std::string_view(argv[1]) == "--assign-app-output" ||
                          std::string_view(argv[1]) == "--unassign-app-output")) {
            const std::string_view action(argv[1]);
            const bool output_sink = action == "--assign-output" || action == "--unassign-output";
            const bool assigned = action == "--assign-output" || action == "--assign-app-output";
            std::ostringstream command;
            if (output_sink) command << (assigned ? "OUTPUT-SINK-ASSIGN " : "OUTPUT-SINK-UNASSIGN ");
            else command << (assigned ? "APP-OUTPUT-ASSIGN " : "APP-OUTPUT-UNASSIGN ");
            command << std::quoted(argv[2]) << ' ' << std::quoted(argv[3]);
            const std::string response = request(command.str());
            if (response.empty()) { std::cerr << "Shatter engine is unavailable; run `shatter start` first.\n"; return 1; }
            if (response.starts_with("ERR ")) { std::cerr << response; return 1; }
            std::cout << response;
            return 0;
        }
        if (argc == 2) {
            const std::string_view command(argv[1]);
            std::string_view request_command;
            std::string dynamic_command;
            if (command == "--list-in" || command == "--list-out" || command == "--list-all") {
                struct winsize size{};
                const unsigned short width = ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0
                    ? size.ws_col : 100;
                const std::string_view list_command = command == "--list-in" ? "LIST-IN "
                    : command == "--list-out" ? "LIST-OUT " : "LIST-ALL ";
                dynamic_command = std::string(list_command) + std::to_string(width);
                request_command = dynamic_command;
            } else if (command == "--profile-start") {
                request_command = "PROFILE-START";
            } else if (command == "--profile-stop") {
                request_command = "PROFILE-STOP";
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

        if (argc == 3 && (std::string_view(argv[1]) == "--config-save" ||
                          std::string_view(argv[1]) == "--config-load")) {
            std::ostringstream command;
            command << (std::string_view(argv[1]) == "--config-save" ? "CONFIG-SAVE " : "CONFIG-LOAD ")
                    << std::quoted(argv[2]);
            const std::string response = request(command.str());
            if (response.empty()) { std::cerr << "Shatter engine is unavailable; run `shatter start` first.\n"; return 1; }
            if (response.starts_with("ERR ")) { std::cerr << response; return 1; }
            std::cout << response;
            return 0;
        }

        if (argc == 5 && std::string_view(argv[1]) == "--set-gain") {
            const std::string_view scope(argv[2]);
            if (scope != "app" && scope != "stream") {
                std::cerr << "Gain scope must be app or stream.\n";
                return 2;
            }
            float decibels = 0.0F;
            const std::string_view value(argv[4]);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), decibels);
            if (error != std::errc{} || end != value.data() + value.size()) {
                std::cerr << "Gain must be a number in dB.\n";
                return 2;
            }
            std::ostringstream command_builder;
            command_builder << "GAIN " << (scope == "app" ? "APP " : "STREAM ")
                            << std::quoted(argv[3]) << ' ' << std::to_string(decibels);
            const std::string command = command_builder.str();
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

        if (argc == 5 && std::string_view(argv[1]) == "--set-mute") {
            const std::string_view scope(argv[2]);
            const std::string_view value(argv[4]);
            if ((scope != "app" && scope != "stream") || (value != "on" && value != "off")) {
                std::cerr << "Usage: shatter --set-mute <app|stream> <id> <on|off>\n";
                return 2;
            }
            std::ostringstream command_builder;
            command_builder << "MUTE " << (scope == "app" ? "APP " : "STREAM ")
                            << std::quoted(argv[3]) << ' ' << value;
            const std::string command = command_builder.str();
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

        if (argc == 5 && std::string_view(argv[1]) == "--rename") {
            const std::string_view scope(argv[2]);
            if (scope != "app" && scope != "stream") {
                std::cerr << "Rename scope must be app or stream.\n";
                return 2;
            }
            std::ostringstream command_builder;
            command_builder << "RENAME " << (scope == "app" ? "APP " : "STREAM ")
                            << std::quoted(argv[3]) << ' ' << std::quoted(argv[4]);
            const std::string response = request(command_builder.str());
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

        if (argc == 3 && std::string_view(argv[1]) == "--set-capture-source") {
            std::uint32_t source_id = 0;
            const std::string_view value(argv[2]);
            const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), source_id);
            if (parse_error != std::errc{} || end != value.data() + value.size()) {
                std::cerr << "Capture source id must be a PipeWire node id.\n";
                return 2;
            }
            const std::string response = request("CAPTURE-SOURCE " + std::string(value));
            if (response.empty()) {
                std::cerr << "Shatter engine is unavailable; run `shatter start` first.\n";
                return 1;
            }
            if (response.starts_with("ERR ")) { std::cerr << response; return 1; }
            std::cout << response;
            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--set-capture-gain") {
            float decibels = 0.0F;
            const std::string_view value(argv[2]);
            const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), decibels);
            if (parse_error != std::errc{} || end != value.data() + value.size()) {
                std::cerr << "Capture gain must be a number in dB.\n";
                return 2;
            }
            const std::string response = request("CAPTURE-GAIN " + std::to_string(decibels));
            if (response.empty()) { std::cerr << "Shatter engine is unavailable; run `shatter start` first.\n"; return 1; }
            if (response.starts_with("ERR ")) { std::cerr << response; return 1; }
            std::cout << response;
            return 0;
        }

        if (argc == 3 && std::string_view(argv[1]) == "--set-capture-send") {
            const std::string_view value(argv[2]);
            if (value != "on" && value != "off") {
                std::cerr << "Usage: shatter --set-capture-send <on|off>\n";
                return 2;
            }
            const std::string response = request("CAPTURE-SEND " + std::string(value));
            if (response.empty()) { std::cerr << "Shatter engine is unavailable; run `shatter start` first.\n"; return 1; }
            if (response.starts_with("ERR ")) { std::cerr << response; return 1; }
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
