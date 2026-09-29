#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The append consistent lifecycle history of one object. Every entry is bound
// to its predecessor by a chain digest, so a rewritten, reordered or removed
// entry is detectable without trusting the storage layer, and replaying the
// chain must land exactly on the authoritative state.

#include <cstdint>
#include <string>
#include <vector>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/model.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

/// One durable lifecycle fact about one object.
struct HistoryEntry {
  CommitSequence commit_sequence;
  LogicalTime logical_time;
  ObjectKey key;
  LifecycleGeneration lifecycle_generation;
  Revision revision_before;
  Revision revision_after;
  LifecycleState from = LifecycleState::Ordered;
  LifecycleState to = LifecycleState::Ordered;
  TransitionReason reason = TransitionReason::Unset;
  Provenance provenance;
  Digest receipt_digest;

  /// SHA-256 over the previous chain head and the canonical bytes of this entry
  /// with the chain field cleared.
  Digest chain_digest;

  /// True for the registration entry that opens the chain. A gate decision also
  /// carries from equal to to, so the reason is what tells them apart.
  [[nodiscard]] bool is_registration() const noexcept {
    return from == to && reason == TransitionReason::ObjectRegistered;
  }

  /// True for a service eligibility gate decision: a durable mutation of the
  /// object that left the lifecycle state where it was.
  [[nodiscard]] bool is_gate_decision() const noexcept {
    return from == to &&
           (reason == TransitionReason::EligibilityGateOpened || reason == TransitionReason::EligibilityGateClosed);
  }

  /// True when the entry moved the object along the lifecycle machine. Only
  /// these entries advance the lifecycle generation.
  [[nodiscard]] bool changes_state() const noexcept { return !(from == to); }
};

/// The whole chain for one object, plus its head.
struct HistoryLog {
  std::vector<HistoryEntry> entries;
  Digest chain_head;

  [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
};

/// Computes the chain digest of one entry given the previous chain head.
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_entry_chain_digest(const Digest& previous_chain,
                                                                      const HistoryEntry& entry);

/// Recomputes every link and returns the chain head. Fails with IntegrityFailure
/// when a link does not match; it never repairs, reorders or drops an entry.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<Digest> verify_history_chain(const HistoryLog& log);

/// Replays the chain and returns the state it proves. Enforces: the chain opens
/// with a registration entry, every entry continues from the previous entry's
/// target, revisions increase by exactly one per entry, lifecycle generations
/// increase by exactly one per entry and commit sequences are strictly
/// increasing.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<LifecycleState> replay_history(const HistoryLog& log);

/// Full consistency check between a history chain and the authoritative object
/// state: chain integrity, replay target, revision, lifecycle generation,
/// hardware generation and entry count.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> verify_history_against_object(const HistoryLog& log,
                                                                                const HardwareObject& object);

}  // namespace hardware_lifecycle
