#pragma once

#include <cstddef>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace shatter::mixer::presentation {

inline constexpr float kMinimumGainDb = -60.0F;
inline constexpr float kMaximumGainDb = 12.0F;
inline constexpr std::size_t kUnboundedColumn = std::numeric_limits<std::size_t>::max();
using TableRow = std::vector<std::string>;

std::string fit_column(std::string_view value, std::size_t width);
void append_table(std::ostringstream& table, const std::vector<std::string>& headers,
                  const std::vector<TableRow>& rows, const std::vector<std::size_t>& maximum_widths);
float gain_db(float linear);
std::string format_gain(float linear);
std::string format_fader_position(float linear);

} // namespace shatter::mixer::presentation
