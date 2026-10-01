// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "test_framework.hpp"
#include "test_support.hpp"

using namespace tobsv;
using namespace tobstest;

namespace {

// The framework status assertions are defined over Status; a Result carries its Status through
// error(), so this adapter lets a Result be checked with the same macros and messages.
template <class T>
Status status_of(const Result<T>& result) {
  return result.error();
}

// A fixed seed SplitMix64 generator. The sequence is defined here rather than taken from the
// standard library so that every property check below reproduces exactly on every run and on every
// toolchain: a property test that samples differently per build is not a test.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next_u64() {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t mixed = state_;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
    return mixed ^ (mixed >> 31U);
  }

  std::int64_t next_i64() { return static_cast<std::int64_t>(next_u64()); }

  // A value in [0, 1) built from the top 53 bits, which is exactly the mantissa of a double.
  double next_unit() {
    return static_cast<double>(next_u64() >> 11U) * (1.0 / 9007199254740992.0);
  }

  double next_range(double low, double high) { return low + (high - low) * next_unit(); }

 private:
  std::uint64_t state_;
};

// Real values that have a history of breaking naive encoders: the extremes of the exponent range,
// subnormals, a signed zero and values that need all 17 digits to round trip.
double generated_real(Generator& generator) {
  const double extremes[] = {0.0,
                             -0.0,
                             1.0,
                             -1.0,
                             0.1,
                             1.0 / 3.0,
                             1.0e-300,
                             1.0e300,
                             1.7976931348623157e308,
                             2.2250738585072014e-308,
                             4.9406564584124654e-324,
                             1.2345678901234567e-17,
                             -273.15,
                             1000.0};
  const std::size_t count = sizeof(extremes) / sizeof(extremes[0]);
  if (generator.next_u64() % 3U == 0U) {
    return extremes[generator.next_u64() % count];
  }
  const double magnitude = generator.next_unit() * 2.0 - 1.0;
  const int exponent = static_cast<int>(generator.next_u64() % 601U) - 300;
  return magnitude * std::pow(10.0, static_cast<double>(exponent));
}

const char* generated_string(Generator& generator) {
  const char* texts[] = {"",
                         "plain",
                         "quote\"inside",
                         "back\\slash",
                         "tab\there",
                         "line\nbreak",
                         "unicode \xc3\xa9 accent",
                         "\u00e9 literal escape text",
                         "control \x01 byte",
                         "trailing space "};
  return texts[generator.next_u64() % (sizeof(texts) / sizeof(texts[0]))];
}

JsonValue generated_value(Generator& generator, int depth) {
  // The depth bound keeps the corpus inside the encoder depth budget while still producing nested
  // arrays and objects, which are where a canonical encoder usually goes wrong.
  const std::uint64_t choice =
      depth >= 3 ? generator.next_u64() % 5U : generator.next_u64() % 7U;
  if (choice == 0) {
    return JsonValue::null();
  }
  if (choice == 1) {
    return JsonValue::boolean((generator.next_u64() & 1U) != 0U);
  }
  if (choice == 2) {
    return JsonValue::integer(generator.next_i64());
  }
  if (choice == 3) {
    return JsonValue::real(generated_real(generator));
  }
  if (choice == 4) {
    return JsonValue::text(generated_string(generator));
  }
  if (choice == 5) {
    JsonValue array = JsonValue::array();
    const std::uint64_t items = generator.next_u64() % 5U;
    for (std::uint64_t index = 0; index < items; ++index) {
      array.push(generated_value(generator, depth + 1));
    }
    return array;
  }
  JsonValue object = JsonValue::object();
  const std::uint64_t fields = generator.next_u64() % 5U;
  for (std::uint64_t index = 0; index < fields; ++index) {
    object.set("k" + std::to_string(index), generated_value(generator, depth + 1));
  }
  return object;
}

HeadroomReport hot_report(const std::string& entity_name, double celsius, std::uint64_t seed) {
  HeadroomReport report;
  report.entity = entity_id(entity_name);
  report.sensor = sensor_id("sensor-outlet-1");
  report.site = MeasurementSite::kOutlet;
  report.state = EvidenceState::kFresh;
  report.observed_celsius = celsius;
  report.observed_at = base_instant();
  report.evidence = ObservationId::from_digest(seed);
  report.current_level_known = true;
  report.current_level = ThresholdLevel::kCritical;
  HeadroomValue band;
  band.level = ThresholdLevel::kWarn;
  band.limit_c = 35.0;
  band.observed_c = celsius;
  band.headroom_c = 35.0 - celsius;
  band.exceeded = celsius > 35.0;
  report.levels.push_back(band);
  return report;
}

}  // namespace

TOBSV_TEST("property", "headroom_equals_the_declared_limit_less_the_observation") {
  const Limits limits;
  const ThermalEnvelope envelope =
      make_envelope(entity_id("prop-node"), EntityClass::kComputeNode, MeasurementSite::kOutlet,
                    -200.0, -100.0, 0.0, 100.0, 500.0);
  EnvelopeRegistry envelopes;
  TOBSV_ASSERT_OK(envelopes.add(envelope, limits));

  Generator generator(0x5EEDULL);
  for (int iteration = 0; iteration < 300; ++iteration) {
    SubjectTemperature subject;
    subject.entity = envelope.entity;
    subject.sensor = sensor_id("sensor-outlet-1");
    subject.site = MeasurementSite::kOutlet;
    subject.state = EvidenceState::kFresh;
    subject.representative_celsius = generator.next_range(-273.15, 1000.0);
    subject.representative_at = base_instant();
    subject.representative_id = ObservationId::from_digest(generator.next_u64());

    const Result<HeadroomReport> report =
        compute_headroom(subject, EntityClass::kComputeNode, envelopes, limits);
    TOBSV_ASSERT_TRUE(report.ok());
    TOBSV_ASSERT_EQ(report.value().levels.size(), static_cast<std::size_t>(5));
    for (const HeadroomValue& value : report.value().levels) {
      // The identity is exact, not approximate: headroom is the signed distance to the limit, so a
      // negative value has to survive as an exceedance rather than being clamped to zero.
      TOBSV_ASSERT_TRUE(value.headroom_c == value.limit_c - value.observed_c);
      TOBSV_ASSERT_TRUE(value.observed_c == subject.representative_celsius);
      TOBSV_ASSERT_TRUE(value.exceeded == (value.observed_c > value.limit_c));
      if (value.level == ThresholdLevel::kNominal) {
        // The nominal band is the bottom of the usable range, so no fraction of it can be consumed.
        TOBSV_ASSERT_FALSE(value.consumed_fraction_defined);
      } else {
        TOBSV_ASSERT_TRUE(value.consumed_fraction_defined);
        TOBSV_ASSERT_TRUE(value.consumed_fraction ==
                          (subject.representative_celsius - envelope.nominal_c) /
                              (value.limit_c - envelope.nominal_c));
      }
    }
  }
}

TOBSV_TEST("property", "band_of_never_decreases_as_temperature_rises") {
  const ThermalEnvelope envelope =
      make_envelope(entity_id("prop-node"), EntityClass::kComputeNode, MeasurementSite::kOutlet,
                    -200.0, -100.0, 0.0, 100.0, 500.0);
  Generator generator(0xB0B0ULL);

  // Any pair of temperatures ordered the same way must produce bands ordered the same way; that is
  // what lets a report say "hotter than before" without re-reading the envelope.
  for (int iteration = 0; iteration < 400; ++iteration) {
    const double lower = generator.next_range(-273.15, 1000.0);
    const double higher = lower + generator.next_range(0.0, 1273.15);
    ThresholdLevel lower_band = ThresholdLevel::kNominal;
    ThresholdLevel higher_band = ThresholdLevel::kNominal;
    TOBSV_ASSERT_TRUE(band_of(envelope, lower, lower_band));
    TOBSV_ASSERT_TRUE(band_of(envelope, higher, higher_band));
    TOBSV_ASSERT_TRUE(static_cast<unsigned>(lower_band) <= static_cast<unsigned>(higher_band));
  }

  // A rising sequence never steps back down a band.
  double temperature = -273.15;
  unsigned previous = 0;
  for (int step = 0; step < 500; ++step) {
    temperature += generator.next_range(0.0, 3.0);
    ThresholdLevel band = ThresholdLevel::kNominal;
    TOBSV_ASSERT_TRUE(band_of(envelope, temperature, band));
    const unsigned rank = static_cast<unsigned>(band);
    TOBSV_ASSERT_TRUE(rank >= previous);
    previous = rank;
  }
  // The hottest band is reachable at the envelope ceiling, so the walk above is not vacuous.
  ThresholdLevel hottest = ThresholdLevel::kNominal;
  TOBSV_ASSERT_TRUE(band_of(envelope, 600.0, hottest));
  TOBSV_ASSERT_TRUE(hottest == ThresholdLevel::kMaximum);

  // A value that is not a temperature cannot claim any band, and the output is left alone.
  ThresholdLevel untouched = ThresholdLevel::kWarn;
  TOBSV_ASSERT_FALSE(band_of(envelope, std::numeric_limits<double>::quiet_NaN(), untouched));
  TOBSV_ASSERT_FALSE(
      band_of(envelope, std::numeric_limits<double>::infinity(), untouched));
  TOBSV_ASSERT_TRUE(untouched == ThresholdLevel::kWarn);
}

TOBSV_TEST("property", "checked_integer_arithmetic_matches_manual_boundary_reasoning") {
  struct AddCase {
    std::int64_t a;
    std::int64_t b;
    bool fits;
    std::int64_t expected;
  };
  const std::int64_t max = std::numeric_limits<std::int64_t>::max();
  const std::int64_t min = std::numeric_limits<std::int64_t>::min();
  const AddCase additions[] = {
      {0, 0, true, 0},
      {1, -1, true, 0},
      {max, 0, true, max},
      {min, 0, true, min},
      {max, 1, false, 0},
      {min, -1, false, 0},
      {max, -1, true, max - 1},
      {min, 1, true, min + 1},
      {max, min, true, -1},
      {max / 2, max / 2, true, max - 1},
      {max / 2 + 1, max / 2 + 1, false, 0},
      {min / 2, min / 2, true, min},
      {min / 2 - 1, min / 2, false, 0},
  };
  for (const AddCase& entry : additions) {
    const auto sum = checked::add(entry.a, entry.b);
    TOBSV_ASSERT_TRUE(sum.has_value() == entry.fits);
    if (entry.fits) {
      TOBSV_ASSERT_EQ(*sum, entry.expected);
    }
  }

  const AddCase subtractions[] = {
      {0, 0, true, 0},
      {max, max, true, 0},
      {min, min, true, 0},
      {0, max, true, -max},
      {min, 1, false, 0},
      {max, -1, false, 0},
      {min, -1, true, min + 1},
      {min + 1, 1, true, min},
  };
  for (const AddCase& entry : subtractions) {
    const auto difference = checked::sub(entry.a, entry.b);
    TOBSV_ASSERT_TRUE(difference.has_value() == entry.fits);
    if (entry.fits) {
      TOBSV_ASSERT_EQ(*difference, entry.expected);
      // Subtraction is the inverse of addition wherever both are defined: an algebraic identity is
      // an independent check on the sign handling.
      const auto round_trip = checked::add(*difference, entry.b);
      TOBSV_ASSERT_TRUE(round_trip.has_value());
      TOBSV_ASSERT_EQ(*round_trip, entry.a);
    }
  }

  // Values that cannot overflow by construction must always agree with the plain operator.
  Generator generator(0xA55ULL);
  for (int iteration = 0; iteration < 400; ++iteration) {
    const std::int64_t a = static_cast<std::int64_t>(generator.next_u64() % 2000000001ULL) - 1000000000;
    const std::int64_t b = static_cast<std::int64_t>(generator.next_u64() % 2000000001ULL) - 1000000000;
    const auto sum = checked::add(a, b);
    TOBSV_ASSERT_TRUE(sum.has_value());
    TOBSV_ASSERT_EQ(*sum, a + b);
    const auto difference = checked::sub(a, b);
    TOBSV_ASSERT_TRUE(difference.has_value());
    TOBSV_ASSERT_EQ(*difference, a - b);
    const auto product = checked::mul(a, b);
    TOBSV_ASSERT_TRUE(product.has_value());
    TOBSV_ASSERT_EQ(*product, a * b);
  }
}

TOBSV_TEST("property", "narrow_is_exact_whenever_it_succeeds") {
  Generator generator(0x4E41ULL);
  const std::int64_t int32_low = std::numeric_limits<std::int32_t>::min();
  const std::int64_t int32_high = std::numeric_limits<std::int32_t>::max();
  for (int iteration = 0; iteration < 500; ++iteration) {
    const std::int64_t value = generator.next_i64();
    const auto narrowed = checked::narrow<std::int32_t>(value);
    const bool fits = value >= int32_low && value <= int32_high;
    TOBSV_ASSERT_TRUE(narrowed.has_value() == fits);
    if (narrowed.has_value()) {
      TOBSV_ASSERT_EQ(static_cast<std::int64_t>(*narrowed), value);
    }
    const auto unsigned_narrowed = checked::narrow<std::uint32_t>(value);
    const bool unsigned_fits = value >= 0 && value <= 0xFFFFFFFFLL;
    TOBSV_ASSERT_TRUE(unsigned_narrowed.has_value() == unsigned_fits);
    if (unsigned_narrowed.has_value()) {
      TOBSV_ASSERT_EQ(static_cast<std::int64_t>(*unsigned_narrowed), value);
    }
    // The size conversion is the same range check with a different target width, so it must return
    // the value unchanged wherever the value is representable at all.
    if (value >= 0 && value <= 0xFFFFFFFFLL) {
      const auto as_size = checked::size_from_u64(static_cast<std::uint64_t>(value));
      TOBSV_ASSERT_TRUE(as_size.has_value());
      TOBSV_ASSERT_EQ(*as_size, static_cast<std::size_t>(value));
    }
  }
}

TOBSV_TEST("property", "counters_never_decrease_under_repeated_increment") {
  Generator generator(0xC0FFEEULL);
  for (int iteration = 0; iteration < 200; ++iteration) {
    Epoch counter = Epoch::from(generator.next_u64() % 1000000ULL);
    std::uint64_t previous = counter.value();
    for (int step = 0; step < 5; ++step) {
      const Epoch advanced = counter.next();
      const std::uint64_t value = advanced.value();
      // Saturation is the only permitted outcome other than an exact increment; a wrap would make a
      // saturated writer look like a brand new one.
      TOBSV_ASSERT_TRUE(value == previous || value == previous + 1);
      TOBSV_ASSERT_TRUE(value >= previous);
      TOBSV_ASSERT_TRUE(advanced >= counter);
      previous = value;
      counter = advanced;
    }
  }
  // At the ceiling the increment is the identity: a saturated writer never wraps onto a value a
  // reader would classify as brand new evidence.
  const Epoch ceiling = Epoch::from(std::numeric_limits<std::uint64_t>::max());
  TOBSV_ASSERT_TRUE(ceiling.next() == ceiling);
  TOBSV_ASSERT_TRUE(ceiling.next().next() == ceiling);
}

TOBSV_TEST("property", "json_dump_parse_dump_is_a_fixed_point") {
  const Limits limits;
  Generator generator(0x1500ULL);
  for (int iteration = 0; iteration < 150; ++iteration) {
    const JsonValue value = generated_value(generator, 0);
    const Result<std::string> first = value.dump_compact();
    TOBSV_ASSERT_TRUE(first.ok());
    const Result<JsonValue> parsed = JsonValue::parse(first.value(), limits);
    TOBSV_ASSERT_TRUE(parsed.ok());
    const Result<std::string> second = parsed.value().dump_compact();
    TOBSV_ASSERT_TRUE(second.ok());
    // A canonical encoding that is not a fixed point would make two readers disagree about a stored
    // record even though they parsed the same bytes.
    TOBSV_ASSERT_EQ(second.value(), first.value());
  }

  // Unicode escapes survive the round trip as their encoded bytes and re-encode identically.
  const Result<JsonValue> escaped =
      JsonValue::parse("{\"key\":\"\\u00e9\\ud83d\\ude00\"}", limits);
  TOBSV_ASSERT_TRUE(escaped.ok());
  const Result<std::string> text = escaped.value().dump_compact();
  TOBSV_ASSERT_TRUE(text.ok());
  const Result<JsonValue> again = JsonValue::parse(text.value(), limits);
  TOBSV_ASSERT_TRUE(again.ok());
  const Result<std::string> second_text = again.value().dump_compact();
  TOBSV_ASSERT_TRUE(second_text.ok());
  TOBSV_ASSERT_EQ(second_text.value(), text.value());
  TOBSV_ASSERT_TRUE(text.value().find("\\u") == std::string::npos);
}

TOBSV_TEST("property", "timestamp_text_round_trips_for_generated_instants") {
  Generator generator(0x71E5ULL);
  for (int iteration = 0; iteration < 400; ++iteration) {
    const std::int64_t raw = generator.next_i64();
    // The unset sentinel is not a timestamp; every other nanosecond value must survive the text
    // form, because the durable record carries the text and not the number.
    const std::int64_t nanos = raw == kUnsetUnixNanos ? raw + 1 : raw;
    const Timestamp stamp = Timestamp::from_unix_nanos(nanos);
    TOBSV_ASSERT_TRUE(stamp.is_set());
    const std::string text = stamp.to_string();
    TOBSV_ASSERT_EQ(text.size(), static_cast<std::size_t>(30));
    TOBSV_ASSERT_EQ(text[10], 'T');
    TOBSV_ASSERT_EQ(text[19], '.');
    TOBSV_ASSERT_EQ(text[29], 'Z');
    const Result<Timestamp> parsed = Timestamp::parse(text);
    TOBSV_ASSERT_TRUE(parsed.ok());
    TOBSV_ASSERT_TRUE(parsed.value().unix_nanos() == nanos);
    TOBSV_ASSERT_TRUE(parsed.value().to_string() == text);
  }
}

TOBSV_TEST("property", "fence_tracker_never_accepts_a_decreasing_position") {
  const SourceId source = source_id("prop-fence-source");
  Generator generator(0xF3A1ULL);
  FenceTracker tracker(source);
  Fence high_water;
  bool have_high_water = false;
  std::size_t accepted = 0;
  std::size_t refused = 0;

  const auto strictly_newer = [](const Fence& candidate, const Fence& mark) {
    if (candidate.epoch != mark.epoch) return candidate.epoch > mark.epoch;
    if (candidate.generation != mark.generation) return candidate.generation > mark.generation;
    if (candidate.revision != mark.revision) return candidate.revision > mark.revision;
    return candidate.sequence > mark.sequence;
  };

  for (int iteration = 0; iteration < 400; ++iteration) {
    const Fence candidate =
        make_fence(source, 1 + generator.next_u64() % 8U, 1 + generator.next_u64() % 3U,
                   1 + generator.next_u64() % 3U, 1 + generator.next_u64() % 8U);
    const bool same_content = generator.next_u64() % 2U == 0U;
    const Status observed = tracker.observe(candidate, same_content);
    if (observed.ok()) {
      ++accepted;
      if (have_high_water) {
        // An accepted fence either raises the outermost component that differs, or repeats the
        // position that is already there; it may never be older in any component.
        TOBSV_ASSERT_TRUE(strictly_newer(candidate, high_water) ||
                          candidate.to_string() == high_water.to_string());
      }
      TOBSV_ASSERT_TRUE(tracker.high_water().to_string() == candidate.to_string());
      high_water = tracker.high_water();
      have_high_water = true;
    } else {
      ++refused;
      if (have_high_water) {
        // A refusal is never a newer position: the mark only ever moves forward.
        TOBSV_ASSERT_FALSE(strictly_newer(candidate, high_water));
        TOBSV_ASSERT_TRUE(tracker.high_water().to_string() == high_water.to_string());
      }
    }
  }
  // Both outcomes must actually occur, or the property above would be vacuous.
  TOBSV_ASSERT_TRUE(accepted > 0);
  TOBSV_ASSERT_TRUE(refused > 0);
  TOBSV_ASSERT_EQ(accepted + refused, static_cast<std::size_t>(400));
}

TOBSV_TEST("property", "episode_grouping_is_independent_of_insertion_order") {
  const Limits limits;
  ThermalTopology topology;
  TOBSV_ASSERT_OK(topology.add_edge(
      make_adjacency(entity_id("prop-node-1"), entity_id("prop-node-2"),
                     AdjacencyKind::kSharedEnclosure),
      limits));
  TOBSV_ASSERT_OK(topology.add_edge(
      make_adjacency(entity_id("prop-node-2"), entity_id("prop-node-3"),
                     AdjacencyKind::kSharedEnclosure),
      limits));
  TOBSV_ASSERT_OK(topology.add_edge(
      make_adjacency(entity_id("prop-node-4"), entity_id("prop-node-5"),
                     AdjacencyKind::kAirflowPathProximity),
      limits));
  // No supported coupling is recorded: the episodes below are joined by declared adjacency alone.
  const CouplingGraph coupling;

  std::vector<HeadroomReport> reports;
  for (int index = 1; index <= 6; ++index) {
    reports.push_back(hot_report("prop-node-" + std::to_string(index), 45.0 + index,
                                 static_cast<std::uint64_t>(index)));
  }

  Generator generator(0xD1CEULL);
  std::string reference;
  std::size_t episodes = 0;
  std::size_t ungrouped = 0;
  for (int shuffle = 0; shuffle < 24; ++shuffle) {
    std::vector<HeadroomReport> shuffled = reports;
    for (std::size_t index = shuffled.size(); index > 1; --index) {
      const std::size_t other = static_cast<std::size_t>(generator.next_u64() % index);
      std::swap(shuffled[index - 1], shuffled[other]);
    }
    const Result<HotspotResult> grouped =
        group_hotspots(shuffled, topology, coupling, HotspotPolicy{}, limits);
    TOBSV_ASSERT_TRUE(grouped.ok());
    const Result<std::string> text = grouped.value().to_json().dump_compact();
    TOBSV_ASSERT_TRUE(text.ok());
    if (shuffle == 0) {
      reference = text.value();
      episodes = grouped.value().episodes.size();
      ungrouped = grouped.value().ungrouped.size();
      TOBSV_ASSERT_EQ(grouped.value().hot_subjects, static_cast<std::size_t>(6));
      // Three components: the chain of three, the pair and the singleton. The singleton is reported
      // both as an episode of one and as ungrouped, because an isolated hot entity is a place heat is
      // accumulating and a gap in the connectivity evidence at the same time.
      TOBSV_ASSERT_EQ(episodes, static_cast<std::size_t>(3));
      TOBSV_ASSERT_EQ(ungrouped, static_cast<std::size_t>(1));
      for (const HotspotEpisode& episode : grouped.value().episodes) {
        // The identity is derived from the members, so the same members must always hash to the
        // same episode.
        TOBSV_ASSERT_TRUE(episode.id == compute_episode_id(episode.members, episode.threshold));
      }
    } else {
      // The whole report, including every episode identity, is a function of the evidence and not
      // of the order in which the evidence was handed over.
      TOBSV_ASSERT_EQ(text.value(), reference);
    }
  }
}

TOBSV_TEST("property", "stable_digest_changes_whenever_one_absorbed_byte_changes") {
  Generator generator(0x516EULL);
  for (int iteration = 0; iteration < 200; ++iteration) {
    const std::uint64_t seed = generator.next_u64();
    std::string text;
    for (int index = 0; index < 8; ++index) {
      text.push_back(static_cast<char>((seed >> (index * 8)) & 0xFFULL));
    }
    std::string flipped = text;
    const std::size_t position = static_cast<std::size_t>(generator.next_u64() % 8U);
    const unsigned mask = 1U << static_cast<unsigned>(generator.next_u64() % 8U);
    flipped[position] =
        static_cast<char>(static_cast<unsigned char>(flipped[position]) ^ mask);
    // A digest that ignored a byte would let two different observations share one identity.
    TOBSV_ASSERT_TRUE(stable_hash(text) != stable_hash(flipped));
    StableDigest left;
    left.absorb_text(text);
    StableDigest right;
    right.absorb_text(flipped);
    TOBSV_ASSERT_TRUE(left.value() != right.value());
  }
}
