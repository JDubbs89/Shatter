#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>

namespace shatter::state {

struct AppSetting {
    std::string identity;
    std::string name;
    float gain_db = 0.0F;
    bool muted = false;
    std::vector<std::string> output_identities;
    bool has_output_sends = false;
};

struct StreamSetting {
    std::string app_identity;
    std::string original_name;
    std::string name;
    float gain_db = 0.0F;
    bool muted = false;
    std::vector<std::string> output_identities;
    bool has_output_sends = false;
};

struct SinkBinding {
    std::string identity;
    std::string name;
};

struct OutputSetting {
    std::size_t slot = 0;
    std::string identity;
    std::string name;
    std::vector<SinkBinding> sinks;
};

struct Show {
    std::vector<AppSetting> apps;
    std::vector<StreamSetting> streams;
    std::vector<OutputSetting> outputs;
    std::vector<std::string> capture_output_identities;
    bool has_capture_sends = false;
};

class Store {
public:
    Store() = default;
    explicit Store(std::filesystem::path directory);

    bool load(std::string_view name, Show& show, std::string& error) const;
    bool save(std::string_view name, const Show& show, std::string& error) const;
    bool has_show(std::string_view name) const;
    bool default_show(std::string& name, std::string& error) const;
    bool set_default_show(std::string_view name, std::string& error) const;

private:
    std::filesystem::path config_path(std::string_view name) const;
    std::filesystem::path default_show_path() const;

    std::filesystem::path directory_;
};

} // namespace shatter::state
