#include "shatter/api/server.hpp"
#include "shatter/engine/audio_backend.hpp"
#include "shatter/platform/platform.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace shatter::api {
namespace {

void trace_output_command(std::string_view stage, std::string_view detail)
{
#if defined(_WIN32)
    (void)stage;
    (void)detail;
#else
    if (std::getenv("SHATTER_TRACE_OUTPUT") == nullptr) return;
    const char* path = std::getenv("SHATTER_TRACE_OUTPUT_FILE");
    if (path == nullptr || *path == '\0') path = "/tmp/shatter-output-trace.log";
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    dprintf(fd, "[output-trace] pid=%ld stage=%.*s detail=%.*s\n", static_cast<long>(getpid()),
            static_cast<int>(stage.size()), stage.data(), static_cast<int>(detail.size()), detail.data());
    ::close(fd);
#endif
}

} // namespace

std::string request(std::string_view command)
{
    return shatter::platform::request_control(command);
}

int run_server()
{
    std::string transport_error;
    auto control_server = shatter::platform::listen_for_control(transport_error);
    if (!control_server) {
        std::cerr << "Shatter control transport: " << transport_error << '\n';
        return 1;
    }

    auto audio_backend = shatter::engine::create_audio_backend();
    auto& audio_engine = *audio_backend;
    std::string audio_error;
    audio_engine.start(audio_error);
    std::string startup_show = "default";
    std::string default_config_error;
    const bool startup_show_loaded = audio_engine.load_default_config(startup_show, default_config_error);

    bool running = true;
    while (running) {
        auto connection = control_server->accept(transport_error);
        if (!connection) {
            if (!transport_error.empty())
                std::cerr << "Shatter control transport stopped: " << transport_error << '\n';
            break;
        }
        std::string command;
        const bool received = connection->receive(command, transport_error);
        std::string reply = "ERR invalid command\n";
        if (received) {
            const std::string_view input(command);
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
            } else if (input == "HEALTH") {
                reply = audio_engine.health_status();
                if (!audio_engine.available() && !audio_error.empty())
                    reply += "Backend open error: " + audio_error + "\n";
            } else if (input == "STOP") {
                std::string save_error;
                if (!startup_show_loaded)
                    reply = "OK stopping\n";
                else if (!audio_engine.save_config(startup_show, save_error))
                    reply = "ERR could not save default show '" + startup_show + "': " + save_error + "\n";
                else reply = "OK stopping\n";
                audio_engine.begin_shutdown_fade();
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                audio_engine.pause_streams_for_shutdown();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
            } else if (input == "METERS") {
                reply = audio_engine.list_meters();
            } else if (input.starts_with("METERS ")) {
                std::istringstream parsed{std::string(input.substr(7))};
                std::string selector, extra;
                if (!(parsed >> std::quoted(selector)) || selector.empty() || (parsed >> extra))
                    reply = "ERR expected METERS [channel selector]\n";
                else reply = audio_engine.list_meters(selector);
            } else if (input == "FADER-STATUS") {
                reply = audio_engine.fader_status();
            } else if (input.starts_with("FADER-SET ") || input.starts_with("FADER-INC ")) {
                std::istringstream parsed{std::string(input)};
                std::string action, scope, selector, amount, extra;
                if (!(parsed >> action >> scope >> std::quoted(selector) >> amount) || (parsed >> extra)) {
                    reply = "ERR expected FADER-SET|FADER-INC <scope> <channel> <dB>\n";
                } else {
                    const bool relative = action == "FADER-INC";
                    const bool minimum = !relative && amount == "-inf";
                    float decibels = 0.0F;
                    const auto [end, parse_error] = std::from_chars(amount.data(), amount.data() + amount.size(), decibels);
                    std::string fader_error;
                    if (!minimum && (parse_error != std::errc{} || end != amount.data() + amount.size() ||
                                     !std::isfinite(decibels))) {
                        reply = relative ? "ERR fader increment must be a finite number of dB\n"
                                         : "ERR fader level must be a finite number of dB or -inf\n";
                    } else if (!(relative
                            ? audio_engine.adjust_fader_db(scope, selector, decibels, false, fader_error)
                            : audio_engine.set_fader_db(scope, selector, decibels, minimum, fader_error))) {
                        reply = "ERR " + fader_error + "\n";
                    } else {
                        reply = "OK " + scope + (relative ? " fader incremented\n" : " fader set\n");
                    }
                }
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
            } else if (input.starts_with("OUTPUT-DEVICE-") || input.starts_with("OUTPUT-SINK-")) {
                std::istringstream parsed{std::string(input)};
                std::string action, output, device, extra, output_error;
                const bool parsed_request = static_cast<bool>(parsed >> action >> std::quoted(output) >> std::quoted(device));
                const bool assign = action == "OUTPUT-DEVICE-ASSIGN" || action == "OUTPUT-SINK-ASSIGN";
                const bool unassign = action == "OUTPUT-DEVICE-UNASSIGN" || action == "OUTPUT-SINK-UNASSIGN";
                if (!parsed_request || (parsed >> extra) || (!assign && !unassign))
                    reply = "ERR expected OUTPUT-DEVICE <output> <device>\n";
                else if (audio_engine.assign_output_destination(output, device, assign, output_error))
                    reply = std::string("OK output destination ") + (assign ? "assigned\n" : "unassigned\n");
                else reply = "ERR " + output_error + "\n";
            } else if (input.starts_with("APP-OUTPUT-")) {
                std::istringstream parsed{std::string(input)};
                std::string action, app, output, extra, output_error;
                if (!(parsed >> action >> std::quoted(app) >> std::quoted(output)) || (parsed >> extra)) reply = "ERR expected APP-OUTPUT <app> <output>\n";
                else if (audio_engine.assign_app_output(app, output, action == "APP-OUTPUT-ASSIGN", output_error))
                    reply = std::string("OK app channel output ") + (action == "APP-OUTPUT-ASSIGN" ? "assigned\n" : "unassigned\n");
                else reply = "ERR " + output_error + "\n";
            } else if (input.starts_with("INPUT-OUTPUT-")) {
                std::istringstream parsed{std::string(input)};
                std::string action, scope, selector, output, extra, output_error;
                if (!(parsed >> action >> std::quoted(scope) >> std::quoted(selector) >> std::quoted(output)) || (parsed >> extra))
                    reply = "ERR expected INPUT-OUTPUT <scope> <input> <output>\n";
                else if (audio_engine.assign_input_output(scope, selector, output,
                             action == "INPUT-OUTPUT-ASSIGN", output_error))
                    reply = "OK input channel output send updated\n";
                else reply = "ERR " + output_error + "\n";
            } else if (input.starts_with("CAPTURE-SOURCE ")) {
                std::string value(input.substr(std::string_view("CAPTURE-SOURCE ").size()));
                std::string capture_error;
                if (audio_engine.set_capture_source(value, capture_error)) {
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
                    reply = "ERR expected GAIN <app|stream|output> <name-or-id> <decibels>\n";
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
                } else if (scope == "OUTPUT") {
                    std::string gain_error;
                    if (audio_engine.set_output_gain_db(identifier, decibels, gain_error)) {
                        reply = "OK output channel " + identifier + " gain set to " + std::to_string(decibels) + " dB\n";
                    } else {
                        reply = "ERR " + gain_error + "\n";
                    }
                } else {
                    reply = "ERR gain scope must be app, stream, or output\n";
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
                    reply = "ERR expected MUTE <app|stream|capture|output> <id> <on|off>\n";
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
                } else if (scope == "CAPTURE") {
                    std::string mute_error;
                    muted = value == "on";
                    if (identifier != "capture") {
                        reply = "ERR capture input selector must be 'capture'\n";
                    } else if (audio_engine.set_capture_muted(muted, mute_error)) {
                        reply = std::string("OK capture channel ") + (muted ? "muted\n" : "unmuted\n");
                    } else {
                        reply = "ERR " + mute_error + "\n";
                    }
                } else if (scope == "OUTPUT") {
                    std::string mute_error;
                    muted = value == "on";
                    if (audio_engine.set_output_muted(identifier, muted, mute_error)) {
                        reply = "OK output channel " + identifier + (muted ? " muted\n" : " unmuted\n");
                    } else {
                        reply = "ERR " + mute_error + "\n";
                    }
                } else {
                    reply = "ERR mute scope must be app, stream, capture, or output\n";
                }
            }
        }
        if (received && (command.starts_with("OUTPUT-") || command.starts_with("APP-OUTPUT-")))
            trace_output_command("server.reply", reply);
        reply += ".\n";
        std::string send_error;
        connection->send(reply, send_error);
    }
    return running ? 1 : 0;
}


} // namespace shatter::api
