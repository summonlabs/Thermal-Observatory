// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/envelope/headroom.hpp"

#include "tobsv/core/checked.hpp"
#include "tobsv/core/format.hpp"

namespace tobsv {
namespace {

constexpr ThresholdLevel kOrderedLevels[5] = {ThresholdLevel::kNominal, ThresholdLevel::kWarn,
                                              ThresholdLevel::kHigh, ThresholdLevel::kCritical,
                                              ThresholdLevel::kMaximum};

}  // namespace

JsonValue HeadroomValue::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("level", JsonValue::text(std::string(to_string(level))));
  out.set("limit_celsius", JsonValue::real(limit_c));
  out.set("observed_celsius", JsonValue::real(observed_c));
  out.set("headroom_celsius", JsonValue::real(headroom_c));
  out.set("exceeded", JsonValue::boolean(exceeded));
  out.set("consumed_fraction_defined", JsonValue::boolean(consumed_fraction_defined));
  if (consumed_fraction_defined) {
    out.set("consumed_fraction", JsonValue::real(consumed_fraction));
  }
  return out;
}

const HeadroomValue* HeadroomReport::level(ThresholdLevel wanted) const {
  for (const HeadroomValue& value : levels) {
    if (value.level == wanted) {
      return &value;
    }
  }
  return nullptr;
}

JsonValue HeadroomReport::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("entity", JsonValue::text(entity.value()));
  out.set("sensor", JsonValue::text(sensor.value()));
  out.set("site", JsonValue::text(std::string(to_string(site))));
  out.set("state", JsonValue::text(std::string(to_string(state))));
  out.set("envelope_applied", JsonValue::boolean(envelope_applied));
  if (envelope_applied) {
    out.set("envelope", JsonValue::text(envelope.value()));
  }
  out.set("observed_celsius", JsonValue::real(observed_celsius));
  out.set("observed_at", JsonValue::text(observed_at.to_string()));
  out.set("evidence", JsonValue::text(evidence.value()));
  out.set("evidence_synthetic", JsonValue::boolean(evidence_synthetic));
  out.set("current_level_known", JsonValue::boolean(current_level_known));
  out.set("current_level", JsonValue::text(std::string(to_string(current_level))));
  JsonValue level_array = JsonValue::array();
  for (const HeadroomValue& value : levels) {
    level_array.push(value.to_json());
  }
  out.set("levels", level_array);
  JsonValue missing = JsonValue::array();
  for (const ThresholdLevel value : missing_levels) {
    missing.push(JsonValue::text(std::string(to_string(value))));
  }
  out.set("missing_levels", missing);
  out.set("reason", JsonValue::text(reason));
  return out;
}

Result<HeadroomReport> compute_headroom(const SubjectTemperature& subject, EntityClass entity_class,
                                        const EnvelopeRegistry& envelopes, const Limits& limits) {
  HeadroomReport report;
  report.entity = subject.entity;
  report.sensor = subject.sensor;
  report.site = subject.site;
  report.observed_celsius = subject.representative_celsius;
  report.observed_at = subject.representative_at;
  report.evidence = subject.representative_id;
  report.evidence_synthetic = subject.representative_synthetic;

  if (subject.state != EvidenceState::kFresh) {
    report.state = subject.state;
    report.missing_levels.assign(std::begin(kOrderedLevels), std::end(kOrderedLevels));
    report.reason = "no headroom is computed: " + subject.reason;
    return report;
  }
  if (!checked::is_finite(subject.representative_celsius)) {
    report.state = EvidenceState::kIndeterminate;
    report.missing_levels.assign(std::begin(kOrderedLevels), std::end(kOrderedLevels));
    report.reason = "the representative observation is not a finite temperature";
    return report;
  }

  const ThermalEnvelope* envelope = envelopes.select(subject.entity, entity_class, subject.site);
  if (envelope == nullptr) {
    report.state = EvidenceState::kUnsupported;
    report.missing_levels.assign(std::begin(kOrderedLevels), std::end(kOrderedLevels));
    report.reason = "no thermal envelope is registered for entity " + subject.entity.value() +
                    " (class " + std::string(to_string(entity_class)) + ", site " +
                    std::string(to_string(subject.site)) +
                    "), so no limit is available to measure headroom against";
    return report;
  }

  report.envelope = envelope->id;
  report.envelope_applied = true;
  report.current_level_known = true;
  report.current_level = ThresholdLevel::kNominal;

  for (const ThresholdLevel level : kOrderedLevels) {
    if (!envelope->has_level(level)) {
      report.missing_levels.push_back(level);
      continue;
    }
    const double limit = envelope->limit(level);
    const auto headroom = checked::sub(limit, subject.representative_celsius);
    if (!headroom.has_value()) {
      report.state = EvidenceState::kIndeterminate;
      report.levels.clear();
      report.missing_levels.assign(std::begin(kOrderedLevels), std::end(kOrderedLevels));
      report.reason = "headroom to band " + std::string(to_string(level)) +
                      " is not computable from the available values";
      return report;
    }
    HeadroomValue value;
    value.level = level;
    value.limit_c = limit;
    value.observed_c = subject.representative_celsius;
    value.headroom_c = *headroom;
    value.exceeded = subject.representative_celsius > limit;
    if (limit > envelope->nominal_c) {
      const auto consumed = checked::div(subject.representative_celsius - envelope->nominal_c,
                                         limit - envelope->nominal_c);
      if (consumed.has_value()) {
        value.consumed_fraction_defined = true;
        value.consumed_fraction = *consumed;
      }
    }
    report.levels.push_back(value);
    if (subject.representative_celsius >= limit) {
      report.current_level = level;
    }
  }

  const HeadroomValue* ceiling = report.level(ThresholdLevel::kMaximum);
  report.state = EvidenceState::kFresh;
  report.reason = "observed " + format_real(subject.representative_celsius) +
                  " C against envelope " + envelope->id.value() + " for entity " +
                  subject.entity.value() + " at site " + std::string(to_string(subject.site)) +
                  "; the reading sits in the " + std::string(to_string(report.current_level)) +
                  " band";
  if (ceiling != nullptr) {
    report.reason += " with " + format_real(ceiling->headroom_c) +
                     " C of headroom to the envelope ceiling";
  } else {
    report.reason += " and the envelope declares no ceiling band";
  }
  if (!report.missing_levels.empty()) {
    std::vector<std::string> names;
    for (const ThresholdLevel level : report.missing_levels) {
      names.emplace_back(to_string(level));
    }
    report.reason += "; bands not declared by the envelope: " + join(names, ",");
  }
  const Status text = validate_text(report.reason, "headroom reason", limits.max_text_length * 4);
  if (!text.ok()) {
    report.reason = "observed " + format_real(subject.representative_celsius) +
                    " C against envelope " + envelope->id.value();
  }
  return report;
}

std::string_view to_string(ThresholdTransition transition) noexcept {
  switch (transition) {
    case ThresholdTransition::kNone: return "none";
    case ThresholdTransition::kUnchanged: return "unchanged";
    case ThresholdTransition::kEntered: return "entered";
    case ThresholdTransition::kExited: return "exited";
    case ThresholdTransition::kEscalated: return "escalated";
    case ThresholdTransition::kDeescalated: return "deescalated";
    case ThresholdTransition::kUnknown: return "unknown";
  }
  return "unknown";
}

JsonValue ThresholdTransitionRecord::to_json() const {
  JsonValue out = JsonValue::object();
  out.set("entity", JsonValue::text(entity.value()));
  out.set("sensor", JsonValue::text(sensor.value()));
  out.set("site", JsonValue::text(std::string(to_string(site))));
  out.set("transition", JsonValue::text(std::string(to_string(transition))));
  out.set("previous_level", JsonValue::text(std::string(to_string(previous))));
  out.set("current_level", JsonValue::text(std::string(to_string(current))));
  out.set("evaluated_at", JsonValue::text(evaluated_at.to_string()));
  out.set("evidence", JsonValue::text(evidence.value()));
  out.set("reason", JsonValue::text(reason));
  return out;
}

ThresholdTransitionRecord classify_transition(bool have_previous, ThresholdLevel previous,
                                              const HeadroomReport& report,
                                              const Timestamp& evaluated_at) {
  ThresholdTransitionRecord record;
  record.entity = report.entity;
  record.sensor = report.sensor;
  record.site = report.site;
  record.previous = have_previous ? previous : ThresholdLevel::kNominal;
  record.current = report.current_level;
  record.evaluated_at = evaluated_at;
  record.evidence = report.evidence;

  if (report.state != EvidenceState::kFresh || !report.current_level_known) {
    record.transition = ThresholdTransition::kUnknown;
    record.reason = "the band could not be established for this evaluation: " + report.reason;
    return record;
  }
  if (!have_previous) {
    record.transition = ThresholdTransition::kNone;
    record.reason = "no earlier evaluation of entity " + report.entity.value() +
                    " is available, so the reading is reported in the " +
                    std::string(to_string(report.current_level)) +
                    " band without a transition";
    return record;
  }

  const auto rank = [](ThresholdLevel level) { return static_cast<unsigned>(level); };
  if (rank(record.current) == rank(record.previous)) {
    record.transition = ThresholdTransition::kUnchanged;
    record.reason = "the reading remains in the " + std::string(to_string(record.current)) + " band";
    return record;
  }
  if (rank(record.current) > rank(record.previous)) {
    record.transition =
        record.previous == ThresholdLevel::kNominal ? ThresholdTransition::kEntered
                                                    : ThresholdTransition::kEscalated;
    record.reason = "the reading moved from the " + std::string(to_string(record.previous)) +
                    " band to the " + std::string(to_string(record.current)) + " band";
    return record;
  }
  record.transition = record.current == ThresholdLevel::kNominal ? ThresholdTransition::kExited
                                                                 : ThresholdTransition::kDeescalated;
  record.reason = "the reading moved from the " + std::string(to_string(record.previous)) +
                  " band to the " + std::string(to_string(record.current)) + " band";
  return record;
}

}  // namespace tobsv
