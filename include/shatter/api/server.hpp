#pragma once

#include <string>
#include <string_view>

namespace shatter::api {

std::string request(std::string_view command);
int run_server();

} // namespace shatter::api
