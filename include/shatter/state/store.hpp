#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace shatter::state {

struct AppSetting {
    std::string identity;
    std::string name;
    float gain_db = 0.0F;
    bool muted = false;
};

struct StreamSetting {
    std::string app_identity;
    std::string original_name;
    std::string name;
    float gain_db = 0.0F;
    bool muted = false;
};

struct Show {
    std::vector<AppSetting> apps;
    std::vector<StreamSetting> streams;
};

class Store {
public:
    Store() = default;
    explicit Store(std::filesystem::path directory);

    bool load(std::string_view name, Show& show, std::string& error) const;
    bool save(std::string_view name, const Show& show, std::string& error) const;

private:
    std::filesystem::path config_path(std::string_view name) const;

    std::filesystem::path directory_;
};

} // namespace shatter::state
