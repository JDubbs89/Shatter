#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace shatter::engine {

struct WasapiPlaybackSession {
    unsigned int process_id = 0;
    std::string application;
    std::string process_binary;
    std::string session_id;
};

struct WasapiPlaybackEndpoint {
    std::string id;
    std::string name;
    bool is_default = false;
};

inline bool wasapi_selector_equal(std::string_view left, std::string_view right)
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
}

inline bool matches_wasapi_playback_selector(const WasapiPlaybackSession& session,
                                              std::string_view selector)
{
    return wasapi_selector_equal(session.application, selector) ||
           wasapi_selector_equal(session.process_binary, selector);
}

inline bool resolve_wasapi_endpoint(const std::vector<WasapiPlaybackEndpoint>& endpoints,
                                    std::string_view selector, std::string& endpoint_id,
                                    std::string& error)
{
    for (const auto& endpoint : endpoints) {
        if (wasapi_selector_equal(endpoint.id, selector)) {
            endpoint_id = endpoint.id;
            error.clear();
            return true;
        }
    }

    const WasapiPlaybackEndpoint* match = nullptr;
    for (const auto& endpoint : endpoints) {
        if (!wasapi_selector_equal(endpoint.name, selector)) continue;
        if (match != nullptr) {
            error = "WASAPI endpoint name '" + std::string(selector) + "' is ambiguous; use its endpoint ID";
            return false;
        }
        match = &endpoint;
    }
    if (match == nullptr) {
        error = "no WASAPI playback endpoint matches selector '" + std::string(selector) + "'";
        return false;
    }
    endpoint_id = match->id;
    error.clear();
    return true;
}

} // namespace shatter::engine
