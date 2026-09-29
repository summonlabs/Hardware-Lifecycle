#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Replacement lineage. A replacement links two distinct object keys: the
// predecessor keeps its identity, its history and its physical presence, and
// gains a successor reference. Identities are never merged, renamed or reused.

#include <cstddef>
#include <map>
#include <vector>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

/// One durable replacement link.
struct ReplacementRecord {
  ObjectKey predecessor;
  ObjectKey successor;
  TransitionReason reason = TransitionReason::SuccessorLinked;
  Provenance provenance;
  LogicalTime linked_at;
  CommitSequence commit_sequence;
  ReplacementGeneration replacement_generation;
  Digest link_digest;
};

/// Digest binding the two keys, the reason and the authorising provenance.
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_link_digest(const ReplacementRecord& record);

/// Rejects self replacement, invalid keys and identical keys.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> validate_replacement_pair(const ObjectKey& predecessor,
                                                                            const ObjectKey& successor);

/// The replacement forest. Every object has at most one successor and at most
/// one predecessor, so the structure is a set of chains. Cycles are rejected on
/// insertion and chain walks are depth bounded.
class HARDWARE_LIFECYCLE_API LineageGraph {
 public:
  LineageGraph() = default;

  /// Rejects a duplicate predecessor link, a predecessor that is already a
  /// successor, a successor that is already a predecessor, and any link that
  /// would close a cycle.
  [[nodiscard]] Result<void> add(const ReplacementRecord& record);

  [[nodiscard]] const ReplacementRecord* successor_of(const ObjectKey& key) const;
  [[nodiscard]] const ReplacementRecord* predecessor_of(const ObjectKey& key) const;

  /// Forward chain starting at root, in link order. Empty when root has no
  /// successor.
  [[nodiscard]] Result<std::vector<ObjectKey>> chain_from(const ObjectKey& root) const;

  /// Backward walk to the earliest known ancestor of key, depth bounded.
  [[nodiscard]] Result<std::vector<ObjectKey>> ancestry_of(const ObjectKey& key) const;

  [[nodiscard]] std::size_t size() const noexcept { return by_predecessor_.size(); }
  [[nodiscard]] bool empty() const noexcept { return by_predecessor_.empty(); }

  [[nodiscard]] const std::map<ObjectKey, ReplacementRecord>& records() const noexcept { return by_predecessor_; }

 private:
  std::map<ObjectKey, ReplacementRecord> by_predecessor_;
  std::map<ObjectKey, ObjectKey> predecessor_of_;
};

}  // namespace hardware_lifecycle
