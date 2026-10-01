// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/core/format.hpp"

#include <charconv>
#include <cstdio>

#include "tobsv/core/checked.hpp"

namespace tobsv {

std::string format_real(double value) {
  if (!checked::is_finite(value)) {
    return value != value ? "nan" : (value < 0.0 ? "-inf" : "inf");
  }
  // std::to_chars with no format argument produces the shortest representation that round-trips
  // exactly, and it is specified to be independent of the global locale. snprintf is not: under a
  // locale whose decimal separator is a comma it writes "41,5", which is not JSON at all and which
  // this runtime's own parser then refuses - turning a durable record into an unreadable one.
  char buffer[40];
  const std::to_chars_result conversion =
      std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (conversion.ec != std::errc{}) {
    return "0.0";
  }
  std::string out(buffer, conversion.ptr);
  // A value that happens to be integral would otherwise encode without a fractional part and be
  // read back as an integer, so a real always keeps a decimal point.
  if (out.find('.') == std::string::npos && out.find('e') == std::string::npos &&
      out.find('E') == std::string::npos) {
    out += ".0";
  }
  return out;
}

std::string format_seconds(const Duration& duration) {
  return format_real(duration.seconds()) + "s";
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index != 0) {
      out.append(separator);
    }
    out += parts[index];
  }
  return out;
}

}  // namespace tobsv
