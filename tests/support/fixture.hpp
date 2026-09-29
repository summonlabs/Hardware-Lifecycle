#pragma once

// Hardware Lifecycle - shared test fixture.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The fixture never hardcodes what a transition requires. It reads the
// requirements out of the authoritative transition table, so a suite that drives
// a walk through this fixture is exercising the table rather than a second,
// drifting copy of it. Suites that want to prove a rejection still build the
// broken request by hand.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <hardware_lifecycle/hardware_lifecycle.hpp>

namespace hl_fixture {

using namespace hardware_lifecycle;

/// Deterministic evidence digest: 32 bytes of one value.
[[nodiscard]] inline Digest digest_of(std::uint8_t seed) {
  std::array<std::uint8_t, Digest::kSize> bytes{};
  bytes.fill(seed);
  return Digest::from_bytes(bytes);
}

[[nodiscard]] inline std::string plan_text(int counter) { return "plan-" + std::to_string(counter); }

/// The first scope in declaration order that the mask grants.
[[nodiscard]] inline AuthorityScope first_scope(AuthorityMask mask) {
  for (std::uint8_t index = 0; index < kAuthorityScopeCount; ++index) {
    const auto scope = static_cast<AuthorityScope>(index);
    if (mask.intersects(authority_bit(scope))) {
      return scope;
    }
  }
  return AuthorityScope::Service;
}

/// The first reason in declaration order that the rule allows.
[[nodiscard]] inline TransitionReason first_reason(ReasonMask mask) {
  for (std::uint8_t index = 1; index < kTransitionReasonCount; ++index) {
    const auto reason = static_cast<TransitionReason>(index);
    if (reason_allowed(mask, reason)) {
      return reason;
    }
  }
  return TransitionReason::Unset;
}

/// Every evidence kind the rule demands, in declaration order. An empty mask
/// yields one arbitrary kind, because a transition still needs evidence.
[[nodiscard]] inline std::vector<EvidenceKind> required_kinds(EvidenceMask mask) {
  std::vector<EvidenceKind> kinds;
  for (std::uint8_t index = 1; index < kEvidenceKindCount; ++index) {
    const auto kind = static_cast<EvidenceKind>(index);
    if (mask.intersects(evidence_bit(kind))) {
      kinds.push_back(kind);
    }
  }
  if (kinds.empty()) {
    kinds.push_back(EvidenceKind::ServiceRecord);
  }
  return kinds;
}

/// Breadth first path over the authoritative table. Empty when the target is
/// not reachable; otherwise the first element is "from" and the last is "to".
[[nodiscard]] inline std::vector<LifecycleState> path_between(LifecycleState from, LifecycleState to) {
  const std::span<const LifecycleState> states = all_lifecycle_states();
  const std::size_t count = states.size();
  std::vector<int> previous(count, -1);
  std::vector<bool> seen(count, false);
  std::vector<std::size_t> queue;
  const auto index_of = [&states](LifecycleState state) {
    for (std::size_t index = 0; index < states.size(); ++index) {
      if (states[index] == state) {
        return index;
      }
    }
    return states.size();
  };
  const std::size_t start = index_of(from);
  const std::size_t goal = index_of(to);
  if (start == count || goal == count) {
    return {};
  }
  seen[start] = true;
  queue.push_back(start);
  for (std::size_t head = 0; head < queue.size(); ++head) {
    const std::size_t current = queue[head];
    if (current == goal) {
      break;
    }
    for (LifecycleState next : legal_targets(states[current])) {
      const std::size_t next_index = index_of(next);
      if (next_index < count && !seen[next_index]) {
        seen[next_index] = true;
        previous[next_index] = static_cast<int>(current);
        queue.push_back(next_index);
      }
    }
  }
  if (!seen[goal]) {
    return {};
  }
  std::vector<LifecycleState> path;
  for (int at = static_cast<int>(goal); at != -1; at = previous[static_cast<std::size_t>(at)]) {
    path.push_back(states[static_cast<std::size_t>(at)]);
  }
  std::reverse(path.begin(), path.end());
  return path;
}

/// Drives a runtime through the real transition table.
class Driver {
 public:
  /// The prefix makes plan identities unique per session, which they must be:
  /// the applied plan ledger is durable, so a second session that reused a plan
  /// id would be told plan_conflict, correctly.
  explicit Driver(Runtime& runtime, std::string prefix = "plan")
      : runtime_(runtime), prefix_(std::move(prefix)) {
    actor_.id = ActorId::parse("operator-1").value();
    actor_.kind = ActorKind::Operator;
  }

  [[nodiscard]] std::string plan_text(int counter) const { return prefix_ + "-" + std::to_string(counter); }

  void set_actor(const char* id, ActorKind kind) {
    actor_.id = ActorId::parse(id).value();
    actor_.kind = kind;
  }

  [[nodiscard]] int take_plan() noexcept { return ++counter_; }

  [[nodiscard]] Provenance provenance_for(int plan, AuthorityMask authority, EvidenceKind kind,
                                          const char* source) const {
    return provenance_for_mask(plan, authority, evidence_bit(kind), source);
  }

  /// One evidence reference per required kind, each with its own digest so that
  /// the references are distinct facts rather than one fact repeated.
  [[nodiscard]] Provenance provenance_for_mask(int plan, AuthorityMask authority, EvidenceMask required,
                                               const char* source) const {
    Provenance value;
    value.actor = actor_;
    value.authority = authority;
    value.policy_generation = PolicyGeneration::first();
    value.plan = PlanId::parse(plan_text(plan)).value();
    value.attempt = AttemptId::parse("attempt-1").value();
    std::uint8_t seed = static_cast<std::uint8_t>(0x20 + plan);
    for (const EvidenceKind kind : required_kinds(required)) {
      EvidenceRef evidence;
      evidence.kind = kind;
      evidence.digest = digest_of(seed++);
      evidence.source = source;
      value.evidence.push_back(evidence);
    }
    return value;
  }

  [[nodiscard]] Result<CreateReceipt> register_object(const char* asset,
                                                      LifecycleState initial = LifecycleState::Ordered,
                                                      HardwareGeneration generation = HardwareGeneration::first(),
                                                      const char* model = "model-x",
                                                      HardwareKind kind = HardwareKind::Compute) {
    const int plan = take_plan();
    CreateRequest request;
    request.plan = PlanId::parse(plan_text(plan)).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key.asset = AssetId::parse(asset).value();
    request.key.hardware_generation = generation;
    request.kind = kind;
    request.model = ModelId::parse(model).value();
    request.firmware_generation = FirmwareGeneration::first();
    request.initial_state = initial;
    const bool staged = initial == LifecycleState::Staged;
    request.provenance = provenance_for(
        plan, authority_bit(staged ? AuthorityScope::Logistics : AuthorityScope::Procurement),
        staged ? EvidenceKind::DeliveryReceipt : EvidenceKind::ProcurementRecord,
        staged ? "wms/receipt" : "erp/purchase-order");
    return runtime_.create_object(request);
  }

  [[nodiscard]] Result<EligibilityGateReceipt> set_gate(const ObjectKey& key, EligibilityGate gate) {
    const Result<ObjectView> view = runtime_.inspect(key);
    if (!view.has_value()) {
      return view.error();
    }
    const int plan = take_plan();
    EligibilityGateRequest request;
    request.plan = PlanId::parse(plan_text(plan)).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key;
    request.gate = gate;
    request.expected_revision = view.value().object.revision;
    request.expected_lifecycle_generation = view.value().object.lifecycle_generation;
    request.expected_control_epoch = runtime_.registry().control_epoch();
    request.provenance =
        provenance_for(plan, authority_bit(AuthorityScope::Service), EvidenceKind::ServiceRecord, "itsm/gate");
    return runtime_.set_eligibility_gate(request);
  }

  [[nodiscard]] Result<AttestationReceipt> attest(const ObjectKey& key) {
    const Result<ObjectView> view = runtime_.inspect(key);
    if (!view.has_value()) {
      return view.error();
    }
    const int plan = take_plan();
    AttestationRequest request;
    request.plan = PlanId::parse(plan_text(plan)).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key;
    request.expected_revision = view.value().object.revision;
    request.expected_control_epoch = runtime_.registry().control_epoch();
    request.provenance = provenance_for(plan, authority_bit(AuthorityScope::Recovery),
                                        EvidenceKind::RecoveryAttestation, "operator/attestation");
    return runtime_.attest_authority(request);
  }

  /// One legal step, with every requirement taken from the transition table.
  [[nodiscard]] Result<TransitionReceipt> step(const ObjectKey& key, LifecycleState target,
                                               std::optional<ObjectKey> successor = std::nullopt) {
    const Result<ObjectView> view = runtime_.inspect(key);
    if (!view.has_value()) {
      return view.error();
    }
    const HardwareObject& object = view.value().object;
    const TransitionRule* rule = find_rule(object.state, target);
    if (rule == nullptr) {
      return Error::make(ErrorCode::IllegalTransition, "the fixture was asked for an edge the table does not have");
    }
    if (occupies_service_scope(rule->to) && !occupies_service_scope(rule->from) &&
        object.eligibility_gate != EligibilityGate::Open) {
      const Result<EligibilityGateReceipt> gate = set_gate(key, EligibilityGate::Open);
      if (!gate.has_value()) {
        return gate.error();
      }
      return step(key, target, successor);
    }
    const int plan = take_plan();
    TransitionRequest request;
    request.plan = PlanId::parse(plan_text(plan)).value();
    request.attempt = AttemptId::parse("attempt-1").value();
    request.key = key;
    request.expected_lifecycle_generation = object.lifecycle_generation;
    request.expected_revision = object.revision;
    request.expected_control_epoch = runtime_.registry().control_epoch();
    request.expected_state = object.state;
    request.target_state = target;
    request.reason = first_reason(rule->allowed_reasons);
    request.successor = successor;
    if (rule->requires_location && !object.location.has_value()) {
      Location location;
      location.site = SiteId::parse("dc-1").value();
      location.rack = RackId::parse("rack-01").value();
      location.slot = SlotId::parse("u" + std::to_string(plan)).value();
      request.location = location;
    }
    request.provenance = provenance_for_mask(plan, authority_bit(first_scope(rule->required_authority)),
                                             rule->required_evidence, "system/evidence");
    return runtime_.apply_transition(request);
  }

  /// Walks the object to the target state along table edges.
  [[nodiscard]] Result<void> advance_to(const ObjectKey& key, LifecycleState target) {
    for (int guard = 0; guard < 64; ++guard) {
      const Result<ObjectView> view = runtime_.inspect(key);
      if (!view.has_value()) {
        return view.error();
      }
      if (view.value().object.state == target) {
        return ok();
      }
      const std::vector<LifecycleState> path = path_between(view.value().object.state, target);
      if (path.size() < 2) {
        return Error::make(ErrorCode::IllegalTransition, "the fixture could not find a path to the target state");
      }
      const Result<TransitionReceipt> receipt = step(key, path[1]);
      if (!receipt.has_value()) {
        return receipt.error();
      }
    }
    return Error::make(ErrorCode::Internal, "the fixture walk did not converge");
  }

  [[nodiscard]] const HardwareObject& object(const ObjectKey& key) const {
    return *runtime_.registry().find(key);
  }

 private:
  Runtime& runtime_;
  Actor actor_;
  std::string prefix_ = "plan";
  int counter_ = 0;
};

}  // namespace hl_fixture