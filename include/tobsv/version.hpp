// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>

namespace tobsv {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 1;

// Persistence format version. A reader refuses any other major version outright and reports a
// version mismatch rather than attempting a best-effort interpretation.
inline constexpr unsigned kLogFormatVersion = 1U;

// Stable product identity. These strings are part of the observable contract: they appear in CLI
// output, in exported analyses and in the durable log header.
const std::string& product_id();
const std::string& version_string();
const std::string& build_compiler();
const std::string& build_configuration();

}  // namespace tobsv
