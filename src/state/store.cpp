#include "shatter/state/store.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace shatter::state {
namespace {

constexpr std::string_view kHeader = "SHATTER-CONFIG 1";
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
    const char* base = std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path directory = directory_;
    if (directory.empty()) {
        directory = base && *base ? std::filesystem::path(base) / "shatter"
            : std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".config" / "shatter";
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
        if (!std::getline(file, header) || header != kHeader) {
            error = "unsupported or invalid configuration format";
            return false;
        }

        Show loaded;
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream row(line);
            char type = 0;
            if (!(row >> type)) continue;
            int muted = 0;
            if (type == 'A') {
                AppSetting item;
                if (!(row >> std::quoted(item.identity) >> std::quoted(item.name) >> item.gain_db >> muted) ||
                    (muted != 0 && muted != 1) || !valid_gain(item.gain_db)) {
                    error = "invalid app entry in configuration";
                    return false;
                }
                item.muted = muted != 0;
                loaded.apps.push_back(std::move(item));
            } else if (type == 'S') {
                StreamSetting item;
                if (!(row >> std::quoted(item.app_identity) >> std::quoted(item.original_name) >>
                      std::quoted(item.name) >> item.gain_db >> muted) ||
                    (muted != 0 && muted != 1) || !valid_gain(item.gain_db)) {
                    error = "invalid stream entry in configuration";
                    return false;
                }
                item.muted = muted != 0;
                loaded.streams.push_back(std::move(item));
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
        for (const auto& app : show.apps) {
            if (!valid_gain(app.gain_db)) {
                error = "app gain is outside the supported range";
                return false;
            }
            file << "A " << std::quoted(app.identity) << ' ' << std::quoted(app.name) << ' '
                 << app.gain_db << ' ' << (app.muted ? 1 : 0) << '\n';
        }
        for (const auto& stream : show.streams) {
            if (!valid_gain(stream.gain_db)) {
                error = "stream gain is outside the supported range";
                return false;
            }
            file << "S " << std::quoted(stream.app_identity) << ' '
                 << std::quoted(stream.original_name) << ' ' << std::quoted(stream.name) << ' '
                 << stream.gain_db << ' ' << (stream.muted ? 1 : 0) << '\n';
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
