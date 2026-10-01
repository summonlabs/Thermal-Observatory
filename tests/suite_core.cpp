// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

#include "test_framework.hpp"
#include "test_support.hpp"

using namespace tobsv;
using namespace tobstest;

namespace {

// The framework status assertions are defined over Status. A Result carries its Status through
// error(), so this adapter lets a Result be checked with the very same macros and messages.
template <class T>
Status status_of(const Result<T>& result) {
  return result.error();
}

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::uint64_t kUint64Max = std::numeric_limits<std::uint64_t>::max();

}  // namespace

TOBSV_TEST("core", "status_describe_is_deterministic_and_carries_every_field") {
  const Status success = Status::success();
  TOBSV_ASSERT_TRUE(success.ok());
  TOBSV_ASSERT_FALSE(success.failed());
  TOBSV_ASSERT_EQ(success.describe(), std::string("ok"));
  TOBSV_ASSERT_TRUE(success.code() == ErrorCode::kNone);

  const Status failure = Status::failure(ErrorCode::kLimitExceeded, "citations exceed the bound");
  TOBSV_ASSERT_TRUE(failure.failed());
  TOBSV_ASSERT_EQ(failure.detail(), std::string("citations exceed the bound"));
  TOBSV_ASSERT_EQ(failure.describe(), std::string("limit_exceeded: citations exceed the bound"));

  // describe() is the single line a caller logs, so two Status values that compare equal must render
  // equal, and a different detail must never render the same line.
  const Status same = Status::failure(ErrorCode::kLimitExceeded, "citations exceed the bound");
  TOBSV_ASSERT_TRUE(failure == same);
  TOBSV_ASSERT_EQ(failure.describe(), same.describe());
  const Status different = Status::failure(ErrorCode::kLimitExceeded, "entities exceed the bound");
  TOBSV_ASSERT_TRUE(failure != different);
  TOBSV_ASSERT_TRUE(failure.describe() != different.describe());
  const Status other_code = Status::failure(ErrorCode::kOutOfRange, "citations exceed the bound");
  TOBSV_ASSERT_TRUE(failure != other_code);
  TOBSV_ASSERT_TRUE(failure.describe() != other_code.describe());

  // Every code has a stable spelling; an unrecognised one would make the line unparseable.
  TOBSV_ASSERT_EQ(std::string(to_string(ErrorCode::kIntegrityFailure)), std::string("integrity_failure"));
  TOBSV_ASSERT_EQ(std::string(to_string(ErrorCode::kStaleEpoch)), std::string("stale_epoch"));
  TOBSV_ASSERT_TRUE(!std::string(to_string(ErrorCode::kQueueFull)).empty());
}

TOBSV_TEST("core", "result_carries_either_a_value_or_a_status") {
  const Result<int> good(7);
  TOBSV_ASSERT_TRUE(good.ok());
  TOBSV_ASSERT_TRUE(static_cast<bool>(good));
  TOBSV_ASSERT_EQ(good.value(), 7);
  TOBSV_ASSERT_EQ(*good, 7);
  TOBSV_ASSERT_TRUE(good.error().ok());

  const Result<int> bad = Status::failure(ErrorCode::kNotFound, "no such record");
  TOBSV_ASSERT_FALSE(bad.ok());
  TOBSV_ASSERT_FALSE(static_cast<bool>(bad));
  TOBSV_ASSERT_TRUE(bad.error().code() == ErrorCode::kNotFound);
  TOBSV_ASSERT_EQ(bad.failure_detail(), std::string("no such record"));
  // Reading the value of a failed Result is defined and returns the placeholder: the accessor never
  // throws, because a throw out of an evidence path would be worse than a documented zero.
  TOBSV_ASSERT_EQ(bad.value(), 0);

  const Result<std::string> text = std::string("envelope");
  TOBSV_ASSERT_TRUE(text.ok());
  TOBSV_ASSERT_EQ(text.value(), std::string("envelope"));

  const VoidResult void_ok;
  TOBSV_ASSERT_TRUE(void_ok.ok());
  const VoidResult void_bad = Status::failure(ErrorCode::kRefused, "declined");
  TOBSV_ASSERT_FALSE(void_bad.ok());
  TOBSV_ASSERT_EQ(void_bad.error().describe(), std::string("refused: declined"));
}

TOBSV_TEST("core", "checked_integer_arithmetic_reports_overflow_instead_of_wrapping") {
  TOBSV_ASSERT_FALSE(checked::add<std::int64_t>(kInt64Max, 1).has_value());
  TOBSV_ASSERT_FALSE(checked::add<std::int64_t>(kInt64Min, -1).has_value());
  TOBSV_ASSERT_TRUE(checked::add<std::int64_t>(kInt64Max, -1).value() == kInt64Max - 1);
  TOBSV_ASSERT_TRUE(checked::add<std::int64_t>(kInt64Min, kInt64Max).value() == -1);
  TOBSV_ASSERT_TRUE(checked::add<std::int64_t>(0, 0).value() == 0);

  TOBSV_ASSERT_FALSE(checked::sub<std::int64_t>(kInt64Min, 1).has_value());
  TOBSV_ASSERT_FALSE(checked::sub<std::int64_t>(0, kInt64Min).has_value());
  TOBSV_ASSERT_TRUE(checked::sub<std::int64_t>(kInt64Max, kInt64Max).value() == 0);
  TOBSV_ASSERT_TRUE(checked::sub<std::int64_t>(kInt64Min, kInt64Min).value() == 0);

  // The most negative value has no positive counterpart, so negating it is an overflow rather than a
  // silent saturation.
  TOBSV_ASSERT_FALSE(checked::mul<std::int64_t>(kInt64Min, -1).has_value());
  TOBSV_ASSERT_FALSE(checked::mul<std::int64_t>(-1, kInt64Min).has_value());
  TOBSV_ASSERT_TRUE(checked::mul<std::int64_t>(kInt64Min, 1).value() == kInt64Min);
  TOBSV_ASSERT_TRUE(checked::mul<std::int64_t>(0, kInt64Min).value() == 0);
  // 3037000499 squared fits by exactly one unit of headroom; the next integer does not.
  TOBSV_ASSERT_TRUE(checked::mul<std::int64_t>(3037000499LL, 3037000499LL).has_value());
  TOBSV_ASSERT_FALSE(checked::mul<std::int64_t>(3037000500LL, 3037000500LL).has_value());

  TOBSV_ASSERT_FALSE(checked::add<std::uint64_t>(kUint64Max, 1U).has_value());
  TOBSV_ASSERT_FALSE(checked::sub<std::uint64_t>(0U, 1U).has_value());
  TOBSV_ASSERT_FALSE(checked::mul<std::uint64_t>(1ULL << 32U, 1ULL << 32U).has_value());
  TOBSV_ASSERT_TRUE(checked::mul<std::uint64_t>(kUint64Max, 1U).value() == kUint64Max);
}

TOBSV_TEST("core", "checked_narrow_and_size_from_u64_are_range_exact") {
  // Every returned value is held in a named object before it is compared: an assertion macro binds
  // a reference to its operand, and a reference into a temporary optional would dangle.
  const auto highest_byte = checked::narrow<std::uint8_t>(255);
  TOBSV_ASSERT_TRUE(highest_byte.has_value());
  TOBSV_ASSERT_EQ(*highest_byte, static_cast<std::uint8_t>(255));
  const auto lowest_byte = checked::narrow<std::int8_t>(-128);
  TOBSV_ASSERT_TRUE(lowest_byte.has_value());
  TOBSV_ASSERT_EQ(*lowest_byte, static_cast<std::int8_t>(-128));
  const auto top_byte = checked::narrow<std::int8_t>(127);
  TOBSV_ASSERT_TRUE(top_byte.has_value());
  TOBSV_ASSERT_EQ(*top_byte, static_cast<std::int8_t>(127));
  TOBSV_ASSERT_FALSE(checked::narrow<std::uint8_t>(256).has_value());
  TOBSV_ASSERT_FALSE(checked::narrow<std::uint8_t>(-1).has_value());
  TOBSV_ASSERT_FALSE(checked::narrow<std::int8_t>(128).has_value());
  TOBSV_ASSERT_FALSE(checked::narrow<std::int8_t>(-129).has_value());
  TOBSV_ASSERT_TRUE(checked::narrow<std::uint32_t>(static_cast<std::int64_t>(1) << 31).has_value());
  TOBSV_ASSERT_FALSE(checked::narrow<std::uint32_t>(static_cast<std::int64_t>(1) << 32).has_value());
  TOBSV_ASSERT_FALSE(checked::narrow<std::int64_t>(kUint64Max).has_value());

  const auto size_value = checked::size_from_u64(4096);
  TOBSV_ASSERT_TRUE(size_value.has_value());
  TOBSV_ASSERT_EQ(*size_value, static_cast<std::size_t>(4096));
  // On a 64 bit size_t every uint64 value fits, so the largest value is a success rather than a
  // failure; a value that needed more bits than size_t offers is the interesting rejection.
  TOBSV_ASSERT_TRUE(checked::size_from_u64(kUint64Max).has_value());
  const auto size_sum = checked::add_size(2, 3);
  TOBSV_ASSERT_TRUE(size_sum.has_value());
  TOBSV_ASSERT_EQ(*size_sum, static_cast<std::size_t>(5));
  TOBSV_ASSERT_FALSE(checked::add_size(std::numeric_limits<std::size_t>::max(), 1).has_value());
}

TOBSV_TEST("core", "checked_floating_point_refuses_non_finite_and_zero_denominators") {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  const double largest = std::numeric_limits<double>::max();

  TOBSV_ASSERT_TRUE(checked::is_finite(0.0));
  TOBSV_ASSERT_FALSE(checked::is_finite(nan));
  TOBSV_ASSERT_FALSE(checked::is_finite(infinity));
  TOBSV_ASSERT_FALSE(checked::is_finite(-infinity));
  TOBSV_ASSERT_TRUE(checked::is_finite_positive(1.0e-300));
  TOBSV_ASSERT_FALSE(checked::is_finite_positive(0.0));
  TOBSV_ASSERT_TRUE(checked::is_finite_non_negative(0.0));
  TOBSV_ASSERT_FALSE(checked::is_finite_non_negative(-0.001));
  TOBSV_ASSERT_TRUE(checked::in_range(0.5, 0.0, 1.0));
  TOBSV_ASSERT_FALSE(checked::in_range(1.5, 0.0, 1.0));
  TOBSV_ASSERT_FALSE(checked::in_range(nan, 0.0, 1.0));

  const auto sum = checked::add(1.5, 2.25);
  TOBSV_ASSERT_TRUE(sum.has_value());
  TOBSV_ASSERT_EQ(*sum, 3.75);
  TOBSV_ASSERT_FALSE(checked::add(nan, 1.0).has_value());
  TOBSV_ASSERT_FALSE(checked::add(1.0, infinity).has_value());
  TOBSV_ASSERT_FALSE(checked::add(infinity, -infinity).has_value());
  // A sum that leaves the finite range is refused even though both operands are finite.
  TOBSV_ASSERT_FALSE(checked::add(largest, largest).has_value());

  const auto difference = checked::sub(2.25, 1.5);
  TOBSV_ASSERT_TRUE(difference.has_value());
  TOBSV_ASSERT_EQ(*difference, 0.75);
  TOBSV_ASSERT_FALSE(checked::sub(1.0, infinity).has_value());
  TOBSV_ASSERT_FALSE(checked::sub(-largest, largest).has_value());

  const auto quotient = checked::div(3.0, 2.0);
  TOBSV_ASSERT_TRUE(quotient.has_value());
  TOBSV_ASSERT_EQ(*quotient, 1.5);
  TOBSV_ASSERT_FALSE(checked::div(1.0, 0.0).has_value());
  TOBSV_ASSERT_FALSE(checked::div(0.0, 0.0).has_value());
  TOBSV_ASSERT_FALSE(checked::div(1.0, nan).has_value());
  TOBSV_ASSERT_FALSE(checked::div(infinity, 1.0).has_value());

  TOBSV_ASSERT_EQ(checked::clamp(nan, 1.0, 2.0), 1.0);
  TOBSV_ASSERT_EQ(checked::clamp(5.0, 1.0, 2.0), 2.0);
  TOBSV_ASSERT_EQ(checked::clamp(-5.0, 1.0, 2.0), 1.0);
  TOBSV_ASSERT_EQ(checked::clamp(1.5, 1.0, 2.0), 1.5);
  const auto magnitude = checked::abs(-3.5);
  TOBSV_ASSERT_TRUE(magnitude.has_value());
  TOBSV_ASSERT_EQ(*magnitude, 3.5);
  TOBSV_ASSERT_FALSE(checked::abs(-infinity).has_value());
}

TOBSV_TEST("core", "stable_digest_separates_text_numbers_and_signed_zero") {
  // The terminator makes concatenation unambiguous: "ab" then "c" is not "a" then "bc".
  StableDigest left;
  left.absorb_text("ab");
  left.absorb_text("c");
  StableDigest right;
  right.absorb_text("a");
  right.absorb_text("bc");
  TOBSV_ASSERT_TRUE(left.value() != right.value());

  // A one byte text is not the number with that byte value: the two absorb different terminators.
  StableDigest number;
  number.absorb_number(1);
  StableDigest text;
  text.absorb_text(std::string_view("\x01", 1));
  TOBSV_ASSERT_TRUE(number.value() != text.value());

  // The bit pattern is absorbed verbatim, which is what makes a signed zero distinguishable and a
  // negative temperature impossible to confuse with its magnitude.
  StableDigest zero;
  zero.absorb_real(0.0);
  StableDigest negative_zero;
  negative_zero.absorb_real(-0.0);
  TOBSV_ASSERT_TRUE(zero.value() != negative_zero.value());
  StableDigest positive;
  positive.absorb_real(1.5);
  TOBSV_ASSERT_TRUE(positive.value() != negative_zero.value());

  StableDigest flag_on;
  flag_on.absorb_flag(true);
  StableDigest flag_off;
  flag_off.absorb_flag(false);
  TOBSV_ASSERT_TRUE(flag_on.value() != flag_off.value());

  const StableDigest fresh;
  TOBSV_ASSERT_EQ(fresh.value(), StableDigest::kOffsetBasis);
  TOBSV_ASSERT_EQ(fresh.hex().size(), static_cast<std::size_t>(16));
  TOBSV_ASSERT_TRUE(stable_hash("a") != stable_hash("b"));
  // Order matters when combining: a digest of the pair is not symmetric in its arguments.
  TOBSV_ASSERT_TRUE(hash_combine(1, 2) != hash_combine(2, 1));
}

TOBSV_TEST("core", "hex_u64_is_fixed_width_lowercase") {
  TOBSV_ASSERT_EQ(hex_u64(0), std::string("0000000000000000"));
  TOBSV_ASSERT_EQ(hex_u64(0xFULL), std::string("000000000000000f"));
  TOBSV_ASSERT_EQ(hex_u64(0xABCDEF0123456789ULL), std::string("abcdef0123456789"));
  TOBSV_ASSERT_EQ(hex_u64(kUint64Max), std::string("ffffffffffffffff"));
  // The fixed width is what lets a derived identity be compared as text without padding logic.
  TOBSV_ASSERT_EQ(hex_u64(1).size(), static_cast<std::size_t>(16));
  TOBSV_ASSERT_EQ(hex_u64(kUint64Max).size(), static_cast<std::size_t>(16));
  StableDigest digest;
  digest.absorb_text("tobsv");
  TOBSV_ASSERT_EQ(digest.hex(), hex_u64(digest.value()));
}

TOBSV_TEST("core", "crc32c_matches_known_vectors") {
  // The empty message and the standard check message are fixed points of CRC-32C: if either drifts,
  // the table or the final inversion has changed and every stored checksum is worthless.
  TOBSV_ASSERT_EQ(crc32c(""), 0x00000000U);
  TOBSV_ASSERT_EQ(crc32c("123456789"), 0xE3069283U);
  TOBSV_ASSERT_EQ(crc32c("a"), 0xC1D04330U);
  TOBSV_ASSERT_EQ(crc32c("thermal-observatory selfcheck"), 0x5EEF236DU);
  TOBSV_ASSERT_TRUE(crc32c("a") != crc32c("b"));
  TOBSV_ASSERT_TRUE(crc32c("ab") != crc32c("ba"));
}

TOBSV_TEST("core", "crc32c_incremental_updates_equal_one_shot_computation") {
  const std::string text = "the quick brown fox jumps over the lazy dog";
  Crc32c incremental;
  incremental.update(std::string_view(text).substr(0, 10));
  incremental.update(std::string_view(text).substr(10, 15));
  incremental.update(std::string_view(text).substr(25));
  TOBSV_ASSERT_EQ(incremental.value(), crc32c(text));
  TOBSV_ASSERT_EQ(Crc32c::compute(text), crc32c(text));

  // The word helpers must lay bytes down in the same order the byte stream would have carried them.
  Crc32c words;
  words.update_u8(1U);
  words.update_u32(0x05040302U);
  words.update_u64(0x0D0C0B0A09080706ULL);
  const std::uint8_t raw[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  Crc32c bytes;
  bytes.update(raw, sizeof(raw));
  TOBSV_ASSERT_EQ(words.value(), bytes.value());
}

TOBSV_TEST("core", "strong_id_parse_enforces_the_identity_grammar") {
  TOBSV_ASSERT_FAILS_WITH(status_of(EntityId::parse("")), ErrorCode::kInvalidArgument);

  // The length bound is inclusive: 96 characters are accepted and 97 are not, so an identity can
  // never be used as an unbounded payload channel.
  const std::string longest = "e" + std::string(kMaxIdentifierLength - 1, 'x');
  TOBSV_ASSERT_EQ(longest.size(), static_cast<std::size_t>(96));
  const Result<EntityId> accepted = EntityId::parse(longest);
  TOBSV_ASSERT_TRUE(accepted.ok());
  TOBSV_ASSERT_EQ(accepted.value().value().size(), static_cast<std::size_t>(96));
  const std::string too_long = "e" + std::string(kMaxIdentifierLength, 'x');
  TOBSV_ASSERT_FAILS_WITH(status_of(EntityId::parse(too_long)), ErrorCode::kLimitExceeded);

  // The first character must be alphanumeric.
  TOBSV_ASSERT_FAILS_WITH(status_of(EntityId::parse("-node")), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(status_of(EntityId::parse(" node")), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(status_of(EntityId::parse("_node")), ErrorCode::kInvalidArgument);
  // Whitespace, quotes and backslashes could break the framing of a textual or JSON record.
  TOBSV_ASSERT_FAILS_WITH(status_of(EntityId::parse("node 01")), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(status_of(EntityId::parse("node\"01")), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(status_of(EntityId::parse("node\\01")), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(status_of(EntityId::parse("node\n01")), ErrorCode::kInvalidArgument);

  // Every permitted continuation character is accepted together.
  const Result<EntityId> punctuated = EntityId::parse("Node-01.rack:3@hall/a+b#c_d");
  TOBSV_ASSERT_TRUE(punctuated.ok());
  TOBSV_ASSERT_EQ(punctuated.value().value(), std::string("Node-01.rack:3@hall/a+b#c_d"));
  TOBSV_ASSERT_TRUE(EntityId::parse("1").ok());
  TOBSV_ASSERT_TRUE(EntityId::parse("9").ok());
  TOBSV_ASSERT_TRUE(EntityId::parse("Z").ok());
}

TOBSV_TEST("core", "strong_id_from_digest_is_prefixed_and_validates") {
  const EntityId zero = EntityId::from_digest(0);
  TOBSV_ASSERT_TRUE(zero.is_set());
  TOBSV_ASSERT_EQ(zero.value(), std::string("ent-0000000000000000"));
  TOBSV_ASSERT_EQ(zero.value().size(), static_cast<std::size_t>(20));
  TOBSV_ASSERT_EQ(zero.to_string(), zero.value());
  TOBSV_ASSERT_EQ(zero.kind(), std::string_view("entity"));
  // A derived identity is a legal identity in its own right, which is what lets it be stored and
  // compared as one.
  TOBSV_ASSERT_OK(validate_identity_token(zero.value(), EntityIdTag::kKind));

  const ObservationId observation = ObservationId::from_digest(0xDEADBEEFCAFEBABEULL);
  TOBSV_ASSERT_EQ(observation.value(), std::string("obs-deadbeefcafebabe"));
  const CouplingId coupling = CouplingId::from_digest(kUint64Max);
  TOBSV_ASSERT_EQ(coupling.value(), std::string("cpl-ffffffffffffffff"));
  TOBSV_ASSERT_TRUE(EntityId::from_digest(1) != EntityId::from_digest(2));
  TOBSV_ASSERT_TRUE(EntityId::from_digest(1) < EntityId::from_digest(2));

  TOBSV_ASSERT_FAILS_WITH(validate_identity_token("", "entity"), ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(validate_identity_token("has space", "entity"),
                          ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(validate_identity_token(std::string(97, 'x'), "entity"),
                          ErrorCode::kLimitExceeded);
}

TOBSV_TEST("core", "duration_scaling_overflows_are_reported_and_magnitude_saturates") {
  const Result<Duration> micros = Duration::from_micros(1);
  TOBSV_ASSERT_TRUE(micros.ok());
  TOBSV_ASSERT_EQ(micros.value().nanos(), static_cast<Nanos>(1000));
  const Result<Duration> millis = Duration::from_millis(-2);
  TOBSV_ASSERT_TRUE(millis.ok());
  TOBSV_ASSERT_EQ(millis.value().nanos(), static_cast<Nanos>(-2000000));
  TOBSV_ASSERT_TRUE(Duration::from_seconds(-1.5).ok());
  TOBSV_ASSERT_EQ(Duration::from_seconds(-1.5).value().nanos(), static_cast<Nanos>(-1500000000));

  // Scaling is where a duration silently wraps if it is not checked.
  TOBSV_ASSERT_FALSE(Duration::from_micros(kInt64Max).ok());
  TOBSV_ASSERT_FALSE(Duration::from_millis(kInt64Max).ok());
  TOBSV_ASSERT_FALSE(Duration::from_micros(kInt64Min).ok());
  TOBSV_ASSERT_FALSE(Duration::from_millis(kInt64Min).ok());
  TOBSV_ASSERT_TRUE(Duration::from_micros(kInt64Max / 1000).ok());
  TOBSV_ASSERT_FALSE(Duration::from_seconds(std::numeric_limits<double>::quiet_NaN()).ok());
  TOBSV_ASSERT_FALSE(Duration::from_seconds(std::numeric_limits<double>::infinity()).ok());
  TOBSV_ASSERT_FALSE(Duration::from_seconds(1.0e300).ok());

  // The most negative duration has no positive counterpart, so its magnitude clamps to the largest
  // duration instead of wrapping to a negative value that would read as very small.
  TOBSV_ASSERT_TRUE(Duration::min().magnitude() == Duration::max());
  TOBSV_ASSERT_TRUE(Duration::from_nanos(-5).magnitude().nanos() == 5);
  TOBSV_ASSERT_TRUE(Duration::zero().is_zero());
  TOBSV_ASSERT_FALSE(Duration::zero().is_positive());
  TOBSV_ASSERT_TRUE(Duration::from_nanos(-1).is_negative());

  TOBSV_ASSERT_FALSE((Duration::max() + Duration::from_nanos(1)).ok());
  TOBSV_ASSERT_FALSE((Duration::min() - Duration::from_nanos(1)).ok());
  TOBSV_ASSERT_TRUE(saturating_sub(Duration::max(), Duration::from_nanos(-1)) == Duration::max());
  TOBSV_ASSERT_TRUE(saturating_sub(Duration::from_nanos(5), Duration::from_nanos(8)).nanos() == -3);

  // A future observation keeps its negative age so that a clock problem stays visible.
  const Timestamp observed = instant("2026-02-14T09:31:07Z");
  const Timestamp evaluated = instant("2026-02-14T09:31:00Z");
  TOBSV_ASSERT_EQ(age_of(observed, evaluated).nanos(), static_cast<Nanos>(-7000000000LL));
  TOBSV_ASSERT_TRUE(age_of(evaluated, observed).nanos() == 7000000000LL);
}

TOBSV_TEST("core", "timestamp_parse_is_strict_about_shape_and_fields") {
  const Result<Timestamp> whole = Timestamp::parse("2026-02-14T09:31:07Z");
  TOBSV_ASSERT_TRUE(whole.ok());
  TOBSV_ASSERT_EQ(whole.value().unix_nanos(), base_instant().unix_nanos());

  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("")), ErrorCode::kMalformedInput);
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026-02-14T09:31:07")),
                          ErrorCode::kMalformedInput);
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026-02-14T09:31:07ZZ")),
                          ErrorCode::kMalformedInput);
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026-02-14 09:31:07Z")),
                          ErrorCode::kMalformedInput);
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026/02/14T09:31:07Z")),
                          ErrorCode::kMalformedInput);
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026-02-14T09:31:07+00:00")),
                          ErrorCode::kMalformedInput);
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026-13-14T09:31:07Z")),
                          ErrorCode::kMalformedInput);
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026-02-00T09:31:07Z")),
                          ErrorCode::kMalformedInput);
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026-02-14T24:00:00Z")),
                          ErrorCode::kMalformedInput);
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026-02-14T09:60:07Z")),
                          ErrorCode::kMalformedInput);
  // A leap second cannot be represented in the nanosecond timeline, so it is refused rather than
  // folded into the following second.
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026-02-14T09:31:60Z")),
                          ErrorCode::kMalformedInput);
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026-02-14T09:31:07.Z")),
                          ErrorCode::kMalformedInput);
  TOBSV_ASSERT_FAILS_WITH(status_of(Timestamp::parse("2026-02-14T09:31:07.1234567890Z")),
                          ErrorCode::kMalformedInput);

  // Every fractional length from one to nine digits is accepted and scaled exactly.
  for (int count = 1; count <= 9; ++count) {
    const std::string text =
        "2026-02-14T09:31:07." + std::string(static_cast<std::size_t>(count), '7') + "Z";
    const Result<Timestamp> parsed = Timestamp::parse(text);
    TOBSV_ASSERT_TRUE(parsed.ok());
    Nanos fraction = 0;
    Nanos scale = 100000000;
    for (int index = 0; index < count; ++index) {
      fraction += 7 * scale;
      scale /= 10;
    }
    TOBSV_ASSERT_EQ(parsed.value().unix_nanos(), base_instant().unix_nanos() + fraction);
  }
}

TOBSV_TEST("core", "timestamp_text_round_trips_and_is_fixed_width") {
  const std::string text = base_instant().to_string();
  TOBSV_ASSERT_EQ(text, std::string("2026-02-14T09:31:07.000000000Z"));
  TOBSV_ASSERT_EQ(text.size(), static_cast<std::size_t>(30));
  const Result<Timestamp> reparsed = Timestamp::parse(text);
  TOBSV_ASSERT_TRUE(reparsed.ok());
  TOBSV_ASSERT_TRUE(reparsed.value() == base_instant());

  // An unset timestamp renders as a sentinel no parser accepts, so "no timestamp" can never be read
  // back as a real instant.
  TOBSV_ASSERT_EQ(Timestamp{}.to_string(), std::string("unset"));
  TOBSV_ASSERT_FALSE(Timestamp{}.is_set());
  TOBSV_ASSERT_FALSE(Timestamp::parse("unset").ok());
  // The sentinel is produced by the value, not by the arithmetic, and from_unix_seconds refuses to
  // land on it.
  TOBSV_ASSERT_FAILS_WITH(
      status_of(Timestamp::from_unix_seconds(static_cast<double>(kUnsetUnixNanos) / 1.0e9)),
      ErrorCode::kOutOfRange);

  const UnixNanos samples[] = {0,          1,           -1,          999999999,
                               -999999999, 1500000000123456789LL, kInt64Max,
                               kInt64Min + 1000000000LL};
  for (const UnixNanos nanos : samples) {
    const Timestamp stamp = Timestamp::from_unix_nanos(nanos);
    TOBSV_ASSERT_TRUE(stamp.is_set());
    const Result<Timestamp> parsed = Timestamp::parse(stamp.to_string());
    TOBSV_ASSERT_TRUE(parsed.ok());
    TOBSV_ASSERT_TRUE(parsed.value().unix_nanos() == nanos);
  }

  // The very bottom of the range is where the nanosecond arithmetic itself is the limit: the seconds
  // count multiplied by a billion leaves int64 before the fraction is added back, so the last second
  // before the minimum is refused instead of being silently rounded to a neighbouring instant.
  const Timestamp lowest = Timestamp::from_unix_nanos(kInt64Min + 1);
  TOBSV_ASSERT_TRUE(lowest.is_set());
  const Result<Timestamp> refused = Timestamp::parse(lowest.to_string());
  TOBSV_ASSERT_FALSE(refused.ok());
  TOBSV_ASSERT_TRUE(refused.error().code() == ErrorCode::kOutOfRange);
}

TOBSV_TEST("core", "format_real_is_the_shortest_round_tripping_form") {
  TOBSV_ASSERT_EQ(format_real(0.1), std::string("0.1"));
  TOBSV_ASSERT_EQ(format_real(1.0), std::string("1.0"));
  TOBSV_ASSERT_EQ(format_real(0.0), std::string("0.0"));
  TOBSV_ASSERT_EQ(format_real(-0.0), std::string("-0.0"));
  TOBSV_ASSERT_EQ(format_real(2.5), std::string("2.5"));
  TOBSV_ASSERT_EQ(format_real(-273.15), std::string("-273.15"));
  TOBSV_ASSERT_EQ(format_real(1.0e300), std::string("1e+300"));
  // A whole number still renders with a fractional part so that a reader never has to guess whether
  // the value was an integer or a real.
  TOBSV_ASSERT_TRUE(format_real(10.0).find('.') != std::string::npos);

  const double non_finite = std::numeric_limits<double>::quiet_NaN();
  TOBSV_ASSERT_EQ(format_real(non_finite), std::string("nan"));
  TOBSV_ASSERT_EQ(format_real(std::numeric_limits<double>::infinity()), std::string("inf"));
  TOBSV_ASSERT_EQ(format_real(-std::numeric_limits<double>::infinity()), std::string("-inf"));

  const double values[] = {0.1, 1.0 / 3.0, 1.0e-300, 1.7976931348623157e308,
                           1.2345678901234567e-17, -0.0, 2.2250738585072014e-308};
  for (const double value : values) {
    const std::string rendered = format_real(value);
    TOBSV_ASSERT_TRUE(std::strtod(rendered.c_str(), nullptr) == value);
  }

  TOBSV_ASSERT_EQ(format_seconds(Duration::from_nanos(1500000000)), std::string("1.5s"));
  TOBSV_ASSERT_EQ(format_seconds(Duration::zero()), std::string("0.0s"));
  TOBSV_ASSERT_EQ(join({"a", "b", "c"}, ", "), std::string("a, b, c"));
  TOBSV_ASSERT_EQ(join({}, ", "), std::string(""));
  TOBSV_ASSERT_EQ(join({"only"}, ", "), std::string("only"));
}

TOBSV_TEST("core", "limits_validate_rejects_zero_above_ceiling_and_inconsistent_bounds") {
  const Limits defaults;
  TOBSV_ASSERT_OK(defaults.validate());
  // The hard ceiling is inclusive: a bound exactly at it is a configuration, one above is a mistake.
  Limits at_ceiling = defaults;
  at_ceiling.max_entities = ceiling::kEntities;
  TOBSV_ASSERT_OK(at_ceiling.validate());

  Limits zero_bound = defaults;
  zero_bound.max_entities = 0;
  TOBSV_ASSERT_FAILS_WITH(zero_bound.validate(), ErrorCode::kInvalidArgument);

  Limits above_ceiling = defaults;
  above_ceiling.max_entities = ceiling::kEntities + 1;
  TOBSV_ASSERT_FAILS_WITH(above_ceiling.validate(), ErrorCode::kOutOfRange);

  Limits inconsistent = defaults;
  inconsistent.max_observations_per_subject = defaults.max_observations + 1;
  TOBSV_ASSERT_FAILS_WITH(inconsistent.validate(), ErrorCode::kInvalidArgument);

  Limits zero_window;
  zero_window.default_freshness_window = 0;
  TOBSV_ASSERT_FAILS_WITH(zero_window.validate(), ErrorCode::kInvalidArgument);

  Limits negative_window;
  negative_window.default_freshness_window = -1;
  TOBSV_ASSERT_FAILS_WITH(negative_window.validate(), ErrorCode::kInvalidArgument);

  Limits huge_window;
  huge_window.default_freshness_window = 31LL * 24 * 60 * 60 * 1000 * 1000 * 1000;
  TOBSV_ASSERT_FAILS_WITH(huge_window.validate(), ErrorCode::kOutOfRange);

  Limits huge_gap;
  huge_gap.default_episode_gap = 31LL * 24 * 60 * 60 * 1000 * 1000 * 1000;
  TOBSV_ASSERT_FAILS_WITH(huge_gap.validate(), ErrorCode::kOutOfRange);
}

TOBSV_TEST("core", "check_capacity_and_validate_text_are_inclusive_at_the_bound") {
  TOBSV_ASSERT_OK(check_capacity(4, 5, "records"));
  TOBSV_ASSERT_FAILS_WITH(check_capacity(5, 5, "records"), ErrorCode::kLimitExceeded);
  TOBSV_ASSERT_FAILS_WITH(check_capacity(6, 5, "records"), ErrorCode::kLimitExceeded);
  TOBSV_ASSERT_FAILS_WITH(check_capacity(0, 0, "records"), ErrorCode::kLimitExceeded);

  TOBSV_ASSERT_OK(validate_text("a label", "entity label", 32));
  TOBSV_ASSERT_OK(validate_text(std::string(32, 'x'), "entity label", 32));
  TOBSV_ASSERT_FAILS_WITH(validate_text(std::string(33, 'x'), "entity label", 32),
                          ErrorCode::kLimitExceeded);
  TOBSV_ASSERT_FAILS_WITH(validate_text("", "entity label", 32), ErrorCode::kInvalidArgument);
  // Control characters would break a one line record apart, so they are refused whatever the length.
  TOBSV_ASSERT_FAILS_WITH(validate_text(std::string("line\nfeed"), "entity label", 32),
                          ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(validate_text(std::string("carriage\rreturn"), "entity label", 32),
                          ErrorCode::kInvalidArgument);
  TOBSV_ASSERT_FAILS_WITH(validate_text(std::string("delete\x7f"), "entity label", 32),
                          ErrorCode::kInvalidArgument);
}

TOBSV_TEST("core", "json_parse_refuses_every_repair_it_could_have_made") {
  const Limits limits;
  const auto rejected = [&limits](const std::string& document) {
    return !JsonValue::parse(document, limits).ok();
  };
  TOBSV_ASSERT_TRUE(rejected(""));
  TOBSV_ASSERT_TRUE(rejected("[1,]"));
  TOBSV_ASSERT_TRUE(rejected("{\"a\":1,}"));
  TOBSV_ASSERT_TRUE(rejected("{\"a\":1,\"a\":2}"));
  TOBSV_ASSERT_TRUE(rejected("{\"a\":}"));
  TOBSV_ASSERT_TRUE(rejected("01"));
  TOBSV_ASSERT_TRUE(rejected("-"));
  TOBSV_ASSERT_TRUE(rejected("NaN"));
  TOBSV_ASSERT_TRUE(rejected("Infinity"));
  TOBSV_ASSERT_TRUE(rejected("{\"a\":1} trailing"));
  TOBSV_ASSERT_TRUE(rejected("\"unterminated"));
  TOBSV_ASSERT_TRUE(rejected("1e999"));
  TOBSV_ASSERT_TRUE(rejected("\"\\ud800\""));
  TOBSV_ASSERT_TRUE(rejected("\"\\udc00\""));
  TOBSV_ASSERT_TRUE(rejected("\"\\ud800\\u0041\""));
  TOBSV_ASSERT_TRUE(rejected("\"\\q\""));
  TOBSV_ASSERT_TRUE(rejected(std::string("\"a") + '\x01' + "b\""));
  TOBSV_ASSERT_TRUE(rejected(std::string("\"") + '\x80' + "\""));
  TOBSV_ASSERT_TRUE(rejected(std::string(40, '[') + std::string(40, ']')));

  // The byte budget is checked before parsing, so an oversized document never reaches the scanner.
  Limits tiny;
  tiny.max_json_bytes = 8;
  const Result<JsonValue> oversized = JsonValue::parse("[\"a very long document\"]", tiny);
  TOBSV_ASSERT_FALSE(oversized.ok());
  TOBSV_ASSERT_TRUE(oversized.error().code() == ErrorCode::kLimitExceeded);

  // The same shapes are accepted when they are well formed, so the rejections above are about the
  // defects and not about the parser refusing everything.
  const Result<JsonValue> accepted = JsonValue::parse(
      "{\"emoji\":\"\\ud83d\\ude00\",\"slash\":\"\\/\",\"zero\":0,\"n\":-0.5}", limits);
  TOBSV_ASSERT_TRUE(accepted.ok());
  TOBSV_ASSERT_EQ(accepted.value().fields().size(), static_cast<std::size_t>(4));
  const Result<JsonValue> truncated_number = JsonValue::parse("9", limits);
  TOBSV_ASSERT_TRUE(truncated_number.ok());
  TOBSV_ASSERT_TRUE(truncated_number.value().is_number());
  TOBSV_ASSERT_TRUE(truncated_number.value().number().is_integer);
}

TOBSV_TEST("core", "json_dump_is_canonical_and_non_finite_reals_are_reported") {
  JsonValue object = JsonValue::object();
  object.set("zeta", JsonValue::integer(1));
  object.set("alpha", JsonValue::integer(2));
  object.set("middle", JsonValue::real(0.5));
  const Result<std::string> compact = object.dump_compact();
  TOBSV_ASSERT_TRUE(compact.ok());
  TOBSV_ASSERT_EQ(compact.value(), std::string("{\"alpha\":2,\"middle\":0.5,\"zeta\":1}"));

  // Insertion order must not survive into the encoding, or two runs would disagree byte for byte.
  JsonValue reordered = JsonValue::object();
  reordered.set("middle", JsonValue::real(0.5));
  reordered.set("zeta", JsonValue::integer(1));
  reordered.set("alpha", JsonValue::integer(2));
  const Result<std::string> reordered_text = reordered.dump_compact();
  TOBSV_ASSERT_TRUE(reordered_text.ok());
  TOBSV_ASSERT_EQ(reordered_text.value(), compact.value());

  JsonValue nested = JsonValue::array();
  nested.push(JsonValue::text("a\"b"));
  nested.push(JsonValue::boolean(true));
  nested.push(JsonValue::null());
  JsonValue outer = JsonValue::object();
  outer.set("list", nested);
  outer.set("count", JsonValue::integer(-3));
  const Result<std::string> text = outer.dump_compact();
  TOBSV_ASSERT_TRUE(text.ok());
  TOBSV_ASSERT_EQ(text.value(), std::string("{\"count\":-3,\"list\":[\"a\\\"b\",true,null]}"));

  // Reading the canonical form back and encoding it again is a fixed point.
  const Result<JsonValue> reread = JsonValue::parse(text.value(), Limits{});
  TOBSV_ASSERT_TRUE(reread.ok());
  const Result<std::string> round_trip = reread.value().dump_compact();
  TOBSV_ASSERT_TRUE(round_trip.ok());
  TOBSV_ASSERT_EQ(round_trip.value(), text.value());
  const Result<std::string> indented = reread.value().dump_indented();
  TOBSV_ASSERT_TRUE(indented.ok());
  TOBSV_ASSERT_TRUE(indented.value().size() > text.value().size());
  TOBSV_ASSERT_TRUE(indented.value().find('\n') != std::string::npos);

  // A non-finite real has no JSON spelling, so encoding reports it rather than writing a token no
  // reader of the format accepts.
  const Result<std::string> non_finite =
      JsonValue::real(std::numeric_limits<double>::quiet_NaN()).dump_compact();
  TOBSV_ASSERT_FALSE(non_finite.ok());
  TOBSV_ASSERT_TRUE(non_finite.error().code() == ErrorCode::kIndeterminate);
}

TOBSV_TEST("core", "utf8_validation_matches_what_the_parser_accepts") {
  TOBSV_ASSERT_TRUE(is_valid_utf8(""));
  TOBSV_ASSERT_TRUE(is_valid_utf8("plain ascii"));
  TOBSV_ASSERT_TRUE(is_valid_utf8("\xc3\xa9"));
  TOBSV_ASSERT_TRUE(is_valid_utf8("\xe2\x82\xac"));
  TOBSV_ASSERT_TRUE(is_valid_utf8("\xf0\x9f\x98\x80"));
  TOBSV_ASSERT_FALSE(is_valid_utf8("\x80"));
  TOBSV_ASSERT_FALSE(is_valid_utf8("\xc3"));
  TOBSV_ASSERT_FALSE(is_valid_utf8("\xc0\x80"));
  TOBSV_ASSERT_FALSE(is_valid_utf8("\xed\xa0\x80"));
  TOBSV_ASSERT_FALSE(is_valid_utf8("\xf5\x80\x80\x80"));
  TOBSV_ASSERT_FALSE(is_valid_utf8("\xe2\x82"));

  // The parser and the predicate agree: a document carrying a surrogate half is refused rather than
  // stored in a form that no later reader could decode.
  std::string document = "{\"k\":\"aaaaaaaa\"}";
  document[7] = static_cast<char>(0xED);
  document[8] = static_cast<char>(0xA0);
  document[9] = static_cast<char>(0x80);
  TOBSV_ASSERT_FALSE(is_valid_utf8(document));
  const Result<JsonValue> parsed = JsonValue::parse(document, Limits{});
  TOBSV_ASSERT_FALSE(parsed.ok());
  TOBSV_ASSERT_TRUE(parsed.error().code() == ErrorCode::kMalformedInput);
}
