#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace shatter::engine {

inline std::string make_stable_selector_id(std::string_view prefix, std::string_view identity)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : identity) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    std::ostringstream result;
    result << prefix << '-' << std::hex << std::setw(16) << std::setfill('0') << hash;
    return result.str();
}

inline std::string stable_device_identity(std::string_view backend_identity, std::string_view name)
{
    if (backend_identity.starts_with("device:") || backend_identity.starts_with("device-name:"))
        return std::string(backend_identity);
    if (!backend_identity.empty()) return "device:" + std::string(backend_identity);
    return name.empty() ? std::string{} : "device-name:" + std::string(name);
}

inline std::string stable_application_input_identity(std::string_view application_id,
                                                      std::string_view process_binary,
                                                      std::string_view application,
                                                      std::string_view node_name)
{
    const std::string_view application_identity = !application_id.empty() ? application_id
        : !process_binary.empty() ? process_binary : application;
    if (node_name.empty()) return {};
    if (application_identity.empty()) return {};
    return "app-input:" + std::string(application_identity) + ":" + std::string(node_name);
}

} // namespace shatter::engine
