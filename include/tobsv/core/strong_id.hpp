// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "tobsv/core/hash.hpp"
#include "tobsv/core/result.hpp"

namespace tobsv {

// Longest accepted identity token. The bound exists so that identities cannot be used as an
// unbounded payload channel.
inline constexpr std::size_t kMaxIdentifierLength = 96;

// Identity grammar: one to kMaxIdentifierLength printable ASCII characters. The first character must
// be alphanumeric. The remaining characters may be alphanumeric or one of "._:@/+#-". Whitespace,
// control characters, quotes and backslashes are rejected so that an identity can never break the
// framing of a textual or JSON record.
Status validate_identity_token(std::string_view text, std::string_view kind);

// Tag types. Each tag is a distinct C++ type, so an EntityId can never be passed where a SensorId is
// required even though both are strings underneath.
struct EntityIdTag {
  static constexpr std::string_view kKind = "entity";
  static constexpr std::string_view kPrefix = "ent";
};
struct SensorIdTag {
  static constexpr std::string_view kKind = "sensor";
  static constexpr std::string_view kPrefix = "sen";
};
struct SiteIdTag {
  static constexpr std::string_view kKind = "site";
  static constexpr std::string_view kPrefix = "site";
};
struct ZoneRefTag {
  static constexpr std::string_view kKind = "thermal zone reference";
  static constexpr std::string_view kPrefix = "zone";
};
struct TopologyRefTag {
  static constexpr std::string_view kKind = "topology reference";
  static constexpr std::string_view kPrefix = "topo";
};
struct SourceIdTag {
  static constexpr std::string_view kKind = "source";
  static constexpr std::string_view kPrefix = "src";
};
struct EnvelopeIdTag {
  static constexpr std::string_view kKind = "envelope";
  static constexpr std::string_view kPrefix = "env";
};
struct ObservationIdTag {
  static constexpr std::string_view kKind = "observation";
  static constexpr std::string_view kPrefix = "obs";
};
struct CouplingIdTag {
  static constexpr std::string_view kKind = "coupling";
  static constexpr std::string_view kPrefix = "cpl";
};
struct EpisodeIdTag {
  static constexpr std::string_view kKind = "hotspot episode";
  static constexpr std::string_view kPrefix = "hot";
};
struct DeratingIdTag {
  static constexpr std::string_view kKind = "derating";
  static constexpr std::string_view kPrefix = "dr";
};
struct DeratingSiteTag {
  static constexpr std::string_view kKind = "derating site";
  static constexpr std::string_view kPrefix = "drs";
};
struct RuleIdTag {
  static constexpr std::string_view kKind = "rule";
  static constexpr std::string_view kPrefix = "rule";
};

template <class Tag>
class StrongId {
 public:
  StrongId() = default;

  static Result<StrongId> parse(std::string_view text) {
    const Status status = validate_identity_token(text, Tag::kKind);
    if (!status.ok()) {
      return status;
    }
    return StrongId(std::string(text));
  }

  // Constructs an identity from a value the caller already validated. Used by fixtures, by
  // deterministic derivation and by deserialisers that validate separately.
  static StrongId unchecked(std::string_view text) { return StrongId(std::string(text)); }

  // Deterministic derived identity: a stable prefix plus the digest in lower-case hex. Used for
  // observations, coupling relations, episodes and derating appraisals, all of which are addressed
  // by content.
  static StrongId from_digest(std::uint64_t digest) {
    return StrongId(std::string(Tag::kPrefix) + "-" + hex_u64(digest));
  }

  bool is_set() const noexcept { return !value_.empty(); }
  const std::string& value() const noexcept { return value_; }
  std::string_view view() const noexcept { return value_; }

  constexpr std::string_view kind() const noexcept { return Tag::kKind; }

  std::string to_string() const { return value_; }

  friend bool operator==(const StrongId& a, const StrongId& b) { return a.value_ == b.value_; }
  friend bool operator!=(const StrongId& a, const StrongId& b) { return !(a == b); }
  friend bool operator<(const StrongId& a, const StrongId& b) { return a.value_ < b.value_; }
  friend bool operator>(const StrongId& a, const StrongId& b) { return b < a; }
  friend bool operator<=(const StrongId& a, const StrongId& b) { return !(b < a); }
  friend bool operator>=(const StrongId& a, const StrongId& b) { return !(a < b); }

  friend StableDigest& operator<<(StableDigest& digest, const StrongId& id) {
    digest.absorb_text(id.value_);
    return digest;
  }

 private:
  explicit StrongId(std::string value) : value_(std::move(value)) {}
  std::string value_;
};

using EntityId = StrongId<EntityIdTag>;
using SensorId = StrongId<SensorIdTag>;
using SiteId = StrongId<SiteIdTag>;
using ZoneRef = StrongId<ZoneRefTag>;
using TopologyRef = StrongId<TopologyRefTag>;
using SourceId = StrongId<SourceIdTag>;
using EnvelopeId = StrongId<EnvelopeIdTag>;
using ObservationId = StrongId<ObservationIdTag>;
using CouplingId = StrongId<CouplingIdTag>;
using EpisodeId = StrongId<EpisodeIdTag>;
using DeratingId = StrongId<DeratingIdTag>;
using DeratingSiteId = StrongId<DeratingSiteTag>;
using RuleId = StrongId<RuleIdTag>;

}  // namespace tobsv
