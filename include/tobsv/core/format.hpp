// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tobsv/core/time.hpp"

namespace tobsv {

// Shortest decimal representation that still round-trips exactly. Locale independent and identical
// on every supported toolchain, which is what lets explanations and durable records be compared
// byte for byte.
std::string format_real(double value);

// A duration rendered in seconds with the same round-trip guarantee, suffixed with "s".
std::string format_seconds(const Duration& duration);

// Joins a list of strings with a separator, preserving order.
std::string join(const std::vector<std::string>& parts, std::string_view separator);

}  // namespace tobsv
