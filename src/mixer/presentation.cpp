#include "mixer/presentation.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>

namespace shatter::mixer::presentation {
namespace {
constexpr std::size_t kFaderTrackWidth = 25;
} // namespace

std::string fit_column(std::string_view value, std::size_t width)
{
    if (value.size() <= width) return std::string(value);
    if (width < 4) return std::string(value.substr(0, width));
    return std::string(value.substr(0, width - 3)) + "...";
}


void append_table_border(std::ostringstream& table, const std::vector<std::size_t>& widths)
{
    table << '+';
    for (const std::size_t width : widths) table << std::string(width + 2, '-') << '+';
    table << '\n';
}

void append_table_row(std::ostringstream& table, const TableRow& row,
                      const std::vector<std::size_t>& widths)
{
    table << '|';
    for (std::size_t column = 0; column < widths.size(); ++column) {
        const std::string_view value = column < row.size() ? std::string_view(row[column]) : std::string_view{};
        table << ' ' << std::left << std::setw(static_cast<int>(widths[column]))
              << fit_column(value, widths[column]) << " |";
    }
    table << '\n';
}

void append_table(std::ostringstream& table, const std::vector<std::string>& headers,
                  const std::vector<TableRow>& rows, const std::vector<std::size_t>& maximum_widths)
{
    std::vector<std::size_t> widths(headers.size(), 1);
    for (std::size_t column = 0; column < headers.size(); ++column) {
        widths[column] = std::min(headers[column].size(), maximum_widths[column]);
        for (const auto& row : rows) {
            if (column < row.size()) widths[column] = std::max(widths[column],
                std::min(row[column].size(), maximum_widths[column]));
        }
    }
    append_table_border(table, widths);
    append_table_row(table, headers, widths);
    append_table_border(table, widths);
    for (const auto& row : rows) append_table_row(table, row, widths);
    append_table_border(table, widths);
}

float gain_db(float linear)
{
    return linear <= 0.0F ? -60.0F : std::max(-60.0F, 20.0F * std::log10(linear));
}

std::string format_gain(float linear)
{
    std::ostringstream value;
    value << std::fixed << std::setprecision(1) << gain_db(linear) << " dB";
    return value.str();
}

std::string format_fader_position(float linear)
{
    const float decibels = gain_db(linear);
    const float normalized = std::clamp((decibels - kMinimumGainDb) /
        (kMaximumGainDb - kMinimumGainDb), 0.0F, 1.0F);
    const auto marker = static_cast<std::size_t>(std::lround(normalized * (kFaderTrackWidth - 1)));
    std::string track(kFaderTrackWidth, '-');
    track[marker] = 'O';

    std::ostringstream position;
    position << "-inf |" << track << "| +12.0 dB  ";
    if (linear <= 0.0F || decibels <= kMinimumGainDb) position << "-inf";
    else position << format_gain(linear);
    return position.str();
}


} // namespace shatter::mixer::presentation
