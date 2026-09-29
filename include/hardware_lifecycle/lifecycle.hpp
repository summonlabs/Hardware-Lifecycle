#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The authoritative lifecycle state machine. transition_rules() is the single
// source of truth: the runtime, the command line tools, the documentation
// helper and the tests all read the same table. There is no second copy of the
// legal-edge list anywhere in this repository.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

// ---------------------------------------------------------------------------
// States
// ---------------------------------------------------------------------------

/// The canonical lifecycle states of one hardware object.
///
/// Refinement notes, in the order of the enumeration:
///  - Ordered: a commitment exists; no physical unit exists at a facility yet.
///  - Staged: the unit is physically at a facility but is not installed in a
///    target location.
///  - Installed: the unit occupies its target location but carries no service
///    and has not been commissioned.
///  - Commissioning: verification and qualification are in progress.
///  - Active: the unit is in service.
///  - Degraded: an authority has acknowledged impaired service and the unit
///    remains in service. This state is entered only by an explicit transition
///    request with evidence; a health observation can never enter it.
///  - Maintenance: withdrawn from service on purpose, expected to return.
///  - Quarantined: withheld from service and from the maintenance rotation
///    pending an integrity decision.
///  - Retiring: drained. Service has been withdrawn, decommissioning has not.
///  - Retired: decommissioned but still physically present.
///  - Replaced: superseded by a successor object; still physically present.
///  - Removed: physically gone. Terminal, unless a new hardware identity and
///    generation are created, which is a different object key.
enum class LifecycleState : std::uint8_t {
  Ordered = 0,
  Staged,
  Installed,
  Commissioning,
  Active,
  Degraded,
  Maintenance,
  Quarantined,
  Retiring,
  Retired,
  Replaced,
  Removed,
};

/// Number of LifecycleState enumerators, so valid values are 0 .. count - 1.
inline constexpr std::size_t kLifecycleStateCount = 12;

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(LifecycleState state) noexcept;

/// Canonical, case sensitive parse. Decoding always uses this entry point.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<LifecycleState> parse_lifecycle_state(std::string_view text);

/// Case insensitive parse used only by the command line tools for operator
/// convenience. It rejects the same set of spellings as the canonical parser
/// once case is folded.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<LifecycleState> parse_lifecycle_state_lenient(std::string_view text);

[[nodiscard]] HARDWARE_LIFECYCLE_API std::span<const LifecycleState> all_lifecycle_states() noexcept;

/// Coarse grouping used for reporting. Two states in the same class are still
/// distinct states: the class never substitutes for the state.
enum class StateClass : std::uint8_t {
  PrePhysical = 0,
  PhysicalIdle,
  CommissioningPhase,
  InService,
  InServiceImpaired,
  ServiceWithheld,
  IntegrityWithheld,
  Draining,
  Decommissioned,
  Superseded,
  Disposed,
};

[[nodiscard]] HARDWARE_LIFECYCLE_API StateClass state_class(LifecycleState state) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(StateClass klass) noexcept;

/// Removed only.
[[nodiscard]] constexpr bool is_terminal(LifecycleState state) noexcept { return state == LifecycleState::Removed; }

/// False for Ordered: nothing physical exists yet.
[[nodiscard]] constexpr bool is_physical(LifecycleState state) noexcept { return state != LifecycleState::Ordered; }

/// True when the state implies that the object occupies an assigned facility
/// location. Staged does not (it is at the facility, not in place) and Removed
/// does not (it is gone).
[[nodiscard]] constexpr bool requires_location(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::Installed:
    case LifecycleState::Commissioning:
    case LifecycleState::Active:
    case LifecycleState::Degraded:
    case LifecycleState::Maintenance:
    case LifecycleState::Quarantined:
    case LifecycleState::Retiring:
    case LifecycleState::Retired:
    case LifecycleState::Replaced:
      return true;
    case LifecycleState::Ordered:
    case LifecycleState::Staged:
    case LifecycleState::Removed:
      return false;
  }
  return false;
}

/// True when the object is inside the service scope: it is in service, possibly
/// impaired. This is a statement about lifecycle only. It says nothing about
/// readiness, availability, health or whether the service eligibility gate is
/// open.
[[nodiscard]] constexpr bool occupies_service_scope(LifecycleState state) noexcept {
  return state == LifecycleState::Active || state == LifecycleState::Degraded;
}

/// True when decommissioning has completed. Draining is explicitly not
/// decommissioning, so Retiring is excluded.
[[nodiscard]] constexpr bool is_decommissioned(LifecycleState state) noexcept {
  return state == LifecycleState::Retired || state == LifecycleState::Replaced || state == LifecycleState::Removed;
}

/// True when the object has been superseded by a successor object.
[[nodiscard]] constexpr bool is_superseded(LifecycleState state) noexcept { return state == LifecycleState::Replaced; }

// ---------------------------------------------------------------------------
// Reasons
// ---------------------------------------------------------------------------

/// Why a transition happened. Every durable history entry carries one, and each
/// rule declares the exact set of reasons it accepts.
enum class TransitionReason : std::uint8_t {
  Unset = 0,
  ObjectRegistered,
  OrderCancelled,
  DeliveryAccepted,
  StagingReturned,
  StagingLoss,
  InstallationCompleted,
  InstallationReversed,
  InstallationAbandoned,
  CommissioningStarted,
  CommissioningPassed,
  CommissioningFailed,
  CommissioningReworkRequired,
  CommissioningAbandoned,
  ServiceImpairmentAcknowledged,
  ServiceRestored,
  MaintenanceScheduled,
  MaintenanceCompleted,
  MaintenanceCompletedImpaired,
  IntegrityConcernRaised,
  QuarantineReleasedForRecommissioning,
  QuarantineReleasedForRetirement,
  QuarantineReleasedForDisposal,
  RetirementApproved,
  DrainCancelled,
  DrainConvertedToMaintenance,
  DecommissionCompleted,
  SuccessorLinked,
  PhysicallyRemoved,
  /// A service eligibility gate decision. These two are not edges of the
  /// lifecycle machine and no transition rule may allow them: they exist so that
  /// a gate change is recorded in the object's history chain like every other
  /// mutation, with from equal to to and the lifecycle generation unchanged.
  EligibilityGateOpened,
  EligibilityGateClosed,
};

/// Number of TransitionReason enumerators, so valid values are 0 .. count - 1.
inline constexpr std::size_t kTransitionReasonCount = 31;

struct ReasonMaskTag;

using ReasonMask = Mask<ReasonMaskTag>;

/// Empty for Unset: an unset reason satisfies no rule.
[[nodiscard]] constexpr ReasonMask reason_bit(TransitionReason reason) noexcept {
  if (reason == TransitionReason::Unset) {
    return ReasonMask();
  }
  return ReasonMask(static_cast<ReasonMask::value_type>(1u) << (static_cast<unsigned>(reason) - 1u));
}

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(TransitionReason reason) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<TransitionReason> parse_transition_reason(std::string_view text);
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string to_string(ReasonMask mask);
[[nodiscard]] HARDWARE_LIFECYCLE_API bool reason_allowed(ReasonMask mask, TransitionReason reason) noexcept;

// ---------------------------------------------------------------------------
// Transition table
// ---------------------------------------------------------------------------

/// What a transition does to the service eligibility gate. Note that the gate
/// is a separate piece of authority: no transition can quietly set it to Open,
/// and no transition opens it implicitly.
enum class EligibilityEffect : std::uint8_t {
  Unchanged = 0,
  Suspend,
  Resume,
  Terminate,
};

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(EligibilityEffect effect) noexcept;

/// One legal edge of the lifecycle machine.
struct TransitionRule {
  LifecycleState from = LifecycleState::Ordered;
  LifecycleState to = LifecycleState::Ordered;
  /// Any one of these scopes is enough.
  AuthorityMask required_authority;
  /// Every one of these evidence kinds must be present in the request. An empty
  /// mask requires at least one evidence reference of any kind: a transition
  /// never happens without evidence, even when no particular kind is demanded.
  EvidenceMask required_evidence;
  /// The reason must be one of these.
  ReasonMask allowed_reasons;
  EligibilityEffect eligibility_effect = EligibilityEffect::Unchanged;
  /// The request must carry a successor link and the successor must exist.
  bool requires_successor_link = false;
  /// The successor object must already be physically installed or beyond.
  bool requires_successor_installed = false;
  /// The post state requires the object to have a location.
  bool requires_location = false;
  std::string_view note;
};

/// The complete transition table, in a deterministic declaration order.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::span<const TransitionRule> transition_rules() noexcept;

/// Null when the edge is not in the table.
[[nodiscard]] HARDWARE_LIFECYCLE_API const TransitionRule* find_rule(LifecycleState from, LifecycleState to) noexcept;

/// Targets reachable in one legal step from the given state, in table order.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::vector<LifecycleState> legal_targets(LifecycleState from);

[[nodiscard]] HARDWARE_LIFECYCLE_API bool is_legal_transition(LifecycleState from, LifecycleState to) noexcept;

/// Human readable rendering of one rule, derived from the table.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string describe_rule(const TransitionRule& rule);

/// Text rendering of the whole machine, derived from the table.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string lifecycle_machine_diagram();

}  // namespace hardware_lifecycle