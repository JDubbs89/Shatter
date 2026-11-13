#pragma once

#include <string>
#include <string_view>

namespace shatter::control {

// Local command protocol shared by the CLI and persistent control service.
std::string request(std::string_view command);
inline constexpr int restart_requested_exit = 75;
int run_service();

} // namespace shatter::control
