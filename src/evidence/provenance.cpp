// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/evidence/provenance.hpp"

namespace tobsv {

std::string_view to_string(AuthorityLevel level) noexcept {
  switch (level) {
    case AuthorityLevel::kUnknown: return "unknown";
    case AuthorityLevel::kMeasured: return "measured";
    case AuthorityLevel::kDerived: return "derived";
    case AuthorityLevel::kModeled: return "modeled";
    case AuthorityLevel::kSynthetic: return "synthetic";
  }
  return "unknown";
}

bool parse_authority_level(std::string_view text, AuthorityLevel& out) noexcept {
  if (text == "unknown") { out = AuthorityLevel::kUnknown; return true; }
  if (text == "measured") { out = AuthorityLevel::kMeasured; return true; }
  if (text == "derived") { out = AuthorityLevel::kDerived; return true; }
  if (text == "modeled") { out = AuthorityLevel::kModeled; return true; }
  if (text == "synthetic") { out = AuthorityLevel::kSynthetic; return true; }
  return false;
}

std::string_view to_string(SourceKind kind) noexcept {
  switch (kind) {
    case SourceKind::kUnknown: return "unknown";
    case SourceKind::kFacilitySensor: return "facility_sensor";
    case SourceKind::kPlatformAgent: return "platform_agent";
    case SourceKind::kExternalTelemetry: return "external_telemetry";
    case SourceKind::kOperatorDeclaration: return "operator_declaration";
    case SourceKind::kSyntheticGenerator: return "synthetic_generator";
  }
  return "unknown";
}

bool parse_source_kind(std::string_view text, SourceKind& out) noexcept {
  if (text == "unknown") { out = SourceKind::kUnknown; return true; }
  if (text == "facility_sensor") { out = SourceKind::kFacilitySensor; return true; }
  if (text == "platform_agent") { out = SourceKind::kPlatformAgent; return true; }
  if (text == "external_telemetry") { out = SourceKind::kExternalTelemetry; return true; }
  if (text == "operator_declaration") { out = SourceKind::kOperatorDeclaration; return true; }
  if (text == "synthetic_generator") { out = SourceKind::kSyntheticGenerator; return true; }
  return false;
}

Status Provenance::validate(const Limits& limits) const {
  if (!source.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "provenance has no source identity");
  }
  if (authority == AuthorityLevel::kUnknown) {
    return Status::failure(ErrorCode::kUnsupported,
                           "source " + source.value() +
                               " did not declare how its values were obtained");
  }
  if (kind == SourceKind::kUnknown) {
    return Status::failure(ErrorCode::kUnsupported,
                           "source " + source.value() + " did not declare its integration kind");
  }
  if (clock == ClockDomain::kUnspecified) {
    return Status::failure(ErrorCode::kUnsupported,
                           "source " + source.value() + " did not declare its clock domain");
  }
  if (method.empty()) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           "source " + source.value() + " did not name its measurement method");
  }
  const Status text = validate_text(method, "provenance method", limits.max_text_length);
  if (!text.ok()) {
    return text;
  }
  return Status::success();
}

}  // namespace tobsv
