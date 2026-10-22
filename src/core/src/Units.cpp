#include "sbr/Units.h"

#include <cctype>
#include <cmath>
#include <locale>
#include <sstream>

namespace sbr::units {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

std::optional<double> parseLength(std::string_view text) {
  text = trim(text);
  if (text.empty()) return std::nullopt;

  std::size_t numberEnd = 0;
  while (numberEnd < text.size()) {
    const char c = text[numberEnd];
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+' ||
        ((c == 'e' || c == 'E') && numberEnd > 0 && numberEnd + 1 < text.size() &&
         (std::isdigit(static_cast<unsigned char>(text[numberEnd + 1])) ||
          text[numberEnd + 1] == '-' || text[numberEnd + 1] == '+'))) {
      ++numberEnd;
    } else {
      break;
    }
  }
  if (numberEnd == 0) return std::nullopt;

  // Locale independent number parsing ("." is always the decimal separator).
  std::istringstream stream{std::string(text.substr(0, numberEnd))};
  stream.imbue(std::locale::classic());
  double value = 0.0;
  stream >> value;
  if (stream.fail() || !stream.eof() || !std::isfinite(value)) return std::nullopt;

  const std::string suffix = lower(trim(text.substr(numberEnd)));
  double scale = 1.0;
  if (suffix.empty() || suffix == "mm") {
    scale = 1.0;
  } else if (suffix == "cm") {
    scale = 10.0;
  } else if (suffix == "m") {
    scale = 1000.0;
  } else {
    return std::nullopt;
  }
  return value * scale;
}

std::optional<std::vector<double>> parseLengthList(std::string_view text) {
  std::vector<double> values;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find_first_of(",;", start);
    if (end == std::string_view::npos) end = text.size();
    const auto value = parseLength(text.substr(start, end - start));
    if (!value) return std::nullopt;
    values.push_back(*value);
    start = end + 1;
  }
  return values;
}

std::optional<std::vector<double>> parseSpacingList(std::string_view text) {
  std::vector<double> values;
  if (text.find_first_not_of(" \t") == std::string_view::npos) return values;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find_first_of(",;", start);
    if (end == std::string_view::npos) end = text.size();
    std::string_view item = text.substr(start, end - start);
    int repeat = 1;
    const std::size_t times = item.find_first_of("*x");
    if (times != std::string_view::npos) {
      const std::string count(item.substr(times + 1));
      std::size_t used = 0;
      int n = 0;
      try {
        n = std::stoi(count, &used);
      } catch (...) {
        return std::nullopt;
      }
      if (count.find_first_not_of(" \t", used) != std::string::npos || n < 1 || n > 1000) return std::nullopt;
      repeat = n;
      item = item.substr(0, times);
    }
    const auto value = parseLength(item);
    if (!value) return std::nullopt;
    values.insert(values.end(), static_cast<std::size_t>(repeat), *value);
    start = end + 1;
  }
  return values;
}

std::string formatLength(double mm, int decimals) {
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream.setf(std::ios::fixed);
  stream.precision(decimals);
  stream << mm << " mm";
  return stream.str();
}

}  // namespace sbr::units
