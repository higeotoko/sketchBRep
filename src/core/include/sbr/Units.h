#pragma once
// Parsing and formatting of lengths typed into the measurement box.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sbr::units {

// Parses a single length such as "3000", "4.5m", "300 cm", "-12.5mm".
// A value without a suffix is interpreted in millimetres. Returns mm.
std::optional<double> parseLength(std::string_view text);

// Parses a comma/semicolon separated list of lengths, e.g. "3000,4500" or
// "3m; 4.5m". Returns std::nullopt if any element fails to parse.
std::optional<std::vector<double>> parseLengthList(std::string_view text);

// Formats a mm value for display with the given number of decimals.
// Display precision is independent from the model precision.
std::string formatLength(double mm, int decimals = 1);

}  // namespace sbr::units
