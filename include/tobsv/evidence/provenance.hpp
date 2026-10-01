// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/core/strong_id.hpp"
#include "tobsv/core/time.hpp"

namespace tobsv {

// What kind of statement a value is. These levels are declared by the producer and carried through
// every result; the runtime never upgrades one level into another.
enum class AuthorityLevel : std::uint8_t {
  kUnknown = 0,
  kMeasured,    // read from a physical sensor
  kDerived,     // computed deterministically from measured values
  kModeled,     // produced by a model or simulation
  kSynthetic,   // produced by a generator; never evidence about real hardware
};

std::string_view to_string(AuthorityLevel level) noexcept;
bool parse_authority_level(std::string_view text, AuthorityLevel& out) noexcept;

// Where a value came from at the integration boundary. This mirrors the adjacent runtimes this
// observatory composes with, without claiming any of their authority.
enum class SourceKind : std::uint8_t {
  kUnknown = 0,
  kFacilitySensor,       // building or facility telemetry
  kPlatformAgent,        // agent running on the observed platform
  kExternalTelemetry,    // an adjacent control plane's published evidence
  kOperatorDeclaration,  // entered by an operator
  kSyntheticGenerator,   // generated for test or demonstration
};

std::string_view to_string(SourceKind kind) noexcept;
bool parse_source_kind(std::string_view text, SourceKind& out) noexcept;

// Provenance travels with the value. It answers "who says so, how, and on what clock".
struct Provenance {
  SourceId source;
  AuthorityLevel authority = AuthorityLevel::kUnknown;
  SourceKind kind = SourceKind::kUnknown;
  ClockDomain clock = ClockDomain::kUnspecified;
  std::string method;

  Status validate(const Limits& limits) const;

  // Synthetic evidence is still evidence, but it is evidence about a generator. Any result that
  // rests on it must say so.
  bool is_synthetic() const noexcept {
    return kind == SourceKind::kSyntheticGenerator || authority == AuthorityLevel::kSynthetic;
  }
};

}  // namespace tobsv
