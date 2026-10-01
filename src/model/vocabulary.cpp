// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/model/vocabulary.hpp"

namespace tobsv {

std::string_view to_string(EvidenceState state) noexcept {
  switch (state) {
    case EvidenceState::kFresh: return "fresh";
    case EvidenceState::kStale: return "stale";
    case EvidenceState::kUnknown: return "unknown";
    case EvidenceState::kConflicting: return "conflicting";
    case EvidenceState::kUnsupported: return "unsupported";
    case EvidenceState::kIndeterminate: return "indeterminate";
    case EvidenceState::kRefused: return "refused";
  }
  return "unknown";
}

bool parse_evidence_state(std::string_view text, EvidenceState& out) noexcept {
  if (text == "fresh") { out = EvidenceState::kFresh; return true; }
  if (text == "stale") { out = EvidenceState::kStale; return true; }
  if (text == "unknown") { out = EvidenceState::kUnknown; return true; }
  if (text == "conflicting") { out = EvidenceState::kConflicting; return true; }
  if (text == "unsupported") { out = EvidenceState::kUnsupported; return true; }
  if (text == "indeterminate") { out = EvidenceState::kIndeterminate; return true; }
  if (text == "refused") { out = EvidenceState::kRefused; return true; }
  return false;
}

std::string_view to_string(MeasurementSite site) noexcept {
  switch (site) {
    case MeasurementSite::kUnknown: return "unknown";
    case MeasurementSite::kDie: return "die";
    case MeasurementSite::kCase: return "case";
    case MeasurementSite::kBoard: return "board";
    case MeasurementSite::kInlet: return "inlet";
    case MeasurementSite::kOutlet: return "outlet";
    case MeasurementSite::kCoolantSupply: return "coolant_supply";
    case MeasurementSite::kCoolantReturn: return "coolant_return";
    case MeasurementSite::kAmbient: return "ambient";
    case MeasurementSite::kMemory: return "memory";
    case MeasurementSite::kPowerStage: return "power_stage";
  }
  return "unknown";
}

bool parse_measurement_site(std::string_view text, MeasurementSite& out) noexcept {
  if (text == "unknown") { out = MeasurementSite::kUnknown; return true; }
  if (text == "die") { out = MeasurementSite::kDie; return true; }
  if (text == "case") { out = MeasurementSite::kCase; return true; }
  if (text == "board") { out = MeasurementSite::kBoard; return true; }
  if (text == "inlet") { out = MeasurementSite::kInlet; return true; }
  if (text == "outlet") { out = MeasurementSite::kOutlet; return true; }
  if (text == "coolant_supply") { out = MeasurementSite::kCoolantSupply; return true; }
  if (text == "coolant_return") { out = MeasurementSite::kCoolantReturn; return true; }
  if (text == "ambient") { out = MeasurementSite::kAmbient; return true; }
  if (text == "memory") { out = MeasurementSite::kMemory; return true; }
  if (text == "power_stage") { out = MeasurementSite::kPowerStage; return true; }
  return false;
}

std::string_view to_string(EntityClass entity_class) noexcept {
  switch (entity_class) {
    case EntityClass::kUnknown: return "unknown";
    case EntityClass::kComputeNode: return "compute_node";
    case EntityClass::kAccelerator: return "accelerator";
    case EntityClass::kCpuPackage: return "cpu_package";
    case EntityClass::kMemoryModule: return "memory_module";
    case EntityClass::kStorageDevice: return "storage_device";
    case EntityClass::kPowerSupply: return "power_supply";
    case EntityClass::kNetworkSwitch: return "network_switch";
    case EntityClass::kRack: return "rack";
    case EntityClass::kCoolantLoop: return "coolant_loop";
    case EntityClass::kEnvironmentalPoint: return "environmental_point";
  }
  return "unknown";
}

bool parse_entity_class(std::string_view text, EntityClass& out) noexcept {
  if (text == "unknown") { out = EntityClass::kUnknown; return true; }
  if (text == "compute_node") { out = EntityClass::kComputeNode; return true; }
  if (text == "accelerator") { out = EntityClass::kAccelerator; return true; }
  if (text == "cpu_package") { out = EntityClass::kCpuPackage; return true; }
  if (text == "memory_module") { out = EntityClass::kMemoryModule; return true; }
  if (text == "storage_device") { out = EntityClass::kStorageDevice; return true; }
  if (text == "power_supply") { out = EntityClass::kPowerSupply; return true; }
  if (text == "network_switch") { out = EntityClass::kNetworkSwitch; return true; }
  if (text == "rack") { out = EntityClass::kRack; return true; }
  if (text == "coolant_loop") { out = EntityClass::kCoolantLoop; return true; }
  if (text == "environmental_point") { out = EntityClass::kEnvironmentalPoint; return true; }
  return false;
}

std::string_view to_string(ThresholdLevel level) noexcept {
  switch (level) {
    case ThresholdLevel::kNominal: return "nominal";
    case ThresholdLevel::kWarn: return "warn";
    case ThresholdLevel::kHigh: return "high";
    case ThresholdLevel::kCritical: return "critical";
    case ThresholdLevel::kMaximum: return "maximum";
  }
  return "nominal";
}

bool parse_threshold_level(std::string_view text, ThresholdLevel& out) noexcept {
  if (text == "nominal") { out = ThresholdLevel::kNominal; return true; }
  if (text == "warn") { out = ThresholdLevel::kWarn; return true; }
  if (text == "high") { out = ThresholdLevel::kHigh; return true; }
  if (text == "critical") { out = ThresholdLevel::kCritical; return true; }
  if (text == "maximum") { out = ThresholdLevel::kMaximum; return true; }
  return false;
}

ThresholdLevel raise(ThresholdLevel level) noexcept {
  switch (level) {
    case ThresholdLevel::kNominal: return ThresholdLevel::kWarn;
    case ThresholdLevel::kWarn: return ThresholdLevel::kHigh;
    case ThresholdLevel::kHigh: return ThresholdLevel::kCritical;
    case ThresholdLevel::kCritical: return ThresholdLevel::kMaximum;
    case ThresholdLevel::kMaximum: return ThresholdLevel::kMaximum;
  }
  return ThresholdLevel::kMaximum;
}

ThresholdLevel lower(ThresholdLevel level) noexcept {
  switch (level) {
    case ThresholdLevel::kNominal: return ThresholdLevel::kNominal;
    case ThresholdLevel::kWarn: return ThresholdLevel::kNominal;
    case ThresholdLevel::kHigh: return ThresholdLevel::kWarn;
    case ThresholdLevel::kCritical: return ThresholdLevel::kHigh;
    case ThresholdLevel::kMaximum: return ThresholdLevel::kCritical;
  }
  return ThresholdLevel::kNominal;
}

}  // namespace tobsv
