#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Structural comparison of two exported snapshots, used by the command line
// tools and by the round trip proofs. A diff never mutates anything and never
// treats an absent object as present-but-unchanged.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/export.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

enum class ObjectChangeKind : std::uint8_t {
  Added = 0,
  Removed,
  StateChanged,
  MetadataChanged,
  LineageChanged,
  GateChanged,
};

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(ObjectChangeKind kind) noexcept;

struct ObjectChange {
  ObjectKey key;
  ObjectChangeKind kind = ObjectChangeKind::Added;
  bool present_before = false;
  bool present_after = false;
  LifecycleState state_before = LifecycleState::Ordered;
  LifecycleState state_after = LifecycleState::Ordered;
  Revision revision_before;
  Revision revision_after;
  LifecycleGeneration generation_before;
  LifecycleGeneration generation_after;
  std::size_t history_before = 0;
  std::size_t history_after = 0;
};

struct SnapshotDiff {
  bool identical = false;
  Digest digest_before;
  Digest digest_after;
  CommitSequence commit_before;
  CommitSequence commit_after;
  std::size_t added = 0;
  std::size_t removed = 0;
  std::size_t changed = 0;
  std::size_t unchanged = 0;
  std::vector<ObjectChange> changes;
  std::vector<ReplacementRecord> lineage_added;
};

/// Deterministic: changes are emitted in canonical key order.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<SnapshotDiff> diff_snapshots(const Snapshot& before,
                                                                         const Snapshot& after);

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string to_json(const SnapshotDiff& diff, bool pretty);
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string describe(const SnapshotDiff& diff);

}  // namespace hardware_lifecycle
