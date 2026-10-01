// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tobsv {

// Physical plausibility band for any temperature this runtime will accept. Below absolute zero
// nothing is physical; far above it, a value is not a data-centre measurement and admitting it
// would corrupt every aggregate that contains it.
inline constexpr double kAbsoluteZeroCelsius = -273.15;
inline constexpr double kPlausibilityCeilingCelsius = 1000.0;

// Shared vocabulary. Every closed enumeration in this runtime has a stable textual spelling so that
// durable records, CLI output and exported analyses never disagree about a name.

// The outcome of any claim the runtime is asked to make. These are deliberately not booleans: a
// caller must be able to distinguish "we have no evidence" from "we have evidence that disagrees"
// from "this runtime is not the authority for that claim".
enum class EvidenceState : std::uint8_t {
  kFresh = 0,        // supported by fresh, agreeing evidence
  kStale,            // evidence exists but is outside its freshness window
  kUnknown,          // no evidence at all
  kConflicting,      // fresh evidence that disagrees with itself
  kUnsupported,      // the claim cannot be supported by the evidence model at all
  kIndeterminate,    // evidence exists but the value cannot be determined from it
  kRefused,          // the runtime declined the operation (bound, authority, or policy)
};

std::string_view to_string(EvidenceState state) noexcept;
bool parse_evidence_state(std::string_view text, EvidenceState& out) noexcept;

// True only for kFresh. Callers that need "usable evidence" must say so explicitly rather than
// testing truthiness.
inline bool is_usable(EvidenceState state) noexcept { return state == EvidenceState::kFresh; }

// Where on an entity a temperature was measured. Two readings of the same entity at different sites
// are not comparable and never substitute for one another.
enum class MeasurementSite : std::uint8_t {
  kUnknown = 0,
  kDie,
  kCase,
  kBoard,
  kInlet,
  kOutlet,
  kCoolantSupply,
  kCoolantReturn,
  kAmbient,
  kMemory,
  kPowerStage,
};

std::string_view to_string(MeasurementSite site) noexcept;
bool parse_measurement_site(std::string_view text, MeasurementSite& out) noexcept;

// Broad class of the observed entity. Envelopes are bound to a class so that a rack-scale envelope
// can never be applied to a die.
enum class EntityClass : std::uint8_t {
  kUnknown = 0,
  kComputeNode,
  kAccelerator,
  kCpuPackage,
  kMemoryModule,
  kStorageDevice,
  kPowerSupply,
  kNetworkSwitch,
  kRack,
  kCoolantLoop,
  kEnvironmentalPoint,
};

std::string_view to_string(EntityClass entity_class) noexcept;
bool parse_entity_class(std::string_view text, EntityClass& out) noexcept;

// Envelope threshold bands, ordered from the coldest bound to the envelope ceiling.
enum class ThresholdLevel : std::uint8_t {
  kNominal = 0,
  kWarn,
  kHigh,
  kCritical,
  kMaximum,
};

std::string_view to_string(ThresholdLevel level) noexcept;
bool parse_threshold_level(std::string_view text, ThresholdLevel& out) noexcept;
// The next hotter band, or kMaximum when already there.
ThresholdLevel raise(ThresholdLevel level) noexcept;
// The next colder band, or kNominal when already there.
ThresholdLevel lower(ThresholdLevel level) noexcept;

}  // namespace tobsv
