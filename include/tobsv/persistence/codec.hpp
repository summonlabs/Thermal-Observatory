// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "tobsv/core/json.hpp"
#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/coupling/coupling.hpp"
#include "tobsv/derating/derating.hpp"
#include "tobsv/envelope/envelope.hpp"
#include "tobsv/evidence/evidence.hpp"
#include "tobsv/model/generation.hpp"
#include "tobsv/model/inventory.hpp"
#include "tobsv/model/topology.hpp"

namespace tobsv {

// Encoding of the durable record set.
//
// Every record is one canonical JSON document on one line inside a framed log entry. Encoding is
// canonical (object keys are sorted and numbers use the shortest exact form), so the same record
// always produces the same bytes; decoding is strict, so a record that does not match the schema is
// rejected rather than partially applied.
//
// Bumping kRecordVersion is a breaking change: a reader refuses a record of an unknown version
// instead of guessing at the missing fields.
inline constexpr unsigned kCodecVersion = 1U;

enum class RecordKind : std::uint8_t {
  kObservation = 0,
  kFence,
  kEnvelope,
  kEntity,
  kAdjacency,
  kCoupling,
  kDerating,
};

std::string_view to_string(RecordKind kind) noexcept;
bool parse_record_kind(std::string_view text, RecordKind& out) noexcept;

JsonValue encode(const Fence& fence);
JsonValue encode(const TemperatureObservation& observation);
JsonValue encode(const ThermalEnvelope& envelope);
JsonValue encode(const EntityRecord& record);
JsonValue encode(const TopologyEdge& edge);
JsonValue encode(const CouplingRelation& relation);
JsonValue encode(const DeratingEvidence& evidence);

// Wraps an encoded payload in the record envelope.
Result<JsonValue> wrap_record(RecordKind kind, JsonValue payload);

struct DecodedRecord {
  RecordKind kind = RecordKind::kObservation;
  unsigned version = kCodecVersion;
  Fence fence;
  TemperatureObservation observation;
  ThermalEnvelope envelope;
  EntityRecord entity;
  TopologyEdge adjacency;
  CouplingRelation coupling;
  DeratingEvidence derating;
};

// Decodes one canonical record document. The record is validated against the schema and against
// the supplied limits; nothing is accepted on a best-effort basis.
Result<DecodedRecord> decode_record(std::string_view document, const Limits& limits);

}  // namespace tobsv
