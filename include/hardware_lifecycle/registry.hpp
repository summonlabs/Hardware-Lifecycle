#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The authoritative in-memory state: objects, per object history chains, health
// observations, the replacement forest and the applied plan ledger. Ordering is
// always by key, never by hash or insertion order, so every derived value is
// reproducible.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/health.hpp"
#include "hardware_lifecycle/history.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/model.hpp"
#include "hardware_lifecycle/persistence.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/replacement.hpp"
#include "hardware_lifecycle/requests.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

/// Identity of one plan attempt. Two attempts of the same plan are distinct
/// facts, so both fields key the ledger.
struct PlanKey {
  PlanId plan;
  AttemptId attempt;

  friend bool operator==(const PlanKey& left, const PlanKey& right) noexcept {
    return left.plan == right.plan && left.attempt == right.attempt;
  }
  friend bool operator<(const PlanKey& left, const PlanKey& right) noexcept {
    if (left.plan != right.plan) {
      return left.plan < right.plan;
    }
    return left.attempt < right.attempt;
  }
};

/// One applied plan, retained so that a lost response can be replayed instead of
/// being rejected as stale.
struct AppliedPlan {
  PlanKey key;
  RecordKind kind = RecordKind::ObjectRegistered;
  Digest request_digest;
  Receipt receipt;
  CommitSequence commit_sequence;
  LogicalTime logical_time;
};

struct RegistryLimits {
  std::size_t max_objects = limits::kMaxObjects;
  std::size_t max_history_entries_per_object = limits::kMaxHistoryEntriesPerObject;
  std::size_t max_applied_plans = limits::kMaxAppliedPlans;
  std::size_t max_evidence_per_request = limits::kMaxEvidencePerRequest;
};

/// The authoritative state. A Registry is copied whole to give every mutation a
/// transactional rollback point: a mutation that fails to commit leaves the
/// previous generation in place, exactly and completely.
class HARDWARE_LIFECYCLE_API Registry {
 public:
  Registry();
  explicit Registry(const RegistryLimits& limits);

  // ---------------------------------------------------------------------
  // Objects
  // ---------------------------------------------------------------------
  [[nodiscard]] Result<void> insert_object(const HardwareObject& object);
  [[nodiscard]] HardwareObject* find(const ObjectKey& key) noexcept;
  [[nodiscard]] const HardwareObject* find(const ObjectKey& key) const noexcept;
  [[nodiscard]] bool contains(const ObjectKey& key) const noexcept;
  [[nodiscard]] std::size_t object_count() const noexcept { return objects_.size(); }
  [[nodiscard]] const std::map<ObjectKey, HardwareObject>& objects() const noexcept { return objects_; }

  // ---------------------------------------------------------------------
  // History
  // ---------------------------------------------------------------------
  /// Appends one entry after checking chain continuity, revision continuity and
  /// generation continuity against the previous entry.
  [[nodiscard]] Result<void> append_history(HistoryEntry entry);
  [[nodiscard]] const HistoryLog* history(const ObjectKey& key) const noexcept;
  [[nodiscard]] const std::map<ObjectKey, HistoryLog>& histories() const noexcept { return histories_; }
  [[nodiscard]] std::size_t history_entry_count() const noexcept;

  // ---------------------------------------------------------------------
  // Health observations
  // ---------------------------------------------------------------------
  [[nodiscard]] Result<void> set_health(HealthSnapshot snapshot);
  [[nodiscard]] const HealthSnapshot* health(const ObjectKey& key) const noexcept;
  [[nodiscard]] const std::map<ObjectKey, HealthSnapshot>& health_snapshots() const noexcept { return health_; }

  // ---------------------------------------------------------------------
  // Replacement lineage
  // ---------------------------------------------------------------------
  [[nodiscard]] LineageGraph& lineage() noexcept { return lineage_; }
  [[nodiscard]] const LineageGraph& lineage() const noexcept { return lineage_; }

  // ---------------------------------------------------------------------
  // Applied plans
  // ---------------------------------------------------------------------
  [[nodiscard]] Result<void> record_plan(const AppliedPlan& plan);
  [[nodiscard]] const AppliedPlan* find_plan(const PlanId& plan, const AttemptId& attempt) const noexcept;
  [[nodiscard]] const std::map<PlanKey, AppliedPlan>& plans() const noexcept { return plans_; }
  [[nodiscard]] std::size_t plan_count() const noexcept { return plans_.size(); }

  // ---------------------------------------------------------------------
  // Watermarks
  // ---------------------------------------------------------------------
  [[nodiscard]] CommitSequence commit_sequence() const noexcept { return commit_sequence_; }
  [[nodiscard]] LogicalTime logical_time() const noexcept { return logical_time_; }
  [[nodiscard]] ObservationSequence observation_sequence() const noexcept { return observation_sequence_; }
  [[nodiscard]] const RegistryLimits& limits() const noexcept { return limits_; }

  /// Session scoped writer epoch. Like per object authority it is never part of
  /// the durable state image: an epoch that survived a restart would be an epoch
  /// nobody re-established.
  [[nodiscard]] ControlEpoch control_epoch() const noexcept { return control_epoch_; }
  void set_control_epoch(ControlEpoch epoch) noexcept { control_epoch_ = epoch; }

  /// Watermarks only ever move forward. A recovery that would move one
  /// backwards is a corruption, not a reset.
  [[nodiscard]] Result<void> set_watermarks(CommitSequence commit_sequence, LogicalTime logical_time,
                                            ObservationSequence observation_sequence);
  [[nodiscard]] Result<ObservationSequence> next_observation_sequence() const;

  // ---------------------------------------------------------------------
  // Derived values
  // ---------------------------------------------------------------------

  /// Digest of the complete canonical state image at the registry's own
  /// watermarks. The image excludes exactly two derived values that are not
  /// durable facts: session scoped authority and health freshness.
  [[nodiscard]] Digest state_digest() const;

  /// Internal consistency: every history chain verifies and matches its object,
  /// every lineage endpoint exists, no object is its own ancestor, every plan
  /// receipt is well formed.
  [[nodiscard]] Result<void> verify_invariants() const;

  /// Marks every object as recovered, that is, without live authority. Called
  /// after a successful recovery: authority is never inherited across a
  /// process boundary.
  void fence_all_authority() noexcept;

 private:
  RegistryLimits limits_;
  std::map<ObjectKey, HardwareObject> objects_;
  std::map<ObjectKey, HistoryLog> histories_;
  std::map<ObjectKey, HealthSnapshot> health_;
  LineageGraph lineage_;
  std::map<PlanKey, AppliedPlan> plans_;
  CommitSequence commit_sequence_;
  LogicalTime logical_time_;
  ObservationSequence observation_sequence_;
  ControlEpoch control_epoch_;
};

}  // namespace hardware_lifecycle