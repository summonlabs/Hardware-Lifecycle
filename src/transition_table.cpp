// Hardware Lifecycle - the authoritative transition table.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This file is the single source of truth for legal lifecycle edges. The
// runtime validator, the command line tools, the documentation helper and the
// tests all read this table; nothing else in the repository repeats the edge
// list.
//
// Reading the table:
//   required_authority   any one of these scopes is enough
//   required_evidence    every one of these evidence kinds must be present in
//                        the request; a zero mask means "at least one evidence
//                        reference of any kind", because every externally
//                        meaningful mutation must bind to evidence it was
//                        planned against
//   allowed_reasons      the reason must be one of these
//   eligibility_effect   what the transition does to the service gate; no rule
//                        ever opens the gate, because opening it is a separate
//                        explicit decision with its own authority. Every rule
//                        that crosses into the service scope from outside it
//                        carries Resume, and the validator refuses such a
//                        crossing while the gate is not already open
//   requires_location    the post state demands an assigned facility location
//
// The table is deliberately acyclic in the "forwards" direction with a small
// number of explicitly declared reverse edges (Installed -> Staged,
// Retiring -> Active, Retiring -> Maintenance, Degraded -> Active,
// Maintenance -> Active). Reverse edges are legal only because they are written
// down here; anything absent from this table is rejected as IllegalTransition.

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string>
#include <vector>

#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/text.hpp"

namespace hardware_lifecycle {
namespace {

[[nodiscard]] AuthorityMask authority_of(std::initializer_list<AuthorityScope> scopes) {
  AuthorityMask mask;
  for (AuthorityScope scope : scopes) {
    mask |= authority_bit(scope);
  }
  return mask;
}

[[nodiscard]] EvidenceMask evidence_of(std::initializer_list<EvidenceKind> kinds) {
  EvidenceMask mask;
  for (EvidenceKind kind : kinds) {
    mask |= evidence_bit(kind);
  }
  return mask;
}

[[nodiscard]] ReasonMask reasons_of(std::initializer_list<TransitionReason> reasons) {
  ReasonMask mask;
  for (TransitionReason reason : reasons) {
    mask |= reason_bit(reason);
  }
  return mask;
}

void add_rule(std::vector<TransitionRule>& rules, LifecycleState from, LifecycleState to, AuthorityMask authority,
              EvidenceMask evidence, ReasonMask reasons, EligibilityEffect effect, bool requires_successor_link,
              bool requires_successor_installed, std::string_view note) {
  TransitionRule rule;
  rule.from = from;
  rule.to = to;
  rule.required_authority = authority;
  rule.required_evidence = evidence;
  rule.allowed_reasons = reasons;
  rule.eligibility_effect = effect;
  rule.requires_successor_link = requires_successor_link;
  rule.requires_successor_installed = requires_successor_installed;
  rule.requires_location = requires_location(to);
  rule.note = note;
  rules.push_back(rule);
}

[[nodiscard]] std::vector<TransitionRule> build_transition_rules() {
  std::vector<TransitionRule> rules;
  rules.reserve(32);

  const EligibilityEffect unchanged = EligibilityEffect::Unchanged;
  const EligibilityEffect suspend = EligibilityEffect::Suspend;
  const EligibilityEffect resume = EligibilityEffect::Resume;
  const EligibilityEffect terminate = EligibilityEffect::Terminate;

  // --- Procurement and staging -------------------------------------------------
  add_rule(rules, LifecycleState::Ordered, LifecycleState::Staged, authority_of({AuthorityScope::Logistics}),
           evidence_of({EvidenceKind::DeliveryReceipt}), reasons_of({TransitionReason::DeliveryAccepted}), unchanged,
           false, false, "The unit arrived at a facility. It is on site, not in place, and carries no service.");
  add_rule(rules, LifecycleState::Ordered, LifecycleState::Removed, authority_of({AuthorityScope::Procurement}),
           evidence_of({EvidenceKind::CancellationRecord}), reasons_of({TransitionReason::OrderCancelled}), unchanged,
           false, false, "The commitment was cancelled before any physical unit existed at a facility.");
  add_rule(rules, LifecycleState::Staged, LifecycleState::Removed, authority_of({AuthorityScope::Logistics}),
           evidence_of({EvidenceKind::StorageRecord}),
           reasons_of({TransitionReason::StagingReturned, TransitionReason::StagingLoss}), unchanged, false, false,
           "The unit left the facility or was lost while staged. It never entered service.");
  add_rule(rules, LifecycleState::Staged, LifecycleState::Installed, authority_of({AuthorityScope::Installation}),
           evidence_of({EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord}),
           reasons_of({TransitionReason::InstallationCompleted}), unchanged, false, false,
           "The unit now occupies an assigned location. Installed is not commissioned and not active.");

  // --- Installation and commissioning ------------------------------------------
  add_rule(rules, LifecycleState::Installed, LifecycleState::Commissioning,
           authority_of({AuthorityScope::Commissioning}), EvidenceMask(), reasons_of({TransitionReason::CommissioningStarted}),
           unchanged, false, false, "Verification and qualification started.");
  add_rule(rules, LifecycleState::Installed, LifecycleState::Staged, authority_of({AuthorityScope::Installation}),
           evidence_of({EvidenceKind::InstallationRecord}), reasons_of({TransitionReason::InstallationReversed}),
           unchanged, false, false,
           "Declared reverse edge: de-installed back to staging before commissioning. No service was ever carried.");
  add_rule(rules, LifecycleState::Installed, LifecycleState::Removed, authority_of({AuthorityScope::Installation}),
           EvidenceMask(), reasons_of({TransitionReason::InstallationAbandoned}), unchanged, false, false,
           "Damaged or abandoned in place before commissioning. It never entered service.");
  add_rule(rules, LifecycleState::Commissioning, LifecycleState::Installed,
           authority_of({AuthorityScope::Commissioning}), evidence_of({EvidenceKind::CommissioningReport}),
           reasons_of({TransitionReason::CommissioningReworkRequired}), unchanged, false, false,
           "Commissioning stopped and the unit went back to installed pending rework or re-test.");
  add_rule(rules, LifecycleState::Commissioning, LifecycleState::Active, authority_of({AuthorityScope::Service}),
           evidence_of({EvidenceKind::CommissioningReport}), reasons_of({TransitionReason::CommissioningPassed}),
           resume, false, false,
           "Entering the service scope. Requires an explicitly opened service gate; the transition never opens it.");
  add_rule(rules, LifecycleState::Commissioning, LifecycleState::Quarantined, authority_of({AuthorityScope::Integrity}),
           evidence_of({EvidenceKind::IntegrityReport}), reasons_of({TransitionReason::CommissioningFailed}),
           unchanged, false, false, "Withheld pending an integrity decision. It never entered service.");
  add_rule(rules, LifecycleState::Commissioning, LifecycleState::Removed, authority_of({AuthorityScope::Commissioning}),
           EvidenceMask(), reasons_of({TransitionReason::CommissioningAbandoned}), unchanged, false, false,
           "Commissioning was abandoned and the unit was physically removed.");

  // --- In service ---------------------------------------------------------------
  add_rule(rules, LifecycleState::Active, LifecycleState::Degraded, authority_of({AuthorityScope::Service}),
           evidence_of({EvidenceKind::HealthEvidence}), reasons_of({TransitionReason::ServiceImpairmentAcknowledged}),
           unchanged, false, false,
           "An authority acknowledged impaired service. A health observation alone never performs this transition.");
  add_rule(rules, LifecycleState::Degraded, LifecycleState::Active, authority_of({AuthorityScope::Service}),
           evidence_of({EvidenceKind::ServiceRecord, EvidenceKind::HealthEvidence}),
           reasons_of({TransitionReason::ServiceRestored}), resume, false, false, "Service returned to full capability.");
  add_rule(rules, LifecycleState::Active, LifecycleState::Maintenance, authority_of({AuthorityScope::Maintenance}),
           evidence_of({EvidenceKind::MaintenanceRecord}), reasons_of({TransitionReason::MaintenanceScheduled}),
           suspend, false, false, "Withdrawn from service on purpose. The gate closes; maintenance is not removal.");
  add_rule(rules, LifecycleState::Degraded, LifecycleState::Maintenance, authority_of({AuthorityScope::Maintenance}),
           evidence_of({EvidenceKind::MaintenanceRecord}), reasons_of({TransitionReason::MaintenanceScheduled}),
           suspend, false, false, "Impaired service was withdrawn for maintenance.");
  add_rule(rules, LifecycleState::Maintenance, LifecycleState::Active, authority_of({AuthorityScope::Maintenance}),
           evidence_of({EvidenceKind::MaintenanceRecord}), reasons_of({TransitionReason::MaintenanceCompleted}),
           resume, false, false,
           "Maintenance completed. Returning to service requires the service gate to be open again.");
  add_rule(rules, LifecycleState::Maintenance, LifecycleState::Degraded, authority_of({AuthorityScope::Maintenance}),
           evidence_of({EvidenceKind::MaintenanceRecord}),
           reasons_of({TransitionReason::MaintenanceCompletedImpaired}), resume, false, false,
           "Maintenance completed with a known impairment; the unit returns to impaired service.");
  add_rule(rules, LifecycleState::Active, LifecycleState::Quarantined, authority_of({AuthorityScope::Integrity}),
           evidence_of({EvidenceKind::IntegrityReport}), reasons_of({TransitionReason::IntegrityConcernRaised}),
           suspend, false, false, "Withheld from service and from maintenance pending an integrity decision.");
  add_rule(rules, LifecycleState::Degraded, LifecycleState::Quarantined, authority_of({AuthorityScope::Integrity}),
           evidence_of({EvidenceKind::IntegrityReport}), reasons_of({TransitionReason::IntegrityConcernRaised}),
           suspend, false, false, "Impaired service was withheld pending an integrity decision.");
  add_rule(rules, LifecycleState::Maintenance, LifecycleState::Quarantined, authority_of({AuthorityScope::Integrity}),
           evidence_of({EvidenceKind::IntegrityReport}), reasons_of({TransitionReason::IntegrityConcernRaised}),
           suspend, false, false, "Maintenance turned into an integrity investigation.");
  add_rule(rules, LifecycleState::Quarantined, LifecycleState::Commissioning,
           authority_of({AuthorityScope::Integrity}), evidence_of({EvidenceKind::IntegrityReport}),
           reasons_of({TransitionReason::QuarantineReleasedForRecommissioning}), unchanged, false, false,
           "Quarantine released for re-commissioning. The unit re-enters the qualification path.");

  // --- Drain, decommission, dispose ---------------------------------------------
  add_rule(rules, LifecycleState::Active, LifecycleState::Retiring, authority_of({AuthorityScope::Retirement}),
           evidence_of({EvidenceKind::DrainRecord}), reasons_of({TransitionReason::RetirementApproved}), terminate,
           false, false, "Draining. Service is withdrawn; decommissioning has not happened.");
  add_rule(rules, LifecycleState::Degraded, LifecycleState::Retiring, authority_of({AuthorityScope::Retirement}),
           evidence_of({EvidenceKind::DrainRecord}), reasons_of({TransitionReason::RetirementApproved}), terminate,
           false, false, "Draining an impaired unit. Drained is not decommissioned.");
  add_rule(rules, LifecycleState::Maintenance, LifecycleState::Retiring, authority_of({AuthorityScope::Retirement}),
           evidence_of({EvidenceKind::DrainRecord}), reasons_of({TransitionReason::RetirementApproved}), terminate,
           false, false, "Maintenance was converted into retirement rather than a return to service.");
  add_rule(rules, LifecycleState::Quarantined, LifecycleState::Retiring, authority_of({AuthorityScope::Retirement}),
           evidence_of({EvidenceKind::DrainRecord}),
           reasons_of({TransitionReason::QuarantineReleasedForRetirement}), terminate, false, false,
           "Quarantine was resolved by retiring the unit. It still exists and is still present.");
  add_rule(rules, LifecycleState::Retiring, LifecycleState::Active, authority_of({AuthorityScope::Retirement}),
           evidence_of({EvidenceKind::DrainRecord}), reasons_of({TransitionReason::DrainCancelled}), resume, false,
           false, "Declared reverse edge: the drain was cancelled before decommissioning completed.");
  add_rule(rules, LifecycleState::Retiring, LifecycleState::Maintenance, authority_of({AuthorityScope::Retirement}),
           evidence_of({EvidenceKind::DrainRecord}), reasons_of({TransitionReason::DrainConvertedToMaintenance}),
           suspend, false, false, "Declared reverse edge: the drain became planned maintenance instead.");
  add_rule(rules, LifecycleState::Retiring, LifecycleState::Retired,
           authority_of({AuthorityScope::Decommissioning}), evidence_of({EvidenceKind::DecommissioningRecord}),
           reasons_of({TransitionReason::DecommissionCompleted}), terminate, false, false,
           "Decommissioned. The unit is still physically present.");
  add_rule(rules, LifecycleState::Quarantined, LifecycleState::Removed,
           authority_of({AuthorityScope::Decommissioning}), evidence_of({EvidenceKind::RemovalRecord}),
           reasons_of({TransitionReason::QuarantineReleasedForDisposal}), terminate, false, false,
           "Quarantine was resolved by physically removing the unit.");
  add_rule(rules, LifecycleState::Retired, LifecycleState::Replaced, authority_of({AuthorityScope::Replacement}),
           evidence_of({EvidenceKind::ReplacementAuthorization, EvidenceKind::SuccessorRecord}),
           reasons_of({TransitionReason::SuccessorLinked}), terminate, true, true,
           "A successor object exists. The predecessor keeps its identity, its history and its physical presence; "
           "only the successor reference is added.");
  add_rule(rules, LifecycleState::Retired, LifecycleState::Removed, authority_of({AuthorityScope::Decommissioning}),
           evidence_of({EvidenceKind::RemovalRecord}), reasons_of({TransitionReason::PhysicallyRemoved}), terminate,
           false, false, "The decommissioned unit was physically removed. Removed is terminal.");
  add_rule(rules, LifecycleState::Replaced, LifecycleState::Removed, authority_of({AuthorityScope::Decommissioning}),
           evidence_of({EvidenceKind::RemovalRecord}), reasons_of({TransitionReason::PhysicallyRemoved}), terminate,
           false, false, "The superseded unit was physically removed. Removed is terminal.");

  return rules;
}

}  // namespace

std::span<const TransitionRule> transition_rules() noexcept {
  static const std::vector<TransitionRule> rules = build_transition_rules();
  return std::span<const TransitionRule>(rules.data(), rules.size());
}

const TransitionRule* find_rule(LifecycleState from, LifecycleState to) noexcept {
  for (const TransitionRule& rule : transition_rules()) {
    if (rule.from == from && rule.to == to) {
      return &rule;
    }
  }
  return nullptr;
}

std::vector<LifecycleState> legal_targets(LifecycleState from) {
  std::vector<LifecycleState> targets;
  for (const TransitionRule& rule : transition_rules()) {
    if (rule.from == from) {
      targets.push_back(rule.to);
    }
  }
  return targets;
}

bool is_legal_transition(LifecycleState from, LifecycleState to) noexcept { return find_rule(from, to) != nullptr; }

std::string describe_rule(const TransitionRule& rule) {
  std::string text;
  text += std::string(to_string(rule.from));
  text += " -> ";
  text += std::string(to_string(rule.to));
  text += " | authority: ";
  text += to_string(rule.required_authority);
  text += " | evidence: ";
  text += rule.required_evidence.empty() ? std::string("any") : to_string(rule.required_evidence);
  text += " | reasons: ";
  text += to_string(rule.allowed_reasons);
  text += " | eligibility: ";
  text += std::string(to_string(rule.eligibility_effect));
  if (rule.requires_location) {
    text += " | requires_location";
  }
  if (rule.requires_successor_link) {
    text += " | requires_successor";
  }
  if (rule.requires_successor_installed) {
    text += " | successor_must_be_installed";
  }
  text += " | ";
  text += std::string(rule.note);
  return text;
}

std::string lifecycle_machine_diagram() {
  std::string text;
  for (LifecycleState state : all_lifecycle_states()) {
    text += to_string(state);
    text += " [";
    text += to_string(state_class(state));
    text += "]";
    const std::vector<LifecycleState> targets = legal_targets(state);
    if (targets.empty()) {
      text += " -> (terminal)";
    } else {
      text += " -> ";
      for (std::size_t index = 0; index < targets.size(); ++index) {
        if (index != 0) {
          text += ", ";
        }
        text += to_string(targets[index]);
      }
    }
    text += "\n";
  }
  return text;
}

}  // namespace hardware_lifecycle