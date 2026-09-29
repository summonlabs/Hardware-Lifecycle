#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Requests, receipts and the one validator that both applies and explains.
// Every externally meaningful mutation binds to the exact identity, hardware
// generation, lifecycle generation, revision, control epoch and evidence the
// caller planned against.
//
// Validation precedence is deterministic. The validator collects every
// violation it finds and reports the one with the lowest validation_rank();
// therefore the primary error of a request does not depend on which check ran
// first, on map ordering or on thread scheduling.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/health.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/model.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

class Registry;

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

/// Registration of one hardware object. The initial state is restricted to
/// Ordered or Staged: an object cannot be born installed, commissioned or
/// active, because those states assert facts that only the transition table may
/// establish. Discovery is not capability.
struct CreateRequest {
  PlanId plan;
  AttemptId attempt;
  ObjectKey key;
  HardwareKind kind = HardwareKind::Unknown;
  ModelId model;
  FirmwareGeneration firmware_generation;
  LifecycleState initial_state = LifecycleState::Ordered;
  std::optional<Location> location;
  Provenance provenance;
};

/// A request to move one object along one legal edge of the transition table.
struct TransitionRequest {
  PlanId plan;
  AttemptId attempt;
  ObjectKey key;
  LifecycleGeneration expected_lifecycle_generation;
  Revision expected_revision;
  ControlEpoch expected_control_epoch;
  LifecycleState expected_state = LifecycleState::Ordered;
  LifecycleState target_state = LifecycleState::Ordered;
  TransitionReason reason = TransitionReason::Unset;
  Provenance provenance;
  /// Required by rules that demand a successor link.
  std::optional<ObjectKey> successor;
  /// Required when the target state demands a location the object does not have.
  std::optional<Location> location;
};

struct ReplacementRequest {
  PlanId plan;
  AttemptId attempt;
  ObjectKey predecessor;
  ObjectKey successor;
  LifecycleGeneration expected_lifecycle_generation;
  Revision expected_revision;
  ControlEpoch expected_control_epoch;
  Provenance provenance;
};

struct EligibilityGateRequest {
  PlanId plan;
  AttemptId attempt;
  ObjectKey key;
  EligibilityGate gate = EligibilityGate::Unknown;
  LifecycleGeneration expected_lifecycle_generation;
  Revision expected_revision;
  ControlEpoch expected_control_epoch;
  Provenance provenance;
};

struct HealthObservationRequest {
  PlanId plan;
  AttemptId attempt;
  ObjectKey key;
  HealthObservation observation;
  Provenance provenance;
};

/// Re-establish live authority over an object recovered by a restart.
struct AttestationRequest {
  PlanId plan;
  AttemptId attempt;
  ObjectKey key;
  Revision expected_revision;
  ControlEpoch expected_control_epoch;
  Provenance provenance;
};

// ---------------------------------------------------------------------------
// Receipts
// ---------------------------------------------------------------------------

/// Receipts are issued only after the durable commit point has been passed and
/// the in-memory state has been advanced. A receipt that exists describes an
/// effect that happened; acknowledgement alone never produces one.
struct CreateReceipt {
  PlanId plan;
  AttemptId attempt;
  ObjectKey key;
  LifecycleState state = LifecycleState::Ordered;
  LifecycleGeneration lifecycle_generation;
  Revision revision;
  CommitSequence commit_sequence;
  LogicalTime logical_time;
  Digest receipt_digest;
  bool idempotent_replay = false;
};

struct TransitionReceipt {
  PlanId plan;
  AttemptId attempt;
  ObjectKey key;
  LifecycleState from = LifecycleState::Ordered;
  LifecycleState to = LifecycleState::Ordered;
  TransitionReason reason = TransitionReason::Unset;
  Revision revision_before;
  Revision revision_after;
  LifecycleGeneration lifecycle_generation;
  EligibilityGate gate_after = EligibilityGate::Unknown;
  CommitSequence commit_sequence;
  LogicalTime logical_time;
  Digest request_digest;
  Digest entry_digest;
  Digest receipt_digest;
  bool idempotent_replay = false;
};

struct ReplacementReceipt {
  PlanId plan;
  AttemptId attempt;
  ObjectKey predecessor;
  ObjectKey successor;
  ReplacementGeneration replacement_generation;
  CommitSequence commit_sequence;
  LogicalTime logical_time;
  Digest link_digest;
  Digest request_digest;
  Digest receipt_digest;
  bool idempotent_replay = false;
};

struct EligibilityGateReceipt {
  PlanId plan;
  AttemptId attempt;
  ObjectKey key;
  EligibilityGate before = EligibilityGate::Unknown;
  EligibilityGate after = EligibilityGate::Unknown;
  Revision revision;
  CommitSequence commit_sequence;
  LogicalTime logical_time;
  Digest request_digest;
  Digest receipt_digest;
  bool idempotent_replay = false;
};

struct HealthReceipt {
  PlanId plan;
  AttemptId attempt;
  ObjectKey key;
  ObservationSequence sequence;
  CommitSequence commit_sequence;
  LogicalTime logical_time;
  Digest request_digest;
  Digest receipt_digest;
  bool idempotent_replay = false;
};

struct AttestationReceipt {
  PlanId plan;
  AttemptId attempt;
  ObjectKey key;
  Revision revision;
  CommitSequence commit_sequence;
  LogicalTime logical_time;
  Digest request_digest;
  Digest receipt_digest;
  bool idempotent_replay = false;
};

/// Every receipt shape, used by the applied plan ledger so that a replayed
/// request returns the receipt that was issued the first time, byte for byte.
using Receipt = std::variant<CreateReceipt, TransitionReceipt, ReplacementReceipt, EligibilityGateReceipt, HealthReceipt,
                             AttestationReceipt>;

// ---------------------------------------------------------------------------
// Request digests
// ---------------------------------------------------------------------------

/// Digest of the semantic content of a request. The authoritative logical time
/// and the advisory wall clock are excluded: the runtime assigns the first, and
/// the second is not evidence. Everything else, including the plan and attempt
/// identities and every expected generation, is covered.
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_request_digest(const CreateRequest& request);
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_request_digest(const TransitionRequest& request);
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_request_digest(const ReplacementRequest& request);
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_request_digest(const EligibilityGateRequest& request);
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_request_digest(const HealthObservationRequest& request);
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_request_digest(const AttestationRequest& request);

/// Digest of an issued receipt. Excludes the digest field itself, the
/// idempotent_replay marker and the entry_digest cross reference, because the
/// last one is derived from the chain position the receipt digest takes part
/// in. The result is that a replayed receipt is byte identical to the original.
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_receipt_digest(const CreateReceipt& receipt);
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_receipt_digest(const TransitionReceipt& receipt);
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_receipt_digest(const ReplacementReceipt& receipt);
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_receipt_digest(const EligibilityGateReceipt& receipt);
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_receipt_digest(const HealthReceipt& receipt);
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_receipt_digest(const AttestationReceipt& receipt);
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_receipt_digest(const Receipt& receipt);

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

/// What a request would do, resolved against the current authoritative state.
struct ValidatedTransition {
  const TransitionRule* rule = nullptr;
  ObjectKey key;
  LifecycleState from = LifecycleState::Ordered;
  LifecycleState to = LifecycleState::Ordered;
  TransitionReason reason = TransitionReason::Unset;
  Revision revision_before;
  Revision revision_after;
  LifecycleGeneration lifecycle_generation_before;
  LifecycleGeneration lifecycle_generation_after;
  EligibilityGate gate_before = EligibilityGate::Unknown;
  EligibilityGate gate_after = EligibilityGate::Unknown;
  std::optional<Location> location_after;
  std::optional<ObjectKey> successor;
};

struct ValidatedCreate {
  ObjectKey key;
  LifecycleState initial_state = LifecycleState::Ordered;
  Revision revision;
  LifecycleGeneration lifecycle_generation;
  std::optional<Location> location;
};

struct ValidatedLink {
  ObjectKey predecessor;
  ObjectKey successor;
  ReplacementGeneration replacement_generation;
  TransitionReason reason = TransitionReason::SuccessorLinked;
};

struct ValidatedGate {
  ObjectKey key;
  EligibilityGate before = EligibilityGate::Unknown;
  EligibilityGate after = EligibilityGate::Unknown;
  Revision revision_before;
  Revision revision_after;
};

struct ValidatedAttestation {
  ObjectKey key;
  Revision revision;
  AuthorityState before = AuthorityState::Recovered;
};

/// The outcome of validating a request without mutating anything.
enum class PreflightOutcome : std::uint8_t {
  /// The request would be applied.
  Legal = 0,
  /// The request is rejected; primary_error() carries the deterministic primary
  /// error.
  Rejected,
  /// The plan and attempt were already applied with the same request digest, so
  /// the original receipt is returned instead of a stale-plan rejection. This is
  /// checked before revision and generation staleness on purpose: a lost
  /// response must be replayable.
  AlreadyApplied,
};

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(PreflightOutcome outcome) noexcept;

struct PreflightReport {
  PreflightOutcome outcome = PreflightOutcome::Rejected;
  Error primary_error;
  ValidatedTransition transition;
  const struct AppliedPlan* applied = nullptr;
};

/// The one validator. preflight() and apply_transition() call exactly this
/// function, so an explanation and a rejection can never disagree.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<ValidatedCreate> validate_create(const Registry& registry,
                                                                             const CreateRequest& request);
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<ValidatedTransition> validate_transition(const Registry& registry,
                                                                                     const TransitionRequest& request);
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<ValidatedLink> validate_replacement(const Registry& registry,
                                                                                const ReplacementRequest& request);
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<ValidatedGate> validate_gate(const Registry& registry,
                                                                         const EligibilityGateRequest& request);
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<ValidatedAttestation> validate_attestation(const Registry& registry,
                                                                                       const AttestationRequest& request);

/// Validation of a health observation. It can only ever fail; it can never
/// change a lifecycle state.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> validate_observation(const Registry& registry,
                                                                       const HealthObservationRequest& request);

/// Structure-only validation of a transition request, used before an asset is
/// known to exist.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> validate_transition_shape(const TransitionRequest& request);

}  // namespace hardware_lifecycle
