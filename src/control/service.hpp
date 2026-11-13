#pragma once

#include <string>
#include <string_view>

namespace shatter::control {

// Local command protocol shared by the CLI and persistent control service.
std::string request(std::string_view command);
int run_service();

} // namespace shatter::control
