// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "tobsv/model/inventory.hpp"

namespace tobsv {

Status EntityRecord::validate(const Limits& limits) const {
  if (!entity.is_set()) {
    return Status::failure(ErrorCode::kInvalidArgument, "inventory record has no entity identity");
  }
  if (entity_class == EntityClass::kUnknown) {
    return Status::failure(ErrorCode::kUnsupported,
                           "entity " + entity.value() +
                               " must declare a class; a class cannot be inferred from its name");
  }
  const Status label_status = validate_text(label, "entity label", limits.max_text_length);
  if (!label_status.ok()) {
    return label_status;
  }
  return Status::success();
}

Status ThermalInventory::add(const EntityRecord& record, const Limits& limits) {
  const Status valid = record.validate(limits);
  if (!valid.ok()) {
    return valid;
  }
  if (records_.find(record.entity) != records_.end()) {
    return Status::failure(ErrorCode::kDuplicateIdentity,
                           "entity " + record.entity.value() + " is already inventoried");
  }
  const Status capacity = check_capacity(records_.size(), limits.max_entities, "inventoried entities");
  if (!capacity.ok()) {
    return capacity;
  }
  records_.emplace(record.entity, record);
  return Status::success();
}

const EntityRecord* ThermalInventory::find(const EntityId& entity) const {
  const auto it = records_.find(entity);
  return it == records_.end() ? nullptr : &it->second;
}

EntityClass ThermalInventory::classify(const EntityId& entity) const {
  const EntityRecord* record = find(entity);
  return record == nullptr ? EntityClass::kUnknown : record->entity_class;
}

ZoneRef ThermalInventory::zone_of(const EntityId& entity) const {
  const EntityRecord* record = find(entity);
  return record == nullptr ? ZoneRef{} : record->zone;
}

std::vector<EntityId> ThermalInventory::entities() const {
  std::vector<EntityId> out;
  out.reserve(records_.size());
  for (const auto& entry : records_) {
    out.push_back(entry.first);
  }
  return out;
}

}  // namespace tobsv
