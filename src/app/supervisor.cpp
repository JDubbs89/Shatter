#include "shatter/app/supervisor.hpp"
#include "shatter/api/server.hpp"

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

bool is_running()
{
    return shatter::api::request("PING").starts_with("OK running");
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


} // namespace

int run_engine()
{
    try {
        return shatter::api::run_server();
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
            const std::string response = shatter::api::request(command.str());
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
            const std::string response = shatter::api::request(command.str());
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
                const std::string response = shatter::api::request(request_command);
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
            const std::string response = shatter::api::request(command.str());
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
            const std::string response = shatter::api::request(command);
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
            const std::string response = shatter::api::request(command);
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
            const std::string response = shatter::api::request(command_builder.str());
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
            const std::string response = shatter::api::request("CAPTURE-SOURCE " + std::string(value));
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
            const std::string response = shatter::api::request("CAPTURE-GAIN " + std::to_string(decibels));
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
            const std::string response = shatter::api::request("CAPTURE-SEND " + std::string(value));
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
            const std::string response = shatter::api::request("PING");
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
            const std::string response = shatter::api::request("STOP");
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
