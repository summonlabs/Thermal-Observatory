// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

// Umbrella header for Thermal Observatory.
//
// Thermal Observatory is the thermal observation, explanation and attribution layer of the Summon
// Software Labs data centre control plane. It answers one question: given the thermal evidence it
// holds, where is heat accumulating, how much headroom remains, which hotspots and propagation
// paths that evidence supports, what is derated, and where the attribution is uncertain.
//
// It observes. It does not actuate cooling, govern thermal policy, own thermal zones, place
// workloads or perform recovery; those belong to adjacent authorities, and every public type that
// refers to one of them refers to it as a reference rather than as something this runtime controls.

#include "tobsv/analysis/analysis.hpp"
#include "tobsv/attribution/attribution.hpp"
#include "tobsv/core/checked.hpp"
#include "tobsv/core/crc32c.hpp"
#include "tobsv/core/format.hpp"
#include "tobsv/core/hash.hpp"
#include "tobsv/core/json.hpp"
#include "tobsv/core/limits.hpp"
#include "tobsv/core/result.hpp"
#include "tobsv/core/strong_id.hpp"
#include "tobsv/core/time.hpp"
#include "tobsv/coupling/coupling.hpp"
#include "tobsv/derating/derating.hpp"
#include "tobsv/envelope/envelope.hpp"
#include "tobsv/envelope/headroom.hpp"
#include "tobsv/evidence/evidence.hpp"
#include "tobsv/evidence/provenance.hpp"
#include "tobsv/evidence/quality.hpp"
#include "tobsv/evidence/store.hpp"
#include "tobsv/hotspot/hotspot.hpp"
#include "tobsv/model/generation.hpp"
#include "tobsv/model/inventory.hpp"
#include "tobsv/model/topology.hpp"
#include "tobsv/model/vocabulary.hpp"
#include "tobsv/persistence/codec.hpp"
#include "tobsv/persistence/files.hpp"
#include "tobsv/persistence/lock.hpp"
#include "tobsv/persistence/log.hpp"
#include "tobsv/persistence/worker.hpp"
#include "tobsv/runtime/observatory.hpp"
#include "tobsv/version.hpp"
