// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/persistence/codec.hpp"

#include <algorithm>

namespace tobsv {
namespace {

Result<std::string> require_text(const JsonValue& value, const char* field) {
  const Result<std::string> text = value.require_string(field);
  if (!text.ok()) {
    return text.error();
  }
  return text.value();
}

Result<std::uint64_t> require_counter_value(const JsonValue& value, const char* field) {
  const Result<std::int64_t> raw = value.require_integer(field);
  if (!raw.ok()) {
    return raw.error();
  }
  if (raw.value() < 0) {
    return Status::failure(ErrorCode::kOutOfRange,
                           std::string("record field '") + field + "' must not be negative");
  }
  return static_cast<std::uint64_t>(raw.value());
}

template <class Id>
Result<Id> parse_id(const JsonValue& value, const char* field) {
  const Result<std::string> text = require_text(value, field);
  if (!text.ok()) {
    return text.error();
  }
  const Result<Id> id = Id::parse(text.value());
  if (!id.ok()) {
    return id.error();
  }
  return id.value();
}

template <class CounterType>
Result<CounterType> parse_counter(const JsonValue& value, const char* field) {
  const Result<std::uint64_t> raw = require_counter_value(value, field);
  if (!raw.ok()) {
    return raw.error();
  }
  if (raw.value() == 0) {
    return Status::failure(ErrorCode::kInvalidArgument,
                           std::string("record field '") + field + "' must be at least 1");
  }
  return CounterType::from(raw.value());
}

Result<Timestamp> parse_timestamp(const JsonValue& value, const char* field) {
  const Result<std::string> text = require_text(value, field);
  if (!text.ok()) {
    return text.error();
  }
  return Timestamp::parse(text.value());
}

template <class Enum>
Result<Enum> parse_enum(const JsonValue& value, const char* field,
                        bool (*parser)(std::string_view, Enum&) noexcept) {
  const Result<std::string> text = require_text(value, field);
  if (!text.ok()) {
    return text.error();
  }
  Enum out{};
  if (!parser(text.value(), out)) {
    return Status::failure(ErrorCode::kMalformedInput,
                           std::string("record field '") + field + "' has the unknown value '" +
                               text.value() + "'");
  }
  return out;
}

JsonValue id_array(const std::vector<ObservationId>& ids) {
  JsonValue out = JsonValue::array();
  for (const ObservationId& id : ids) {
    out.push(JsonValue::text(id.value()));
  }
  return out;
}

Result<std::vector<ObservationId>> parse_id_array(const JsonValue& value, const char* field,
                                                  const Limits& limits) {
  const JsonValue* array = value.find(field);
  if (array == nullptr) {
    return Status::failure(ErrorCode::kMalformedInput,
                           std::string("record is missing the '") + field + "' list");
  }
  if (!array->is_array()) {
    return Status::failure(ErrorCode::kMalformedInput,
                           std::string("record field '") + field + "' is not a list");
  }
  if (array->items().size() > limits.max_citations) {
    return Status::failure(ErrorCode::kLimitExceeded,
                           std::string("record field '") + field + "' is longer than the bound");
  }
  std::vector<ObservationId> out;
  out.reserve(array->items().size());
  for (const JsonValue& item : array->items()) {
    if (!item.is_string()) {
      return Status::failure(ErrorCode::kMalformedInput,
                             std::string("record field '") + field + "' holds a non-string");
    }
    const Result<ObservationId> id = ObservationId::parse(item.as_string());
    if (!id.ok()) {
      return id.error();
    }
    out.push_back(id.value());
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace

std::string_view to_string(RecordKind kind) noexcept {
  switch (kind) {
    case RecordKind::kObservation: return "observation";
    case RecordKind::kFence: return "fence";
    case RecordKind::kEnvelope: return "envelope";
    case RecordKind::kEntity: return "entity";
    case RecordKind::kAdjacency: return "adjacency";
    case RecordKind::kCoupling: return "coupling";
    case RecordKind::kDerating: return "derating";
  }
  return "observation";
}

bool parse_record_kind(std::string_view text, RecordKind& out) noexcept {
  if (text == "observation") { out = RecordKind::kObservation; return true; }
  if (text == "fence") { out = RecordKind::kFence; return true; }
  if (text == "envelope") { out = RecordKind::kEnvelope; return true; }
  if (text == "entity") { out = RecordKind::kEntity; return true; }
  if (text == "adjacency") { out = RecordKind::kAdjacency; return true; }
  if (text == "coupling") { out = RecordKind::kCoupling; return true; }
  if (text == "derating") { out = RecordKind::kDerating; return true; }
  return false;
}

JsonValue encode(const Fence& fence) {
  JsonValue out = JsonValue::object();
  out.set("source", JsonValue::text(fence.source.value()));
  out.set("epoch", JsonValue::integer(static_cast<std::int64_t>(fence.epoch.value())));
  out.set("generation", JsonValue::integer(static_cast<std::int64_t>(fence.generation.value())));
  out.set("revision", JsonValue::integer(static_cast<std::int64_t>(fence.revision.value())));
  out.set("incarnation", JsonValue::integer(static_cast<std::int64_t>(fence.incarnation.value())));
  out.set("sequence", JsonValue::integer(static_cast<std::int64_t>(fence.sequence.value())));
  out.set("attempt", JsonValue::text(fence.attempt.value()));
  return out;
}

JsonValue encode(const TemperatureObservation& observation) {
  JsonValue out = JsonValue::object();
  out.set("id", JsonValue::text(observation.id.value()));
  out.set("entity", JsonValue::text(observation.entity.value()));
  out.set("sensor", JsonValue::text(observation.sensor.value()));
  out.set("site", JsonValue::text(std::string(to_string(observation.site))));
  out.set("celsius", JsonValue::real(observation.celsius));
  out.set("observed_at", JsonValue::text(observation.observed_at.to_string()));
  out.set("received_at", JsonValue::text(observation.received_at.to_string()));
  out.set("source", JsonValue::text(observation.provenance.source.value()));
  out.set("authority", JsonValue::text(std::string(to_string(observation.provenance.authority))));
  out.set("source_kind", JsonValue::text(std::string(to_string(observation.provenance.kind))));
  out.set("clock", JsonValue::text(std::string(to_string(observation.provenance.clock))));
  out.set("method", JsonValue::text(observation.provenance.method));
  out.set("quality_supplied", JsonValue::boolean(observation.quality.supplied));
  out.set("quality_flags", JsonValue::integer(
                               static_cast<std::int64_t>(observation.quality.flags.bits())));
  out.set("quality_has_confidence", JsonValue::boolean(observation.quality.has_confidence));
  out.set("quality_confidence", JsonValue::real(observation.quality.confidence));
  if (observation.envelope.is_set()) {
    out.set("envelope", JsonValue::text(observation.envelope.value()));
  }
  out.set("fence", encode(observation.fence));
  return out;
}

JsonValue encode(const ThermalEnvelope& envelope) {
  JsonValue out = JsonValue::object();
  out.set("id", JsonValue::text(envelope.id.value()));
  if (envelope.entity.is_set()) {
    out.set("entity", JsonValue::text(envelope.entity.value()));
  }
  out.set("entity_class", JsonValue::text(std::string(to_string(envelope.entity_class))));
  out.set("site", JsonValue::text(std::string(to_string(envelope.site))));
  out.set("has_nominal", JsonValue::boolean(envelope.has_nominal));
  out.set("nominal_c", JsonValue::real(envelope.nominal_c));
  out.set("has_warn", JsonValue::boolean(envelope.has_warn));
  out.set("warn_c", JsonValue::real(envelope.warn_c));
  out.set("has_high", JsonValue::boolean(envelope.has_high));
  out.set("high_c", JsonValue::real(envelope.high_c));
  out.set("has_critical", JsonValue::boolean(envelope.has_critical));
  out.set("critical_c", JsonValue::real(envelope.critical_c));
  out.set("has_maximum", JsonValue::boolean(envelope.has_maximum));
  out.set("maximum_c", JsonValue::real(envelope.maximum_c));
  out.set("declared_by", JsonValue::text(envelope.declared_by.value()));
  out.set("declared_at", JsonValue::text(envelope.declared_at.to_string()));
  out.set("basis", JsonValue::text(envelope.basis));
  return out;
}

JsonValue encode(const EntityRecord& record) {
  JsonValue out = JsonValue::object();
  out.set("entity", JsonValue::text(record.entity.value()));
  out.set("entity_class", JsonValue::text(std::string(to_string(record.entity_class))));
  if (record.zone.is_set()) {
    out.set("zone", JsonValue::text(record.zone.value()));
  }
  if (record.site.is_set()) {
    out.set("site", JsonValue::text(record.site.value()));
  }
  out.set("label", JsonValue::text(record.label));
  return out;
}

JsonValue encode(const TopologyEdge& edge) {
  JsonValue out = JsonValue::object();
  out.set("from", JsonValue::text(edge.from.value()));
  out.set("to", JsonValue::text(edge.to.value()));
  out.set("kind", JsonValue::text(std::string(to_string(edge.kind))));
  out.set("topology", JsonValue::text(edge.topology.value()));
  out.set("declared_by", JsonValue::text(edge.declared_by.value()));
  return out;
}

JsonValue encode(const CouplingRelation& relation) {
  JsonValue out = JsonValue::object();
  out.set("id", JsonValue::text(relation.id.value()));
  out.set("from", JsonValue::text(relation.from.value()));
  out.set("to", JsonValue::text(relation.to.value()));
  out.set("kind", JsonValue::text(std::string(to_string(relation.kind))));
  out.set("direction", JsonValue::text(std::string(to_string(relation.direction))));
  out.set("strength", JsonValue::real(relation.strength));
  out.set("asserted_by", JsonValue::text(relation.asserted_by.value()));
  out.set("asserted_at", JsonValue::text(relation.asserted_at.to_string()));
  out.set("method", JsonValue::text(relation.method));
  JsonValue citations = JsonValue::array();
  for (const CouplingCitation& citation : relation.citations) {
    citations.push(citation.to_json());
  }
  out.set("citations", citations);
  out.set("fence", encode(relation.fence));
  return out;
}

JsonValue encode(const DeratingEvidence& evidence) {
  JsonValue out = JsonValue::object();
  out.set("id", JsonValue::text(evidence.id.value()));
  out.set("entity", JsonValue::text(evidence.entity.value()));
  if (evidence.sensor.is_set()) {
    out.set("sensor", JsonValue::text(evidence.sensor.value()));
  }
  out.set("site", JsonValue::text(std::string(to_string(evidence.site))));
  out.set("kind", JsonValue::text(std::string(to_string(evidence.kind))));
  out.set("magnitude", JsonValue::real(evidence.magnitude));
  out.set("observed_at", JsonValue::text(evidence.observed_at.to_string()));
  out.set("asserted_at", JsonValue::text(evidence.asserted_at.to_string()));
  out.set("asserted_by", JsonValue::text(evidence.asserted_by.value()));
  out.set("source_kind", JsonValue::text(std::string(to_string(evidence.source_kind))));
  out.set("citations", id_array(evidence.citations));
  out.set("trigger_level", JsonValue::text(std::string(to_string(evidence.trigger_level))));
  out.set("method", JsonValue::text(evidence.method));
  out.set("fence", encode(evidence.fence));
  return out;
}

Result<JsonValue> wrap_record(RecordKind kind, JsonValue payload) {
  // The payload is nested under the record kind, so a document names its own shape twice: once in
  // the envelope and once in the body. A reader that trusts only one of them cannot silently accept
  // a record whose body belongs to a different kind.
  JsonValue body = JsonValue::object();
  body.set(std::string(to_string(kind)), std::move(payload));
  JsonValue out = JsonValue::object();
  out.set("codec", JsonValue::integer(static_cast<std::int64_t>(kCodecVersion)));
  out.set("kind", JsonValue::text(std::string(to_string(kind))));
  out.set("payload", std::move(body));
  return out;
}

Result<DecodedRecord> decode_record(std::string_view document, const Limits& limits) {
  Result<JsonValue> parsed = JsonValue::parse(document, limits);
  if (!parsed.ok()) {
    return parsed.error();
  }
  const JsonValue& root = parsed.value();
  if (!root.is_object()) {
    return Status::failure(ErrorCode::kMalformedInput, "a durable record must be a JSON object");
  }
  const Result<std::int64_t> codec = root.require_integer("codec");
  if (!codec.ok()) {
    return codec.error();
  }
  if (codec.value() != static_cast<std::int64_t>(kCodecVersion)) {
    return Status::failure(ErrorCode::kVersionMismatch,
                           "record codec version " + std::to_string(codec.value()) +
                               " is not readable by this build, which writes version " +
                               std::to_string(kCodecVersion));
  }
  const Result<RecordKind> kind = parse_enum<RecordKind>(root, "kind", parse_record_kind);
  if (!kind.ok()) {
    return kind.error();
  }
  const JsonValue* payload = root.find("payload");
  if (payload == nullptr || !payload->is_object()) {
    return Status::failure(ErrorCode::kMalformedInput, "a durable record needs an object payload");
  }
  if (payload->fields().size() != 1 || payload->find(to_string(kind.value())) == nullptr) {
    return Status::failure(ErrorCode::kMalformedInput,
                           "record body does not match the declared kind '" +
                               std::string(to_string(kind.value())) + "'");
  }

  DecodedRecord decoded;
  decoded.kind = kind.value();
  decoded.version = kCodecVersion;

  if (decoded.kind == RecordKind::kFence) {
    const JsonValue* fence = payload->find("fence");
    if (fence == nullptr) {
      return Status::failure(ErrorCode::kMalformedInput, "fence record has no fence payload");
    }
    const Result<SourceId> source = parse_id<SourceId>(*fence, "source");
    if (!source.ok()) return source.error();
    const Result<Epoch> epoch = parse_counter<Epoch>(*fence, "epoch");
    if (!epoch.ok()) return epoch.error();
    const Result<Generation> generation = parse_counter<Generation>(*fence, "generation");
    if (!generation.ok()) return generation.error();
    const Result<Revision> revision = parse_counter<Revision>(*fence, "revision");
    if (!revision.ok()) return revision.error();
    const Result<Incarnation> incarnation = parse_counter<Incarnation>(*fence, "incarnation");
    if (!incarnation.ok()) return incarnation.error();
    const Result<Sequence> sequence = parse_counter<Sequence>(*fence, "sequence");
    if (!sequence.ok()) return sequence.error();
    const Result<AttemptId> attempt = parse_id<AttemptId>(*fence, "attempt");
    if (!attempt.ok()) return attempt.error();
    decoded.fence.source = source.value();
    decoded.fence.epoch = epoch.value();
    decoded.fence.generation = generation.value();
    decoded.fence.revision = revision.value();
    decoded.fence.incarnation = incarnation.value();
    decoded.fence.sequence = sequence.value();
    decoded.fence.attempt = attempt.value();
    const Status valid = decoded.fence.validate();
    if (!valid.ok()) return valid;
    return decoded;
  }

  if (decoded.kind == RecordKind::kObservation) {
    const JsonValue* observation = payload->find("observation");
    if (observation == nullptr || !observation->is_object()) {
      return Status::failure(ErrorCode::kMalformedInput,
                             "observation record has no observation payload");
    }
    TemperatureObservation& out = decoded.observation;
    const Result<ObservationId> id = parse_id<ObservationId>(*observation, "id");
    if (!id.ok()) return id.error();
    const Result<EntityId> entity = parse_id<EntityId>(*observation, "entity");
    if (!entity.ok()) return entity.error();
    const Result<SensorId> sensor = parse_id<SensorId>(*observation, "sensor");
    if (!sensor.ok()) return sensor.error();
    const Result<MeasurementSite> site =
        parse_enum<MeasurementSite>(*observation, "site", parse_measurement_site);
    if (!site.ok()) return site.error();
    const Result<double> celsius = observation->require_real("celsius");
    if (!celsius.ok()) return celsius.error();
    const Result<Timestamp> observed_at = parse_timestamp(*observation, "observed_at");
    if (!observed_at.ok()) return observed_at.error();
    const Result<Timestamp> received_at = parse_timestamp(*observation, "received_at");
    if (!received_at.ok()) return received_at.error();
    const Result<AuthorityLevel> authority =
        parse_enum<AuthorityLevel>(*observation, "authority", parse_authority_level);
    if (!authority.ok()) return authority.error();
    const Result<SourceKind> source_kind =
        parse_enum<SourceKind>(*observation, "source_kind", parse_source_kind);
    if (!source_kind.ok()) return source_kind.error();
    const Result<ClockDomain> clock =
        parse_enum<ClockDomain>(*observation, "clock", parse_clock_domain);
    if (!clock.ok()) return clock.error();
    const Result<std::string> method = require_text(*observation, "method");
    if (!method.ok()) return method.error();
    const Result<bool> quality_supplied = observation->require_bool("quality_supplied");
    if (!quality_supplied.ok()) return quality_supplied.error();
    const Result<std::int64_t> quality_flags = observation->require_integer("quality_flags");
    if (!quality_flags.ok()) return quality_flags.error();
    const Result<bool> quality_has_confidence = observation->require_bool("quality_has_confidence");
    if (!quality_has_confidence.ok()) return quality_has_confidence.error();
    const Result<double> quality_confidence = observation->require_real("quality_confidence");
    if (!quality_confidence.ok()) return quality_confidence.error();

    out.id = id.value();
    out.entity = entity.value();
    out.sensor = sensor.value();
    out.site = site.value();
    out.celsius = celsius.value();
    out.observed_at = observed_at.value();
    out.received_at = received_at.value();
    const Result<SourceId> provenance_source = parse_id<SourceId>(*observation, "source");
    if (!provenance_source.ok()) return provenance_source.error();
    out.provenance.source = provenance_source.value();
    out.provenance.authority = authority.value();
    out.provenance.kind = source_kind.value();
    out.provenance.clock = clock.value();
    out.provenance.method = method.value();
    out.quality.supplied = quality_supplied.value();
    out.quality.flags = QualityFlags(static_cast<std::uint32_t>(quality_flags.value()));
    out.quality.has_confidence = quality_has_confidence.value();
    out.quality.confidence = quality_confidence.value();
    const JsonValue* envelope = observation->find("envelope");
    if (envelope != nullptr && envelope->is_string()) {
      const Result<EnvelopeId> parsed_envelope = EnvelopeId::parse(envelope->as_string());
      if (!parsed_envelope.ok()) return parsed_envelope.error();
      out.envelope = parsed_envelope.value();
    }
    const JsonValue* fence = observation->find("fence");
    if (fence == nullptr || !fence->is_object()) {
      return Status::failure(ErrorCode::kMalformedInput, "observation record has no fence");
    }
    const Result<SourceId> fence_source = parse_id<SourceId>(*fence, "source");
    if (!fence_source.ok()) return fence_source.error();
    const Result<Epoch> epoch = parse_counter<Epoch>(*fence, "epoch");
    if (!epoch.ok()) return epoch.error();
    const Result<Generation> generation = parse_counter<Generation>(*fence, "generation");
    if (!generation.ok()) return generation.error();
    const Result<Revision> revision = parse_counter<Revision>(*fence, "revision");
    if (!revision.ok()) return revision.error();
    const Result<Incarnation> incarnation = parse_counter<Incarnation>(*fence, "incarnation");
    if (!incarnation.ok()) return incarnation.error();
    const Result<Sequence> sequence = parse_counter<Sequence>(*fence, "sequence");
    if (!sequence.ok()) return sequence.error();
    const Result<AttemptId> attempt = parse_id<AttemptId>(*fence, "attempt");
    if (!attempt.ok()) return attempt.error();
    out.fence.source = fence_source.value();
    out.fence.epoch = epoch.value();
    out.fence.generation = generation.value();
    out.fence.revision = revision.value();
    out.fence.incarnation = incarnation.value();
    out.fence.sequence = sequence.value();
    out.fence.attempt = attempt.value();
    const Status valid = out.validate(limits);
    if (!valid.ok()) return valid;
    const ObservationId derived = compute_observation_id(out);
    if (derived != out.id) {
      return Status::failure(ErrorCode::kIntegrityFailure,
                             "stored observation " + out.id.value() +
                                 " does not match its content, which hashes to " +
                                 derived.value());
    }
    return decoded;
  }

  if (decoded.kind == RecordKind::kEnvelope) {
    const JsonValue* envelope = payload->find("envelope");
    if (envelope == nullptr || !envelope->is_object()) {
      return Status::failure(ErrorCode::kMalformedInput, "envelope record has no envelope payload");
    }
    ThermalEnvelope& out = decoded.envelope;
    const Result<EnvelopeId> id = parse_id<EnvelopeId>(*envelope, "id");
    if (!id.ok()) return id.error();
    out.id = id.value();
    const JsonValue* entity = envelope->find("entity");
    if (entity != nullptr && entity->is_string()) {
      const Result<EntityId> parsed_entity = EntityId::parse(entity->as_string());
      if (!parsed_entity.ok()) return parsed_entity.error();
      out.entity = parsed_entity.value();
    }
    const Result<EntityClass> entity_class =
        parse_enum<EntityClass>(*envelope, "entity_class", parse_entity_class);
    if (!entity_class.ok()) return entity_class.error();
    const Result<MeasurementSite> site =
        parse_enum<MeasurementSite>(*envelope, "site", parse_measurement_site);
    if (!site.ok()) return site.error();
    const Result<bool> has_nominal = envelope->require_bool("has_nominal");
    if (!has_nominal.ok()) return has_nominal.error();
    const Result<double> nominal_c = envelope->require_real("nominal_c");
    if (!nominal_c.ok()) return nominal_c.error();
    const Result<bool> has_warn = envelope->require_bool("has_warn");
    if (!has_warn.ok()) return has_warn.error();
    const Result<double> warn_c = envelope->require_real("warn_c");
    if (!warn_c.ok()) return warn_c.error();
    const Result<bool> has_high = envelope->require_bool("has_high");
    if (!has_high.ok()) return has_high.error();
    const Result<double> high_c = envelope->require_real("high_c");
    if (!high_c.ok()) return high_c.error();
    const Result<bool> has_critical = envelope->require_bool("has_critical");
    if (!has_critical.ok()) return has_critical.error();
    const Result<double> critical_c = envelope->require_real("critical_c");
    if (!critical_c.ok()) return critical_c.error();
    const Result<bool> has_maximum = envelope->require_bool("has_maximum");
    if (!has_maximum.ok()) return has_maximum.error();
    const Result<double> maximum_c = envelope->require_real("maximum_c");
    if (!maximum_c.ok()) return maximum_c.error();
    const Result<SourceId> declared_by = parse_id<SourceId>(*envelope, "declared_by");
    if (!declared_by.ok()) return declared_by.error();
    const Result<Timestamp> declared_at = parse_timestamp(*envelope, "declared_at");
    if (!declared_at.ok()) return declared_at.error();
    const Result<std::string> basis = require_text(*envelope, "basis");
    if (!basis.ok()) return basis.error();

    out.entity_class = entity_class.value();
    out.site = site.value();
    out.has_nominal = has_nominal.value();
    out.nominal_c = nominal_c.value();
    out.has_warn = has_warn.value();
    out.warn_c = warn_c.value();
    out.has_high = has_high.value();
    out.high_c = high_c.value();
    out.has_critical = has_critical.value();
    out.critical_c = critical_c.value();
    out.has_maximum = has_maximum.value();
    out.maximum_c = maximum_c.value();
    out.declared_by = declared_by.value();
    out.declared_at = declared_at.value();
    out.basis = basis.value();
    const Status valid = out.validate(limits);
    if (!valid.ok()) return valid;
    const EnvelopeId derived = compute_envelope_id(out);
    if (derived != out.id) {
      return Status::failure(ErrorCode::kIntegrityFailure,
                             "stored envelope " + out.id.value() +
                                 " does not match its content, which hashes to " +
                                 derived.value());
    }
    return decoded;
  }

  if (decoded.kind == RecordKind::kEntity) {
    const JsonValue* entity = payload->find("entity");
    if (entity == nullptr || !entity->is_object()) {
      return Status::failure(ErrorCode::kMalformedInput, "entity record has no entity payload");
    }
    EntityRecord& out = decoded.entity;
    const Result<EntityId> id = parse_id<EntityId>(*entity, "entity");
    if (!id.ok()) return id.error();
    const Result<EntityClass> entity_class =
        parse_enum<EntityClass>(*entity, "entity_class", parse_entity_class);
    if (!entity_class.ok()) return entity_class.error();
    const Result<std::string> label = require_text(*entity, "label");
    if (!label.ok()) return label.error();
    out.entity = id.value();
    out.entity_class = entity_class.value();
    out.label = label.value();
    const JsonValue* zone = entity->find("zone");
    if (zone != nullptr && zone->is_string()) {
      const Result<ZoneRef> parsed_zone = ZoneRef::parse(zone->as_string());
      if (!parsed_zone.ok()) return parsed_zone.error();
      out.zone = parsed_zone.value();
    }
    const JsonValue* site = entity->find("site");
    if (site != nullptr && site->is_string()) {
      const Result<SiteId> parsed_site = SiteId::parse(site->as_string());
      if (!parsed_site.ok()) return parsed_site.error();
      out.site = parsed_site.value();
    }
    return decoded;
  }

  if (decoded.kind == RecordKind::kAdjacency) {
    const JsonValue* edge = payload->find("adjacency");
    if (edge == nullptr || !edge->is_object()) {
      return Status::failure(ErrorCode::kMalformedInput, "adjacency record has no payload");
    }
    TopologyEdge& out = decoded.adjacency;
    const Result<EntityId> from = parse_id<EntityId>(*edge, "from");
    if (!from.ok()) return from.error();
    const Result<EntityId> to = parse_id<EntityId>(*edge, "to");
    if (!to.ok()) return to.error();
    const Result<AdjacencyKind> adjacency_kind =
        parse_enum<AdjacencyKind>(*edge, "kind", parse_adjacency_kind);
    if (!adjacency_kind.ok()) return adjacency_kind.error();
    const Result<TopologyRef> topology = parse_id<TopologyRef>(*edge, "topology");
    if (!topology.ok()) return topology.error();
    const Result<SourceId> declared_by = parse_id<SourceId>(*edge, "declared_by");
    if (!declared_by.ok()) return declared_by.error();
    out.from = from.value();
    out.to = to.value();
    out.kind = adjacency_kind.value();
    out.topology = topology.value();
    out.declared_by = declared_by.value();
    const Status valid = out.validate();
    if (!valid.ok()) return valid;
    return decoded;
  }

  if (decoded.kind == RecordKind::kCoupling) {
    const JsonValue* relation = payload->find("coupling");
    if (relation == nullptr || !relation->is_object()) {
      return Status::failure(ErrorCode::kMalformedInput, "coupling record has no payload");
    }
    CouplingRelation& out = decoded.coupling;
    const Result<CouplingId> id = parse_id<CouplingId>(*relation, "id");
    if (!id.ok()) return id.error();
    const Result<EntityId> from = parse_id<EntityId>(*relation, "from");
    if (!from.ok()) return from.error();
    const Result<EntityId> to = parse_id<EntityId>(*relation, "to");
    if (!to.ok()) return to.error();
    const Result<CouplingKind> coupling_kind =
        parse_enum<CouplingKind>(*relation, "kind", parse_coupling_kind);
    if (!coupling_kind.ok()) return coupling_kind.error();
    const Result<std::string> direction_text = require_text(*relation, "direction");
    if (!direction_text.ok()) return direction_text.error();
    CouplingDirection direction = CouplingDirection::kSymmetric;
    if (direction_text.value() == "from_to") {
      direction = CouplingDirection::kFromTo;
    } else if (direction_text.value() != "symmetric") {
      return Status::failure(ErrorCode::kMalformedInput,
                             "coupling direction '" + direction_text.value() + "' is unknown");
    }
    const Result<double> strength = relation->require_real("strength");
    if (!strength.ok()) return strength.error();
    const Result<SourceId> asserted_by = parse_id<SourceId>(*relation, "asserted_by");
    if (!asserted_by.ok()) return asserted_by.error();
    const Result<Timestamp> asserted_at = parse_timestamp(*relation, "asserted_at");
    if (!asserted_at.ok()) return asserted_at.error();
    const Result<std::string> method = require_text(*relation, "method");
    if (!method.ok()) return method.error();
    const JsonValue* citations = relation->find("citations");
    if (citations == nullptr || !citations->is_array()) {
      return Status::failure(ErrorCode::kMalformedInput, "coupling record has no citation list");
    }
    if (citations->items().size() > limits.max_citations_per_relation) {
      return Status::failure(ErrorCode::kLimitExceeded,
                             "coupling record cites more relations than the configured bound");
    }
    std::vector<CouplingCitation> parsed_citations;
    for (const JsonValue& item : citations->items()) {
      CouplingCitation citation;
      if (!item.is_object()) {
        return Status::failure(ErrorCode::kMalformedInput, "a coupling citation is not an object");
      }
      if (const JsonValue* observation = item.find("observation"); observation != nullptr) {
        if (!observation->is_string()) {
          return Status::failure(ErrorCode::kMalformedInput, "a coupling citation is malformed");
        }
        const Result<ObservationId> cited_observation =
            ObservationId::parse(observation->as_string());
        if (!cited_observation.ok()) return cited_observation.error();
        citation.is_topology = false;
        citation.observation = cited_observation.value();
      } else if (const JsonValue* topology = item.find("topology"); topology != nullptr) {
        if (!topology->is_string()) {
          return Status::failure(ErrorCode::kMalformedInput, "a coupling citation is malformed");
        }
        const Result<TopologyRef> cited_topology = TopologyRef::parse(topology->as_string());
        if (!cited_topology.ok()) return cited_topology.error();
        citation.is_topology = true;
        citation.topology = cited_topology.value();
      } else {
        return Status::failure(ErrorCode::kMalformedInput,
                               "a coupling citation names neither an observation nor a topology");
      }
      parsed_citations.push_back(citation);
    }

    out.id = id.value();
    out.from = from.value();
    out.to = to.value();
    out.kind = coupling_kind.value();
    out.direction = direction;
    out.strength = strength.value();
    out.asserted_by = asserted_by.value();
    out.asserted_at = asserted_at.value();
    out.method = method.value();
    out.citations = std::move(parsed_citations);
    const JsonValue* fence = relation->find("fence");
    if (fence == nullptr || !fence->is_object()) {
      return Status::failure(ErrorCode::kMalformedInput, "coupling record has no fence");
    }
    const Result<SourceId> fence_source = parse_id<SourceId>(*fence, "source");
    if (!fence_source.ok()) return fence_source.error();
    const Result<Epoch> epoch = parse_counter<Epoch>(*fence, "epoch");
    if (!epoch.ok()) return epoch.error();
    const Result<Generation> generation = parse_counter<Generation>(*fence, "generation");
    if (!generation.ok()) return generation.error();
    const Result<Revision> revision = parse_counter<Revision>(*fence, "revision");
    if (!revision.ok()) return revision.error();
    const Result<Incarnation> incarnation = parse_counter<Incarnation>(*fence, "incarnation");
    if (!incarnation.ok()) return incarnation.error();
    const Result<Sequence> sequence = parse_counter<Sequence>(*fence, "sequence");
    if (!sequence.ok()) return sequence.error();
    const Result<AttemptId> attempt = parse_id<AttemptId>(*fence, "attempt");
    if (!attempt.ok()) return attempt.error();
    out.fence.source = fence_source.value();
    out.fence.epoch = epoch.value();
    out.fence.generation = generation.value();
    out.fence.revision = revision.value();
    out.fence.incarnation = incarnation.value();
    out.fence.sequence = sequence.value();
    out.fence.attempt = attempt.value();
    const Status valid = out.validate(limits);
    if (!valid.ok()) return valid;
    const CouplingId derived = compute_coupling_id(out);
    if (derived != out.id) {
      return Status::failure(ErrorCode::kIntegrityFailure,
                             "stored coupling " + out.id.value() +
                                 " does not match its content, which hashes to " +
                                 derived.value());
    }
    return decoded;
  }

  const JsonValue* evidence = payload->find("derating");
  if (evidence == nullptr || !evidence->is_object()) {
    return Status::failure(ErrorCode::kMalformedInput, "derating record has no payload");
  }
  DeratingEvidence& out = decoded.derating;
  const Result<DeratingId> id = parse_id<DeratingId>(*evidence, "id");
  if (!id.ok()) return id.error();
  const Result<EntityId> entity = parse_id<EntityId>(*evidence, "entity");
  if (!entity.ok()) return entity.error();
  const Result<MeasurementSite> site =
      parse_enum<MeasurementSite>(*evidence, "site", parse_measurement_site);
  if (!site.ok()) return site.error();
  const Result<DeratingKind> derating_kind =
      parse_enum<DeratingKind>(*evidence, "kind", parse_derating_kind);
  if (!derating_kind.ok()) return derating_kind.error();
  const Result<double> magnitude = evidence->require_real("magnitude");
  if (!magnitude.ok()) return magnitude.error();
  const Result<Timestamp> observed_at = parse_timestamp(*evidence, "observed_at");
  if (!observed_at.ok()) return observed_at.error();
  const Result<Timestamp> asserted_at = parse_timestamp(*evidence, "asserted_at");
  if (!asserted_at.ok()) return asserted_at.error();
  const Result<SourceId> asserted_by = parse_id<SourceId>(*evidence, "asserted_by");
  if (!asserted_by.ok()) return asserted_by.error();
  const Result<SourceKind> source_kind =
      parse_enum<SourceKind>(*evidence, "source_kind", parse_source_kind);
  if (!source_kind.ok()) return source_kind.error();
  const Result<std::vector<ObservationId>> citations =
      parse_id_array(*evidence, "citations", limits);
  if (!citations.ok()) return citations.error();
  const Result<ThresholdLevel> trigger =
      parse_enum<ThresholdLevel>(*evidence, "trigger_level", parse_threshold_level);
  if (!trigger.ok()) return trigger.error();
  const Result<std::string> method = require_text(*evidence, "method");
  if (!method.ok()) return method.error();
  const JsonValue* fence = evidence->find("fence");
  if (fence == nullptr || !fence->is_object()) {
    return Status::failure(ErrorCode::kMalformedInput, "derating record has no fence");
  }
  const Result<SourceId> fence_source = parse_id<SourceId>(*fence, "source");
  if (!fence_source.ok()) return fence_source.error();
  const Result<Epoch> epoch = parse_counter<Epoch>(*fence, "epoch");
  if (!epoch.ok()) return epoch.error();
  const Result<Generation> generation = parse_counter<Generation>(*fence, "generation");
  if (!generation.ok()) return generation.error();
  const Result<Revision> revision = parse_counter<Revision>(*fence, "revision");
  if (!revision.ok()) return revision.error();
  const Result<Incarnation> incarnation = parse_counter<Incarnation>(*fence, "incarnation");
  if (!incarnation.ok()) return incarnation.error();
  const Result<Sequence> sequence = parse_counter<Sequence>(*fence, "sequence");
  if (!sequence.ok()) return sequence.error();
  const Result<AttemptId> attempt = parse_id<AttemptId>(*fence, "attempt");
  if (!attempt.ok()) return attempt.error();

  out.id = id.value();
  out.entity = entity.value();
  out.site = site.value();
  out.kind = derating_kind.value();
  out.magnitude = magnitude.value();
  out.observed_at = observed_at.value();
  out.asserted_at = asserted_at.value();
  out.asserted_by = asserted_by.value();
  out.source_kind = source_kind.value();
  out.citations = citations.value();
  out.trigger_level = trigger.value();
  out.method = method.value();
  out.fence.source = fence_source.value();
  out.fence.epoch = epoch.value();
  out.fence.generation = generation.value();
  out.fence.revision = revision.value();
  out.fence.incarnation = incarnation.value();
  out.fence.sequence = sequence.value();
  out.fence.attempt = attempt.value();
  const JsonValue* sensor = evidence->find("sensor");
  if (sensor != nullptr && sensor->is_string()) {
    const Result<SensorId> parsed_sensor = SensorId::parse(sensor->as_string());
    if (!parsed_sensor.ok()) return parsed_sensor.error();
    out.sensor = parsed_sensor.value();
  }
  const Status valid = out.validate(limits);
  if (!valid.ok()) return valid;
  const DeratingId derived = compute_derating_id(out);
  if (derived != out.id) {
    return Status::failure(ErrorCode::kIntegrityFailure,
                           "stored derating evidence " + out.id.value() +
                               " does not match its content, which hashes to " + derived.value());
  }
  return decoded;
}

}  // namespace tobsv