#include "shatter/state/store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace shatter::state {
namespace {

constexpr std::string_view kHeaderV1 = "SHATTER-CONFIG 1";
constexpr std::string_view kHeaderV2 = "SHATTER-CONFIG 2";
constexpr std::string_view kHeaderV3 = "SHATTER-CONFIG 3";
constexpr std::string_view kHeaderV4 = "SHATTER-CONFIG 4";
constexpr std::string_view kHeaderV5 = "SHATTER-CONFIG 5";
constexpr std::string_view kHeader = "SHATTER-CONFIG 6";
constexpr float kMinimumGainDb = -60.0F;
constexpr float kMaximumGainDb = 12.0F;

bool valid_gain(float gain_db)
{
    return std::isfinite(gain_db) && gain_db >= kMinimumGainDb && gain_db <= kMaximumGainDb;
}

} // namespace

Store::Store(std::filesystem::path directory)
    : directory_(std::move(directory))
{
}

std::filesystem::path Store::config_path(std::string_view name) const
{
    std::filesystem::path directory = directory_;
    if (directory.empty()) {
#if defined(_WIN32)
        const char* app_data = std::getenv("APPDATA");
        if (app_data == nullptr || *app_data == '\0')
            throw std::runtime_error("APPDATA is not set; cannot locate Shatter's configuration directory");
        directory = std::filesystem::path(app_data) / "Shatter";
#else
        const char* base = std::getenv("XDG_CONFIG_HOME");
        directory = base && *base ? std::filesystem::path(base) / "shatter"
            : std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".config" / "shatter";
#endif
    }

    std::string filename(name);
    if (filename.ends_with(".conf")) filename.resize(filename.size() - 5);
    if (filename.empty() || filename == "." || filename == ".." ||
        filename.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.\0") != std::string::npos ||
        filename.find('\0') != std::string::npos) {
        throw std::runtime_error("config name may contain only letters, digits, dot, dash, and underscore");
    }
    return directory / (filename + ".conf");
}

std::filesystem::path Store::default_show_path() const
{
    return config_path("default").parent_path() / "default-show";
}

bool Store::has_show(std::string_view name) const
{
    try {
        return std::filesystem::is_regular_file(config_path(name));
    } catch (const std::exception&) {
        return false;
    }
}

bool Store::default_show(std::string& name, std::string& error) const
{
    try {
        const auto path = default_show_path();
        std::ifstream file(path);
        if (!file) {
            if (!std::filesystem::exists(path)) {
                name = "default";
                return true;
            }
            error = "cannot read default show selection";
            return false;
        }
        std::string selected;
        std::string extra;
        if (!std::getline(file, selected) || selected.empty() || std::getline(file, extra)) {
            error = "invalid default show selection";
            return false;
        }
        const auto selected_path = config_path(selected);
        if (!std::filesystem::is_regular_file(selected_path)) {
            error = "default show configuration not found: " + selected;
            return false;
        }
        name = std::move(selected);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool Store::set_default_show(std::string_view name, std::string& error) const
{
    try {
        const auto show_path = config_path(name);
        if (!std::filesystem::is_regular_file(show_path)) {
            error = "configuration not found: " + std::string(name);
            return false;
        }
        const auto path = default_show_path();
        std::filesystem::create_directories(path.parent_path());
        const auto temporary_path = path.string() + ".tmp";
        std::ofstream file(temporary_path, std::ios::trunc);
        if (!file) {
            error = "cannot write default show selection";
            return false;
        }
        file << name << '\n';
        file.close();
        if (!file) {
            error = "failed while writing default show selection";
            return false;
        }
        std::filesystem::rename(temporary_path, path);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool Store::load(std::string_view name, Show& show, std::string& error) const
{
    try {
        std::ifstream file(config_path(name));
        if (!file) {
            error = "configuration not found: " + std::string(name);
            return false;
        }
        std::string header;
        if (!std::getline(file, header) ||
            (header != kHeader && header != kHeaderV5 && header != kHeaderV4 && header != kHeaderV3 && header != kHeaderV2 && header != kHeaderV1)) {
            error = "unsupported or invalid configuration format";
            return false;
        }
        const bool version_two = header == kHeaderV2 || header == kHeaderV3 || header == kHeaderV4 || header == kHeaderV5 || header == kHeader;
        const bool version_three = header == kHeaderV3 || header == kHeaderV4 || header == kHeaderV5 || header == kHeader;
        const bool version_four = header == kHeaderV4 || header == kHeaderV5 || header == kHeader;
        const bool version_five = header == kHeaderV5 || header == kHeader;
        const bool version_six = header == kHeader;

        Show loaded;
        std::map<std::string, std::size_t> outputs_by_identity;
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream row(line);
            char type = 0;
            if (!(row >> type)) continue;
            int muted = 0;
            if (type == 'A') {
                AppSetting item;
                std::size_t send_count = 0;
                if (!(row >> std::quoted(item.identity) >> std::quoted(item.name) >> item.gain_db >> muted) ||
                    (muted != 0 && muted != 1) || !valid_gain(item.gain_db)) {
                    error = "invalid app entry in configuration";
                    return false;
                }
                item.muted = muted != 0;
                if (version_two) {
                    if (!(row >> send_count)) { error = "invalid app send list in configuration"; return false; }
                    item.has_output_sends = true;
                    for (std::size_t i = 0; i < send_count; ++i) {
                        std::string identity;
                        if (!(row >> std::quoted(identity)) || identity.empty()) {
                            error = "invalid app send list in configuration";
                            return false;
                        }
                        item.output_identities.push_back(std::move(identity));
                    }
                }
                loaded.apps.push_back(std::move(item));
            } else if (type == 'S') {
                StreamSetting item;
                std::size_t send_count = 0;
                if (!(row >> std::quoted(item.app_identity) >> std::quoted(item.original_name) >>
                      std::quoted(item.name) >> item.gain_db >> muted) ||
                    (muted != 0 && muted != 1) || !valid_gain(item.gain_db)) {
                    error = "invalid stream entry in configuration";
                    return false;
                }
                item.muted = muted != 0;
                if (version_two) {
                    if (!(row >> send_count)) { error = "invalid stream send list in configuration"; return false; }
                    item.has_output_sends = true;
                    for (std::size_t i = 0; i < send_count; ++i) {
                        std::string identity;
                        if (!(row >> std::quoted(identity)) || identity.empty()) {
                            error = "invalid stream send list in configuration";
                            return false;
                        }
                        item.output_identities.push_back(std::move(identity));
                    }
                }
                loaded.streams.push_back(std::move(item));
            } else if (type == 'O' && version_two) {
                OutputSetting item;
                if (!(row >> item.slot >> std::quoted(item.identity) >> std::quoted(item.name)) ||
                    item.identity.empty() || item.name.empty() || outputs_by_identity.contains(item.identity)) {
                    error = "invalid or duplicate output entry in configuration";
                    return false;
                }
                if (version_four) {
                    if (!(row >> item.gain_db >> muted) || !valid_gain(item.gain_db) ||
                        (muted != 0 && muted != 1)) {
                        error = "invalid output channel controls in configuration";
                        return false;
                    }
                    item.muted = muted != 0;
                }
                if (version_five) {
                    int capture_source = 0;
                    if (!(row >> capture_source) || (capture_source != 0 && capture_source != 1)) {
                        error = "invalid output channel type in configuration";
                        return false;
                    }
                    item.capture_source = capture_source != 0;
                }
                outputs_by_identity[item.identity] = loaded.outputs.size();
                loaded.outputs.push_back(std::move(item));
            } else if ((type == 'B' && header == kHeaderV2) || (type == 'D' && version_three)) {
                std::string output_identity;
                DeviceBinding binding;
                if (!(row >> std::quoted(output_identity) >> std::quoted(binding.identity) >>
                      std::quoted(binding.name)) || binding.name.empty()) {
                    error = "invalid output device binding in configuration";
                    return false;
                }
                const auto output = outputs_by_identity.find(output_identity);
                if (output == outputs_by_identity.end()) {
                    error = "output sink binding references an unknown output";
                    return false;
                }
                loaded.outputs[output->second].devices.push_back(std::move(binding));
            } else if (type == 'C' && version_two) {
                if (loaded.has_capture_sends) { error = "duplicate capture entry in configuration"; return false; }
                std::size_t send_count = 0;
                if (version_four) {
                    if (!(row >> loaded.capture_gain_db >> muted) || !valid_gain(loaded.capture_gain_db) ||
                        (muted != 0 && muted != 1)) {
                        error = "invalid capture channel controls in configuration";
                        return false;
                    }
                    loaded.capture_muted = muted != 0;
                }
                if (!(row >> send_count)) { error = "invalid capture send list in configuration"; return false; }
                loaded.has_capture_sends = true;
                for (std::size_t i = 0; i < send_count; ++i) {
                    std::string identity;
                    if (!(row >> std::quoted(identity)) || identity.empty()) {
                        error = "invalid capture send list in configuration";
                        return false;
                    }
                    loaded.capture_output_identities.push_back(std::move(identity));
                }
            } else if (type == 'M' && version_three) {
                if (!loaded.capture_source_identity.empty() || !(row >> std::quoted(loaded.capture_source_identity)) ||
                    loaded.capture_source_identity.empty()) {
                    error = "invalid capture device identity in configuration";
                    return false;
                }
            } else if (type == 'I' && version_six) {
                CaptureSetting item;
                std::size_t send_count = 0;
                if (!(row >> std::quoted(item.identity) >> std::quoted(item.name) >> item.gain_db >> muted >> send_count) ||
                    item.identity.empty() || item.name.empty() || !valid_gain(item.gain_db) ||
                    (muted != 0 && muted != 1) ||
                    std::any_of(loaded.captures.begin(), loaded.captures.end(), [&](const auto& prior) {
                        return prior.identity == item.identity;
                    })) {
                    error = "invalid or duplicate capture channel entry in configuration";
                    return false;
                }
                item.muted = muted != 0;
                for (std::size_t i = 0; i < send_count; ++i) {
                    std::string identity;
                    if (!(row >> std::quoted(identity)) || identity.empty()) {
                        error = "invalid capture output send list in configuration";
                        return false;
                    }
                    item.output_identities.push_back(std::move(identity));
                }
                loaded.captures.push_back(std::move(item));
            } else {
                error = "unknown entry in configuration";
                return false;
            }
            std::string extra;
            if (row >> extra) {
                error = "extra data in configuration entry";
                return false;
            }
        }
        show = std::move(loaded);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool Store::save(std::string_view name, const Show& show, std::string& error) const
{
    try {
        const auto path = config_path(name);
        std::filesystem::create_directories(path.parent_path());
        const auto temporary_path = path.string() + ".tmp";
        std::ofstream file(temporary_path, std::ios::trunc);
        if (!file) {
            error = "cannot write configuration file";
            return false;
        }
        file << kHeader << '\n' << std::setprecision(7);
        std::set<std::string> output_identities;
        for (const auto& output : show.outputs) {
            if (output.identity.empty() || output.name.empty() ||
                !output_identities.insert(output.identity).second) {
                error = "output identity or name is invalid or duplicated";
                return false;
            }
            if (!valid_gain(output.gain_db)) { error = "output gain is outside the supported range"; return false; }
            file << "O " << output.slot << ' ' << std::quoted(output.identity) << ' '
                 << std::quoted(output.name) << ' ' << output.gain_db << ' ' << (output.muted ? 1 : 0)
                 << ' ' << (output.capture_source ? 1 : 0) << '\n';
            for (const auto& device : output.devices) {
                if (device.name.empty()) { error = "output device name cannot be empty"; return false; }
                file << "D " << std::quoted(output.identity) << ' ' << std::quoted(device.identity)
                     << ' ' << std::quoted(device.name) << '\n';
            }
        }
        for (const auto& app : show.apps) {
            if (!valid_gain(app.gain_db)) {
                error = "app gain is outside the supported range";
                return false;
            }
            file << "A " << std::quoted(app.identity) << ' ' << std::quoted(app.name) << ' '
                 << app.gain_db << ' ' << (app.muted ? 1 : 0) << ' ' << app.output_identities.size();
            for (const auto& identity : app.output_identities) file << ' ' << std::quoted(identity);
            file << '\n';
        }
        for (const auto& stream : show.streams) {
            if (!valid_gain(stream.gain_db)) {
                error = "stream gain is outside the supported range";
                return false;
            }
            file << "S " << std::quoted(stream.app_identity) << ' '
                 << std::quoted(stream.original_name) << ' ' << std::quoted(stream.name) << ' '
                 << stream.gain_db << ' ' << (stream.muted ? 1 : 0) << ' ' << stream.output_identities.size();
            for (const auto& identity : stream.output_identities) file << ' ' << std::quoted(identity);
            file << '\n';
        }
        if (!valid_gain(show.capture_gain_db)) { error = "capture gain is outside the supported range"; return false; }
        file << "C " << show.capture_gain_db << ' ' << (show.capture_muted ? 1 : 0) << ' '
             << show.capture_output_identities.size();
        for (const auto& identity : show.capture_output_identities) file << ' ' << std::quoted(identity);
        file << '\n';
        if (!show.capture_source_identity.empty())
            file << "M " << std::quoted(show.capture_source_identity) << '\n';
        for (const auto& capture : show.captures) {
            if (capture.identity.empty() || capture.name.empty() || !valid_gain(capture.gain_db)) {
                error = "capture channel identity or gain is invalid";
                return false;
            }
            file << "I " << std::quoted(capture.identity) << ' ' << std::quoted(capture.name) << ' '
                 << capture.gain_db << ' '
                 << (capture.muted ? 1 : 0) << ' ' << capture.output_identities.size();
            for (const auto& identity : capture.output_identities) file << ' ' << std::quoted(identity);
            file << '\n';
        }
        file.close();
        if (!file) {
            error = "failed while writing configuration file";
            return false;
        }
        std::filesystem::rename(temporary_path, path);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace shatter::state
