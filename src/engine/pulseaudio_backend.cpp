#include "shatter/engine/audio_backend.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace shatter::engine {
namespace {

struct CommandResult {
    int status = -1;
    std::string output;
    std::string error;
};

CommandResult run_pactl(const std::vector<std::string>& arguments)
{
    int descriptors[2];
    if (pipe(descriptors) < 0)
        return {-1, {}, "cannot create pactl output pipe: " + std::string(std::strerror(errno))};

    const pid_t child = fork();
    if (child < 0) {
        const std::string error = std::strerror(errno);
        close(descriptors[0]);
        close(descriptors[1]);
        return {-1, {}, "cannot start pactl: " + error};
    }
    if (child == 0) {
        close(descriptors[0]);
        dup2(descriptors[1], STDOUT_FILENO);
        dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[1]);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>("pactl"));
        for (const auto& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(descriptors[1]);
    std::string output;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<std::size_t>(count));
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            const std::string error = std::strerror(errno);
            close(descriptors[0]);
            int status = 0;
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return {-1, {}, "cannot read pactl output: " + error};
        }
    }
    close(descriptors[0]);

    int wait_status = 0;
    while (waitpid(child, &wait_status, 0) < 0) {
        if (errno == EINTR) continue;
        return {-1, {}, "cannot wait for pactl: " + std::string(std::strerror(errno))};
    }
    if (!WIFEXITED(wait_status)) return {-1, std::move(output), "pactl terminated unexpectedly"};
    const int exit_status = WEXITSTATUS(wait_status);
    if (exit_status != 0) {
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
        return {exit_status, std::move(output), "pactl exited with status " + std::to_string(exit_status)};
    }
    return {0, std::move(output), {}};
}

std::string_view trim(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
        value.remove_suffix(1);
    return value;
}

std::string property_value(std::string_view value)
{
    value = trim(value);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return std::string(value);
    value.remove_prefix(1);
    value.remove_suffix(1);
    std::string result;
    result.reserve(value.size());
    bool escaped = false;
    for (char character : value) {
        if (escaped) {
            result.push_back(character);
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else {
            result.push_back(character);
        }
    }
    if (escaped) result.push_back('\\');
    return result;
}

struct PlaybackStream {
    std::string index;
    std::string application;
    std::string process_binary;
};

struct PlaybackSink {
    std::string name;
    std::string description;
};

bool read_playback_streams(std::vector<PlaybackStream>& streams, std::string& error)
{
    const auto result = run_pactl({"list", "sink-inputs"});
    if (result.status != 0) {
        error = "cannot list PulseAudio playback streams: " + result.error +
            (result.output.empty() ? "" : " (" + result.output + ")");
        return false;
    }

    PlaybackStream current;
    bool in_properties = false;
    auto finish = [&] {
        if (!current.index.empty()) streams.push_back(current);
    };
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string_view value = trim(line);
        if (value.starts_with("Sink Input #")) {
            finish();
            current = {};
            current.index = std::string(value.substr(std::string_view("Sink Input #").size()));
            in_properties = false;
        } else if (value == "Properties:") {
            in_properties = true;
        } else if (in_properties) {
            const auto separator = value.find(" = ");
            if (separator == std::string_view::npos) continue;
            const auto key = value.substr(0, separator);
            const std::string property = property_value(value.substr(separator + 3));
            if (key == "application.name") current.application = property;
            else if (key == "application.process.binary") current.process_binary = property;
        }
    }
    finish();
    return true;
}

bool read_playback_sinks(std::vector<PlaybackSink>& sinks, std::string& error)
{
    const auto result = run_pactl({"list", "sinks"});
    if (result.status != 0) {
        error = "cannot list PulseAudio playback sinks: " + result.error +
            (result.output.empty() ? "" : " (" + result.output + ")");
        return false;
    }

    PlaybackSink current;
    auto finish = [&] {
        if (!current.name.empty()) sinks.push_back(current);
    };
    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string_view value = trim(line);
        if (value.starts_with("Sink #")) {
            finish();
            current = {};
        } else if (value.starts_with("Name:")) {
            current.name = std::string(trim(value.substr(5)));
        } else if (value.starts_with("Description:")) {
            current.description = std::string(trim(value.substr(12)));
        }
    }
    finish();
    return true;
}

std::string unsupported_operation()
{
    return "PulseAudio prototype supports listing playback streams and sinks, and routing active app streams to sinks";
}

class PulseAudioBackend final : public AudioBackend {
public:
    bool start(std::string& error) override
    {
        const auto result = run_pactl({"info"});
        if (result.status != 0) {
            error = "PulseAudio server is unavailable: " + result.error +
                (result.output.empty() ? "" : " (" + result.output + ")");
            return false;
        }
        started_ = true;
        return true;
    }

    void stop() override { started_ = false; }

    bool set_app_gain_db(std::string_view, float, std::string& error) override { return unsupported(error); }
    bool set_stream_gain_db(std::string_view, float, std::string& error) override { return unsupported(error); }
    bool set_app_muted(std::string_view, bool, std::string& error) override { return unsupported(error); }
    bool set_stream_muted(std::string_view, bool, std::string& error) override { return unsupported(error); }
    bool rename_app(std::string_view, std::string, std::string& error) override { return unsupported(error); }
    bool rename_stream(std::string_view, std::string, std::string& error) override { return unsupported(error); }
    bool set_capture_source(std::string_view, std::string& error) override { return unsupported(error); }
    bool set_capture_gain_db(float, std::string& error) override { return unsupported(error); }
    bool set_capture_muted(bool, std::string& error) override { return unsupported(error); }
    bool set_fader_db(std::string_view, std::string_view, float, bool, std::string& error) override
    { return unsupported(error); }
    bool adjust_fader_db(std::string_view, std::string_view, float, bool, std::string& error) override
    { return unsupported(error); }
    bool set_capture_speaker_send(bool, std::string& error) override { return unsupported(error); }
    bool set_output_gain_db(std::string_view, float, std::string& error) override { return unsupported(error); }
    bool set_output_muted(std::string_view, bool, std::string& error) override { return unsupported(error); }
    bool make_output(std::string, std::string& error) override { return unsupported(error); }
    bool delete_output(std::string_view, std::string& error) override { return unsupported(error); }
    bool assign_output_destination(std::string_view, std::string_view, bool, std::string& error) override
    { return unsupported(error); }

    bool assign_app_output(std::string_view app_selector, std::string_view sink_selector,
                           bool assigned, std::string& error) override
    {
        std::vector<PlaybackStream> streams;
        if (!read_playback_streams(streams, error)) return false;
        std::vector<PlaybackStream> matches;
        for (const auto& stream : streams) {
            if (stream.application == app_selector || stream.process_binary == app_selector)
                matches.push_back(stream);
        }
        if (matches.empty()) {
            error = "no active PulseAudio playback streams match app selector '" + std::string(app_selector) + "'";
            return false;
        }

        std::string sink_name;
        if (!assigned) {
            const auto result = run_pactl({"get-default-sink"});
            if (result.status != 0) {
                error = "cannot resolve the default PulseAudio sink: " + result.error;
                return false;
            }
            sink_name = std::string(trim(result.output));
        } else {
            std::vector<PlaybackSink> sinks;
            if (!read_playback_sinks(sinks, error)) return false;
            std::vector<PlaybackSink> matched_sinks;
            for (const auto& sink : sinks) {
                if (sink.name == sink_selector || sink.description == sink_selector)
                    matched_sinks.push_back(sink);
            }
            if (matched_sinks.empty()) {
                error = "no PulseAudio sink matches selector '" + std::string(sink_selector) + "'";
                return false;
            }
            if (matched_sinks.size() > 1) {
                error = "PulseAudio sink selector '" + std::string(sink_selector) + "' is ambiguous";
                return false;
            }
            sink_name = matched_sinks.front().name;
        }

        for (const auto& stream : matches) {
            const auto result = run_pactl({"move-sink-input", stream.index, sink_name});
            if (result.status != 0) {
                error = "cannot route PulseAudio stream for '" + std::string(app_selector) + "': " +
                    result.error + (result.output.empty() ? "" : " (" + result.output + ")");
                return false;
            }
        }
        return true;
    }

    bool assign_input_output(std::string_view, std::string_view, std::string_view, bool,
                             std::string& error) override { return unsupported(error); }
    bool load_config(std::string_view, std::string& error) override { return unsupported(error); }
    bool save_config(std::string_view, std::string& error) override { return unsupported(error); }
    bool load_default_config(std::string&, std::string& error) override { return unsupported(error); }

    std::string list_inputs(std::size_t) const override
    {
        std::vector<PlaybackStream> streams;
        std::string error;
        if (!read_playback_streams(streams, error)) return error + "\n";
        std::map<std::string, std::pair<std::string, std::size_t>> apps;
        for (const auto& stream : streams) {
            const std::string selector = stream.process_binary.empty()
                ? stream.application : stream.process_binary;
            auto& app = apps[selector];
            if (app.first.empty()) app.first = stream.application.empty() ? selector : stream.application;
            ++app.second;
        }

        std::ostringstream output;
        output << "PulseAudio playback streams\n";
        output << "App selector | Application | Active streams\n";
        for (const auto& [selector, app] : apps)
            output << selector << " | " << app.first << " | " << app.second << '\n';
        if (apps.empty()) output << "No app streams discovered\n";
        return output.str();
    }

    std::string list_meters(std::string_view) const override
    {
        return "Live peak/RMS meters are unavailable for the PulseAudio backend.\n";
    }

    std::string list_outputs(std::size_t) const override
    {
        std::vector<PlaybackSink> sinks;
        std::string error;
        if (!read_playback_sinks(sinks, error)) return error + "\n";
        const auto default_result = run_pactl({"get-default-sink"});
        const std::string default_sink = default_result.status == 0
            ? std::string(trim(default_result.output)) : std::string{};
        std::ostringstream output;
        output << "PulseAudio playback sinks\n";
        output << "Sink selector | Description | Status\n";
        for (const auto& sink : sinks)
            output << sink.name << " | " << sink.description << " | "
                   << (sink.name == default_sink ? "default" : "available") << '\n';
        if (sinks.empty()) output << "No playback sinks found\n";
        return output.str();
    }

    std::string fader_status() const override { return unsupported_operation() + "\n"; }
    bool profile_start(std::string& error) override { return unsupported(error); }
    std::string profile_stop(std::string& error) override { error = unsupported_operation(); return {}; }
    bool available() const noexcept override { return started_; }
    void begin_shutdown_fade() noexcept override {}
    void pause_streams_for_shutdown() noexcept override {}

private:
    bool unsupported(std::string& error) const
    {
        error = unsupported_operation();
        return false;
    }

    bool started_ = false;
};

} // namespace

std::unique_ptr<AudioBackend> create_audio_backend()
{
    return std::make_unique<PulseAudioBackend>();
}

} // namespace shatter::engine
