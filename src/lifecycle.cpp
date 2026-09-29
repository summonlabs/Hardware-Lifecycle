// Hardware Lifecycle - canonical lifecycle spellings, state classes and reasons.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The textual half of the lifecycle contract: exactly one canonical spelling per
// enumerator, a strict parser for every durable and command line entry point, and
// one explicitly lenient parser used only for operator convenience.
//
// Two invariants hold throughout this file:
//  - an unparsable spelling produces a MalformedRequest that names the field and
//    the offending text; a default enumerator is never substituted for input the
//    runtime did not understand;
//  - every table below is ordered by the declaration order of its enumeration, so
//    a rendering, a reason mask and a decoded document are reproducible byte for
//    byte and never depend on container iteration order.
//
// The transition table and the reachability helpers derived from it
// (transition_rules, find_rule, legal_targets, is_legal_transition,
// describe_rule, lifecycle_machine_diagram) live in src/transition_table.cpp.

#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/result.hpp"
#include "hardware_lifecycle/text.hpp"

namespace hardware_lifecycle {
namespace {

// Canonical spellings, indexed by the enumeration value. The static assert on
// every table turns a missing entry into a compile error instead of a silent gap.
constexpr std::string_view kStateSpellings[] = {
    "Ordered",     "Staged",      "Installed",     "Commissioning", "Active",   "Degraded",
    "Maintenance", "Quarantined", "Retiring",      "Retired",       "Replaced", "Removed"};
static_assert(std::size(kStateSpellings) == kLifecycleStateCount,
              "one canonical spelling per lifecycle state");

// Declaration order, no duplicates: all_lifecycle_states() is the enumeration.
constexpr LifecycleState kAllStates[] = {
    LifecycleState::Ordered,       LifecycleState::Staged,        LifecycleState::Installed,
    LifecycleState::Commissioning, LifecycleState::Active,        LifecycleState::Degraded,
    LifecycleState::Maintenance,   LifecycleState::Quarantined,   LifecycleState::Retiring,
    LifecycleState::Retired,       LifecycleState::Replaced,      LifecycleState::Removed};
static_assert(std::size(kAllStates) == kLifecycleStateCount,
              "all_lifecycle_states must enumerate every state exactly once");

constexpr StateClass kStateClasses[] = {
    StateClass::PrePhysical,        StateClass::PhysicalIdle,      StateClass::PhysicalIdle,
    StateClass::CommissioningPhase, StateClass::InService,         StateClass::InServiceImpaired,
    StateClass::ServiceWithheld,    StateClass::IntegrityWithheld, StateClass::Draining,
    StateClass::Decommissioned,     StateClass::Superseded,        StateClass::Disposed};
static_assert(std::size(kStateClasses) == kLifecycleStateCount, "one state class per lifecycle state");

constexpr std::string_view kStateClassSpellings[] = {
    "pre_physical",       "physical_idle", "commissioning", "in_service", "in_service_impaired", "service_withheld",
    "integrity_withheld", "draining",      "decommissioned", "superseded", "disposed"};
static_assert(std::size(kStateClassSpellings) == static_cast<std::size_t>(StateClass::Disposed) + 1,
              "one canonical spelling per state class");

constexpr std::string_view kReasonSpellings[] = {
    "unset",
    "object_registered",
    "order_cancelled",
    "delivery_accepted",
    "staging_returned",
    "staging_loss",
    "installation_completed",
    "installation_reversed",
    "installation_abandoned",
    "commissioning_started",
    "commissioning_passed",
    "commissioning_failed",
    "commissioning_rework_required",
    "commissioning_abandoned",
    "service_impairment_acknowledged",
    "service_restored",
    "maintenance_scheduled",
    "maintenance_completed",
    "maintenance_completed_impaired",
    "integrity_concern_raised",
    "quarantine_released_for_recommissioning",
    "quarantine_released_for_retirement",
    "quarantine_released_for_disposal",
    "retirement_approved",
    "drain_cancelled",
    "drain_converted_to_maintenance",
    "decommission_completed",
    "successor_linked",
    "physically_removed",
    "eligibility_gate_opened",
    "eligibility_gate_closed"};
static_assert(std::size(kReasonSpellings) == kTransitionReasonCount,
              "one canonical spelling per transition reason");

constexpr std::string_view kEffectSpellings[] = {"unchanged", "suspend", "resume", "terminate"};
static_assert(std::size(kEffectSpellings) == static_cast<std::size_t>(EligibilityEffect::Terminate) + 1,
              "one canonical spelling per eligibility effect");

[[nodiscard]] Error unknown_spelling(std::string_view field, std::string_view text) {
  return Error::make(ErrorCode::MalformedRequest, "unknown " + std::string(field) + " spelling")
      .with(std::string(field), std::string(text));
}

[[nodiscard]] Result<std::size_t> state_index(std::string_view text, bool lenient) {
  for (std::size_t index = 0; index < std::size(kStateSpellings); ++index) {
    const std::string_view spelling = kStateSpellings[index];
    const bool match = lenient ? ascii_iequals(spelling, text) : spelling == text;
    if (match) {
      return index;
    }
  }
  return unknown_spelling("lifecycle state", text);
}

}  // namespace

std::string_view to_string(LifecycleState state) noexcept {
  const std::size_t index = static_cast<std::size_t>(state);
  if (index >= std::size(kStateSpellings)) {
    // A value outside the enumeration is not a state; it is reported as unknown
    // rather than as whatever enumerator happens to be stored first.
    return "unknown";
  }
  return kStateSpellings[index];
}

Result<LifecycleState> parse_lifecycle_state(std::string_view text) {
  const Result<std::size_t> index = state_index(text, false);
  if (!index.has_value()) {
    return index.error();
  }
  return static_cast<LifecycleState>(index.value());
}

Result<LifecycleState> parse_lifecycle_state_lenient(std::string_view text) {
  const Result<std::size_t> index = state_index(text, true);
  if (!index.has_value()) {
    return index.error();
  }
  return static_cast<LifecycleState>(index.value());
}

std::span<const LifecycleState> all_lifecycle_states() noexcept {
  return std::span<const LifecycleState>(kAllStates, std::size(kAllStates));
}

StateClass state_class(LifecycleState state) noexcept {
  const std::size_t index = static_cast<std::size_t>(state);
  if (index >= std::size(kStateClasses)) {
    // No parse path can produce such a value. The mapping stays total so that a
    // corrupt value can never read outside the table.
    return StateClass::PrePhysical;
  }
  return kStateClasses[index];
}

std::string_view to_string(StateClass klass) noexcept {
  const std::size_t index = static_cast<std::size_t>(klass);
  if (index >= std::size(kStateClassSpellings)) {
    return "unknown";
  }
  return kStateClassSpellings[index];
}

std::string_view to_string(TransitionReason reason) noexcept {
  const std::size_t index = static_cast<std::size_t>(reason);
  if (index >= std::size(kReasonSpellings)) {
    return "unknown";
  }
  return kReasonSpellings[index];
}

Result<TransitionReason> parse_transition_reason(std::string_view text) {
  for (std::size_t index = 0; index < std::size(kReasonSpellings); ++index) {
    if (kReasonSpellings[index] == text) {
      return static_cast<TransitionReason>(index);
    }
  }
  return unknown_spelling("transition reason", text);
}

std::string to_string(ReasonMask mask) {
  if (mask.empty()) {
    return "none";
  }
  std::string text;
  // Index 0 is Unset: reason_bit() gives it no bit, so it can never be rendered.
  for (std::size_t index = 1; index < std::size(kReasonSpellings); ++index) {
    const TransitionReason reason = static_cast<TransitionReason>(index);
    if (!mask.intersects(reason_bit(reason))) {
      continue;
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += kReasonSpellings[index];
  }
  return text;
}

bool reason_allowed(ReasonMask mask, TransitionReason reason) noexcept {
  // reason_bit(Unset) is empty, so an unset reason satisfies no rule.
  return mask.intersects(reason_bit(reason));
}

std::string_view to_string(EligibilityEffect effect) noexcept {
  const std::size_t index = static_cast<std::size_t>(effect);
  if (index >= std::size(kEffectSpellings)) {
    return "unknown";
  }
  return kEffectSpellings[index];
}

}  // namespace hardware_lifecycle
