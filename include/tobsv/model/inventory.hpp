// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/core/strong_id.hpp"
#include "tobsv/model/vocabulary.hpp"

namespace tobsv {

// What this runtime knows about an entity it observes. The zone reference is exactly that: a
// reference to a thermal zone owned by an adjacent authority. This runtime never creates, bounds,
// governs or actuates a zone; it records which one an entity is said to belong to so that a report
// can name it, and it refuses any claim that would require authority over it.
struct EntityRecord {
  EntityId entity;
  EntityClass entity_class = EntityClass::kUnknown;
  ZoneRef zone;
  SiteId site;
  std::string label;

  Status validate(const Limits& limits) const;
};

class ThermalInventory {
 public:
  Status add(const EntityRecord& record, const Limits& limits);
  const EntityRecord* find(const EntityId& entity) const;
  // kUnknown for an entity that was never registered. Callers report that as an attribution limit
  // instead of guessing a class from the name.
  EntityClass classify(const EntityId& entity) const;
  ZoneRef zone_of(const EntityId& entity) const;
  std::vector<EntityId> entities() const;
  std::size_t size() const noexcept { return records_.size(); }

 private:
  std::map<EntityId, EntityRecord> records_;
};

}  // namespace tobsv
