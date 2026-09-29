// Hardware Lifecycle - domain proofs for lifecycle transitions.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A lifecycle change happens only through the transition table, only inside the
// service scope rules, and only after the durable commit point: the receipt is
// issued after the state is published, a rejected request leaves the previous
// generation exactly as it was, and the primary error of an invalid request is a
// deterministic function of the request and the state it was planned against.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hardware_lifecycle/hardware_lifecycle.hpp"
#include "test_framework.hpp"

namespace {

using namespace hardware_lifecycle;

int plan_counter = 0;

[[nodiscard]] Digest digest_of(std::uint8_t seed) {
  std::array<std::uint8_t, Digest::kSize> bytes{};
  bytes.fill(seed);
  return Digest::from_bytes(bytes);
}

[[nodiscard]] PlanId next_plan() { return PlanId::parse("plan-" + std::to_string(++plan_counter)).value(); }

[[nodiscard]] AttemptId attempt_id() { return AttemptId::parse("attempt-1").value(); }

[[nodiscard]] Provenance provenance_for(AuthorityMask authority, const std::vector<EvidenceKind>& kinds) {
  Provenance value;
  value.actor.id = ActorId::parse("operator-1").value();
  value.actor.kind = ActorKind::Operator;
  value.authority = authority;
  value.policy_generation = PolicyGeneration::first();
  value.plan = next_plan();
  value.attempt = attempt_id();
  for (std::size_t index = 0; index < kinds.size(); ++index) {
    EvidenceRef evidence;
    evidence.kind = kinds[index];
    evidence.digest = digest_of(static_cast<std::uint8_t>(0x20u + index));
    evidence.source = std::string("evidence/") + std::string(to_string(kinds[index]));
    value.evidence.push_back(std::move(evidence));
  }
  return value;
}

[[nodiscard]] Location location_of(const char* slot) {
  Location location;
  location.site = SiteId::parse("site-a").value();
  location.rack = RackId::parse("rack-1").value();
  location.slot = SlotId::parse(slot).value();
  return location;
}

/// One edge of a planned walk: the table is asked for the authority, the
/// evidence and the reason, so a walk can never invent an edge.
struct Edge {
  LifecycleState target = LifecycleState::Ordered;
  TransitionReason reason = TransitionReason::Unset;
  AuthorityScope scope = AuthorityScope::Procurement;
  std::vector<EvidenceKind> evidence;
  bool with_location = false;
};

[[nodiscard]] Result<CreateReceipt> create_object_in(Runtime& runtime, const char* asset,
                                                     LifecycleState initial = LifecycleState::Ordered) {
  CreateRequest request;
  request.key.asset = AssetId::parse(asset).value();
  request.key.hardware_generation = HardwareGeneration::first();
  request.kind = HardwareKind::Compute;
  request.model = ModelId::parse("model-x").value();
  request.firmware_generation = FirmwareGeneration::first();
  request.initial_state = initial;
  const bool staged = initial == LifecycleState::Staged;
  request.provenance = provenance_for(authority_bit(staged ? AuthorityScope::Logistics : AuthorityScope::Procurement),
                                      {staged ? EvidenceKind::DeliveryReceipt : EvidenceKind::ProcurementRecord});
  request.plan = request.provenance.plan;
  request.attempt = request.provenance.attempt;
  return runtime.create_object(request);
}

/// The request an edge would be applied with. Callers may mutate one field to
/// prove which violation the validator reports.
[[nodiscard]] Result<TransitionRequest> request_for_edge(const Runtime& runtime, const ObjectKey& key,
                                                         const Edge& edge) {
  const Result<ObjectView> view = runtime.inspect(key);
  if (!view.has_value()) {
    return view.error();
  }
  TransitionRequest request;
  request.key = key;
  request.expected_state = view.value().object.state;
  request.target_state = edge.target;
  request.expected_revision = view.value().object.revision;
  request.expected_lifecycle_generation = view.value().object.lifecycle_generation;
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.reason = edge.reason;
  if (edge.with_location) {
    request.location = location_of("slot-1");
  }
  request.provenance = provenance_for(authority_bit(edge.scope), edge.evidence);
  request.plan = request.provenance.plan;
  request.attempt = request.provenance.attempt;
  return request;
}

[[nodiscard]] Result<TransitionReceipt> apply_edge(Runtime& runtime, const ObjectKey& key, const Edge& edge) {
  const Result<TransitionRequest> request = request_for_edge(runtime, key, edge);
  if (!request.has_value()) {
    return request.error();
  }
  return runtime.apply_transition(request.value());
}

[[nodiscard]] Result<EligibilityGateReceipt> set_gate(Runtime& runtime, const ObjectKey& key, EligibilityGate gate) {
  const Result<ObjectView> view = runtime.inspect(key);
  if (!view.has_value()) {
    return view.error();
  }
  EligibilityGateRequest request;
  request.key = key;
  request.gate = gate;
  request.expected_revision = view.value().object.revision;
  request.expected_lifecycle_generation = view.value().object.lifecycle_generation;
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.provenance = provenance_for(authority_bit(AuthorityScope::Service), {EvidenceKind::ServiceRecord});
  request.plan = request.provenance.plan;
  request.attempt = request.provenance.attempt;
  return runtime.set_eligibility_gate(request);
}

/// Walks the planned edges, opening the gate before an edge that returns the
/// object to service, exactly as the table declares it.
[[nodiscard]] Result<void> advance(Runtime& runtime, const ObjectKey& key, const std::vector<Edge>& path) {
  for (const Edge& edge : path) {
    const Result<ObjectView> view = runtime.inspect(key);
    if (!view.has_value()) {
      return view.error();
    }
    const TransitionRule* rule = find_rule(view.value().object.state, edge.target);
    if (rule == nullptr) {
      return Error::make(ErrorCode::IllegalTransition, "the planned walk asked for an edge the table does not have");
    }
    if (rule->eligibility_effect == EligibilityEffect::Resume &&
        view.value().object.eligibility_gate != EligibilityGate::Open) {
      const Result<EligibilityGateReceipt> gate = set_gate(runtime, key, EligibilityGate::Open);
      if (!gate.has_value()) {
        return gate.error();
      }
    }
    const Result<TransitionReceipt> receipt = apply_edge(runtime, key, edge);
    if (!receipt.has_value()) {
      return receipt.error();
    }
  }
  return ok();
}

[[nodiscard]] std::vector<Edge> path_to_staged() {
  return {{LifecycleState::Staged, TransitionReason::DeliveryAccepted, AuthorityScope::Logistics,
           {EvidenceKind::DeliveryReceipt}, false}};
}

[[nodiscard]] std::vector<Edge> path_to_installed() {
  std::vector<Edge> path = path_to_staged();
  path.push_back({LifecycleState::Installed, TransitionReason::InstallationCompleted, AuthorityScope::Installation,
                  {EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord}, true});
  return path;
}

[[nodiscard]] std::vector<Edge> path_to_commissioning() {
  std::vector<Edge> path = path_to_installed();
  path.push_back({LifecycleState::Commissioning, TransitionReason::CommissioningStarted,
                  AuthorityScope::Commissioning, {EvidenceKind::CommissioningReport}, false});
  return path;
}

[[nodiscard]] std::vector<Edge> path_to_active() {
  std::vector<Edge> path = path_to_commissioning();
  path.push_back({LifecycleState::Active, TransitionReason::CommissioningPassed, AuthorityScope::Service,
                  {EvidenceKind::CommissioningReport}, false});
  return path;
}

[[nodiscard]] std::vector<Edge> path_to_maintenance() {
  std::vector<Edge> path = path_to_active();
  path.push_back({LifecycleState::Maintenance, TransitionReason::MaintenanceScheduled, AuthorityScope::Maintenance,
                  {EvidenceKind::MaintenanceRecord}, false});
  return path;
}

[[nodiscard]] std::vector<Edge> path_to_retired() {
  std::vector<Edge> path = path_to_commissioning();
  path.push_back({LifecycleState::Quarantined, TransitionReason::CommissioningFailed, AuthorityScope::Integrity,
                  {EvidenceKind::IntegrityReport}, false});
  path.push_back({LifecycleState::Retiring, TransitionReason::QuarantineReleasedForRetirement,
                  AuthorityScope::Retirement, {EvidenceKind::DrainRecord}, false});
  path.push_back({LifecycleState::Retired, TransitionReason::DecommissionCompleted,
                  AuthorityScope::Decommissioning, {EvidenceKind::DecommissioningRecord}, false});
  return path;
}

[[nodiscard]] std::vector<Edge> path_to_removed() {
  std::vector<Edge> path = path_to_retired();
  path.push_back({LifecycleState::Removed, TransitionReason::PhysicallyRemoved, AuthorityScope::Decommissioning,
                  {EvidenceKind::RemovalRecord}, false});
  return path;
}

/// A receipt is only meaningful when it is bound to the request, to the chain
/// entry and to its own digest.
void expect_receipt_is_bound(hl_test::Context& ctx, const TransitionReceipt& receipt) {
  (void)ctx.check(!receipt.request_digest.is_zero(), "request digest is computed", __FILE__, __LINE__);
  (void)ctx.check(!receipt.entry_digest.is_zero(), "entry digest is computed", __FILE__, __LINE__);
  (void)ctx.check(!receipt.receipt_digest.is_zero(), "receipt digest is computed", __FILE__, __LINE__);
  (void)ctx.check_eq(compute_receipt_digest(receipt), receipt.receipt_digest, "compute_receipt_digest(receipt)",
                     "receipt.receipt_digest", __FILE__, __LINE__);
}

}  // namespace

// ---------------------------------------------------------------------------
// The legal walk
// ---------------------------------------------------------------------------

HL_TEST(domain_transitions, full_legal_walk_from_ordered_to_removed) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  const ObjectKey key = created.value().key;
  HL_CHECK_EQ(created.value().state, LifecycleState::Ordered);
  HL_CHECK_EQ(created.value().revision, Revision::first());
  HL_CHECK_EQ(created.value().lifecycle_generation, LifecycleGeneration::first());
  HL_CHECK_EQ(created.value().idempotent_replay, false);
  HL_CHECK_EQ(created.value().commit_sequence, CommitSequence::first());

  // Ordered -> Staged: logistics authority, a delivery receipt, no gate change.
  const Result<TransitionReceipt> staged =
      apply_edge(runtime, key, {LifecycleState::Staged, TransitionReason::DeliveryAccepted,
                                AuthorityScope::Logistics, {EvidenceKind::DeliveryReceipt}, false});
  HL_REQUIRE(staged.has_value());
  HL_CHECK_EQ(staged.value().from, LifecycleState::Ordered);
  HL_CHECK_EQ(staged.value().to, LifecycleState::Staged);
  HL_CHECK_EQ(staged.value().reason, TransitionReason::DeliveryAccepted);
  HL_CHECK_EQ(staged.value().revision_before, Revision::first());
  HL_CHECK_EQ(staged.value().revision_after, Revision::from_value(2));
  HL_CHECK_EQ(staged.value().lifecycle_generation, LifecycleGeneration::from_value(2));
  HL_CHECK_EQ(staged.value().gate_after, EligibilityGate::Unknown);
  HL_CHECK_EQ(staged.value().commit_sequence, runtime.registry().commit_sequence());
  HL_CHECK_EQ(staged.value().idempotent_replay, false);
  expect_receipt_is_bound(hl_ctx, staged.value());

  // Staged -> Installed: the target state demands a location, so the request
  // must carry one and the object keeps it.
  const Result<TransitionReceipt> installed =
      apply_edge(runtime, key, {LifecycleState::Installed, TransitionReason::InstallationCompleted,
                                AuthorityScope::Installation,
                                {EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord}, true});
  HL_REQUIRE(installed.has_value());
  HL_CHECK_EQ(installed.value().to, LifecycleState::Installed);
  HL_CHECK_EQ(installed.value().revision_after, Revision::from_value(3));
  HL_CHECK_EQ(installed.value().lifecycle_generation, LifecycleGeneration::from_value(3));
  {
    const Result<ObjectView> view = runtime.inspect(key);
    HL_REQUIRE(view.has_value());
    HL_CHECK_EQ(view.value().object.location.has_value(), true);
  }

  // Installed -> Commissioning: any single evidence reference satisfies the rule.
  const Result<TransitionReceipt> commissioning =
      apply_edge(runtime, key, {LifecycleState::Commissioning, TransitionReason::CommissioningStarted,
                                AuthorityScope::Commissioning, {EvidenceKind::CommissioningReport}, false});
  HL_REQUIRE(commissioning.has_value());
  HL_CHECK_EQ(commissioning.value().revision_after, Revision::from_value(4));
  HL_CHECK_EQ(commissioning.value().lifecycle_generation, LifecycleGeneration::from_value(4));

  // The gate is separate authority: it is opened by an explicit decision that
  // advances the revision and appends a history entry but does not move the
  // lifecycle generation.
  const Result<EligibilityGateReceipt> opened_gate = set_gate(runtime, key, EligibilityGate::Open);
  HL_REQUIRE(opened_gate.has_value());
  HL_CHECK_EQ(opened_gate.value().before, EligibilityGate::Unknown);
  HL_CHECK_EQ(opened_gate.value().after, EligibilityGate::Open);
  HL_CHECK_EQ(opened_gate.value().revision, Revision::from_value(5));
  HL_CHECK_EQ(opened_gate.value().commit_sequence, runtime.registry().commit_sequence());
  {
    const Result<ObjectView> view = runtime.inspect(key);
    HL_REQUIRE(view.has_value());
    HL_CHECK_EQ(view.value().object.revision, Revision::from_value(5));
    HL_CHECK_EQ(view.value().object.lifecycle_generation, LifecycleGeneration::from_value(4));
    HL_CHECK_EQ(view.value().object.eligibility_gate, EligibilityGate::Open);
  }

  // Commissioning -> Active: entering the service scope, with an open gate.
  const Result<TransitionReceipt> active =
      apply_edge(runtime, key, {LifecycleState::Active, TransitionReason::CommissioningPassed,
                                AuthorityScope::Service, {EvidenceKind::CommissioningReport}, false});
  HL_REQUIRE(active.has_value());
  HL_CHECK_EQ(active.value().to, LifecycleState::Active);
  HL_CHECK_EQ(active.value().revision_after, Revision::from_value(6));
  HL_CHECK_EQ(active.value().lifecycle_generation, LifecycleGeneration::from_value(5));
  HL_CHECK_EQ(active.value().gate_after, EligibilityGate::Open);
  expect_receipt_is_bound(hl_ctx, active.value());

  // Active -> Retiring: draining closes the gate and is not decommissioning.
  const Result<TransitionReceipt> retiring =
      apply_edge(runtime, key, {LifecycleState::Retiring, TransitionReason::RetirementApproved,
                                AuthorityScope::Retirement, {EvidenceKind::DrainRecord}, false});
  HL_REQUIRE(retiring.has_value());
  HL_CHECK_EQ(retiring.value().to, LifecycleState::Retiring);
  HL_CHECK_EQ(retiring.value().gate_after, EligibilityGate::Closed);
  HL_CHECK_EQ(retiring.value().revision_after, Revision::from_value(7));
  HL_CHECK_EQ(is_decommissioned(LifecycleState::Retiring), false);

  // Retiring -> Retired: decommissioning completes; the unit is still present.
  const Result<TransitionReceipt> retired =
      apply_edge(runtime, key, {LifecycleState::Retired, TransitionReason::DecommissionCompleted,
                                AuthorityScope::Decommissioning, {EvidenceKind::DecommissioningRecord}, false});
  HL_REQUIRE(retired.has_value());
  HL_CHECK_EQ(retired.value().to, LifecycleState::Retired);
  HL_CHECK_EQ(retired.value().gate_after, EligibilityGate::Closed);
  HL_CHECK_EQ(retired.value().revision_after, Revision::from_value(8));
  HL_CHECK_EQ(retired.value().lifecycle_generation, LifecycleGeneration::from_value(7));
  HL_CHECK_EQ(retired.value().commit_sequence, runtime.registry().commit_sequence());

  // Retired -> Removed: the physical unit is gone and the state is terminal.
  const Result<TransitionReceipt> removed =
      apply_edge(runtime, key, {LifecycleState::Removed, TransitionReason::PhysicallyRemoved,
                                AuthorityScope::Decommissioning, {EvidenceKind::RemovalRecord}, false});
  HL_REQUIRE(removed.has_value());
  HL_CHECK_EQ(removed.value().to, LifecycleState::Removed);
  HL_CHECK_EQ(removed.value().revision_after, Revision::from_value(9));
  HL_CHECK_EQ(removed.value().lifecycle_generation, LifecycleGeneration::from_value(8));
  HL_CHECK_EQ(removed.value().gate_after, EligibilityGate::Closed);
  expect_receipt_is_bound(hl_ctx, removed.value());

  const Result<ObjectView> end = runtime.inspect(key);
  HL_REQUIRE(end.has_value());
  HL_CHECK_EQ(end.value().object.state, LifecycleState::Removed);
  HL_CHECK_EQ(end.value().object.history_entries, static_cast<std::uint64_t>(9));
  HL_CHECK_EQ(end.value().object.revision, Revision::from_value(9));
  HL_CHECK_EQ(end.value().object.lifecycle_generation, LifecycleGeneration::from_value(8));
  HL_CHECK_EQ(is_terminal(end.value().object.state), true);
  HL_CHECK_EQ(runtime.registry().object_count(), static_cast<std::size_t>(1));
  HL_CHECK_EQ(runtime.registry().history_entry_count(), static_cast<std::size_t>(9));
  HL_CHECK_EQ(runtime.registry().commit_sequence(), CommitSequence::from_value(9));

  // The composed state is consistent with its own history and its own watermarks.
  HL_CHECK(runtime.registry().verify_invariants().has_value());
  HL_CHECK(runtime.verify().has_value());
}

HL_TEST(domain_transitions, a_gate_decision_is_a_durable_history_entry_that_does_not_move_the_state) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  const ObjectKey key = created.value().key;
  HL_REQUIRE(advance(runtime, key, path_to_commissioning()).has_value());

  const Result<EligibilityGateReceipt> opened_gate = set_gate(runtime, key, EligibilityGate::Open);
  HL_REQUIRE(opened_gate.has_value());
  const Result<HistoryView> opened_history = runtime.history(key);
  HL_REQUIRE(opened_history.has_value());
  HL_CHECK_EQ(opened_history.value().entries.size(), static_cast<std::size_t>(5));
  const HistoryEntry& opening_entry = opened_history.value().entries.back();
  HL_CHECK_EQ(opening_entry.is_gate_decision(), true);
  HL_CHECK_EQ(opening_entry.is_registration(), false);
  HL_CHECK_EQ(opening_entry.changes_state(), false);
  HL_CHECK_EQ(opening_entry.from, LifecycleState::Commissioning);
  HL_CHECK_EQ(opening_entry.to, LifecycleState::Commissioning);
  HL_CHECK_EQ(opening_entry.reason, TransitionReason::EligibilityGateOpened);
  HL_CHECK_EQ(opening_entry.lifecycle_generation, LifecycleGeneration::from_value(4));
  HL_CHECK_EQ(opening_entry.revision_before, Revision::from_value(4));
  HL_CHECK_EQ(opening_entry.revision_after, Revision::from_value(5));
  HL_CHECK_EQ(opening_entry.receipt_digest, opened_gate.value().receipt_digest);
  {
    HistoryLog log;
    log.entries = opened_history.value().entries;
    log.chain_head = opened_history.value().chain_head;
    HL_CHECK(verify_history_chain(log).has_value());
    HL_CHECK(replay_history(log).has_value());
    HL_CHECK_EQ(replay_history(log).value(), LifecycleState::Commissioning);
  }

  const Result<TransitionReceipt> active =
      apply_edge(runtime, key, {LifecycleState::Active, TransitionReason::CommissioningPassed,
                                AuthorityScope::Service, {EvidenceKind::CommissioningReport}, false});
  HL_REQUIRE(active.has_value());

  // A gate decision may also close the gate while the object stays where it is.
  const Result<EligibilityGateReceipt> closed_gate = set_gate(runtime, key, EligibilityGate::Closed);
  HL_REQUIRE(closed_gate.has_value());
  HL_CHECK_EQ(closed_gate.value().before, EligibilityGate::Open);
  HL_CHECK_EQ(closed_gate.value().after, EligibilityGate::Closed);
  HL_CHECK_EQ(closed_gate.value().revision, Revision::from_value(7));

  const Result<HistoryView> closed_history = runtime.history(key);
  HL_REQUIRE(closed_history.has_value());
  const HistoryEntry& closing_entry = closed_history.value().entries.back();
  HL_CHECK_EQ(closing_entry.is_gate_decision(), true);
  HL_CHECK_EQ(closing_entry.reason, TransitionReason::EligibilityGateClosed);
  HL_CHECK_EQ(closing_entry.from, LifecycleState::Active);
  HL_CHECK_EQ(closing_entry.to, LifecycleState::Active);
  HL_CHECK_EQ(closing_entry.lifecycle_generation, LifecycleGeneration::from_value(5));
  HL_CHECK_EQ(closing_entry.revision_after, Revision::from_value(7));

  const Result<ObjectView> view = runtime.inspect(key);
  HL_REQUIRE(view.has_value());
  HL_CHECK_EQ(view.value().object.state, LifecycleState::Active);
  HL_CHECK_EQ(view.value().object.eligibility_gate, EligibilityGate::Closed);
  HL_CHECK_EQ(view.value().object.lifecycle_generation, LifecycleGeneration::from_value(5));
  HL_CHECK_EQ(service_eligibility(view.value().object.state, view.value().object.eligibility_gate),
              ServiceEligibility::Ineligible);

  // The chain still replays onto the object, gate entries included.
  {
    HistoryLog log;
    log.entries = closed_history.value().entries;
    log.chain_head = closed_history.value().chain_head;
    HL_CHECK(verify_history_chain(log).has_value());
    HL_CHECK(verify_history_against_object(log, view.value().object).has_value());
  }
  HL_CHECK(runtime.registry().verify_invariants().has_value());
  HL_CHECK(runtime.verify().has_value());
}

// ---------------------------------------------------------------------------
// Replay and plan identity
// ---------------------------------------------------------------------------

HL_TEST(domain_transitions, idempotent_replay_returns_the_original_receipt_and_changes_nothing) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  const ObjectKey key = created.value().key;

  const Edge edge{LifecycleState::Staged, TransitionReason::DeliveryAccepted, AuthorityScope::Logistics,
                  {EvidenceKind::DeliveryReceipt}, false};
  const Result<TransitionRequest> request = request_for_edge(runtime, key, edge);
  HL_REQUIRE(request.has_value());

  const Result<TransitionReceipt> first = runtime.apply_transition(request.value());
  HL_REQUIRE(first.has_value());
  HL_CHECK_EQ(first.value().idempotent_replay, false);

  const Digest digest_after_first = runtime.registry().state_digest();
  const std::size_t history_after_first = runtime.registry().history_entry_count();
  const std::size_t plans_after_first = runtime.registry().plan_count();

  // The identical request is a lost response, not a stale plan: it returns the
  // receipt that was issued the first time, byte for byte.
  const Result<TransitionReceipt> replay = runtime.apply_transition(request.value());
  HL_REQUIRE(replay.has_value());
  HL_CHECK_EQ(replay.value().idempotent_replay, true);
  HL_CHECK_EQ(replay.value().receipt_digest, first.value().receipt_digest);
  HL_CHECK_EQ(replay.value().entry_digest, first.value().entry_digest);
  HL_CHECK_EQ(replay.value().request_digest, first.value().request_digest);
  HL_CHECK_EQ(replay.value().commit_sequence, first.value().commit_sequence);
  HL_CHECK_EQ(replay.value().logical_time, first.value().logical_time);
  HL_CHECK_EQ(replay.value().revision_after, first.value().revision_after);
  HL_CHECK_EQ(compute_receipt_digest(replay.value()), first.value().receipt_digest);

  // A replay is not a second fact: no history entry, no revision, no state
  // change and no new applied plan.
  HL_CHECK(runtime.registry().state_digest() == digest_after_first);
  HL_CHECK_EQ(runtime.registry().history_entry_count(), history_after_first);
  HL_CHECK_EQ(runtime.registry().plan_count(), plans_after_first);
  const Result<ObjectView> view = runtime.inspect(key);
  HL_REQUIRE(view.has_value());
  HL_CHECK_EQ(view.value().object.revision, Revision::from_value(2));
  HL_CHECK_EQ(view.value().object.state, LifecycleState::Staged);
  HL_CHECK_EQ(view.value().object.history_entries, static_cast<std::uint64_t>(2));
  HL_CHECK_EQ(runtime.registry().commit_sequence(), CommitSequence::from_value(2));
}

HL_TEST(domain_transitions, reusing_a_plan_id_with_different_content_is_a_plan_conflict) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  const ObjectKey key = created.value().key;
  const Result<CreateReceipt> other = create_object_in(runtime, "asset-b");
  HL_REQUIRE(other.has_value());
  const ObjectKey other_key = other.value().key;

  const Edge edge{LifecycleState::Staged, TransitionReason::DeliveryAccepted, AuthorityScope::Logistics,
                  {EvidenceKind::DeliveryReceipt}, false};
  const Result<TransitionRequest> request = request_for_edge(runtime, key, edge);
  HL_REQUIRE(request.has_value());
  HL_REQUIRE(runtime.apply_transition(request.value()).has_value());
  const Digest settled = runtime.registry().state_digest();

  // Same plan identity, different content: a plan identity may not be reused for
  // a different fact.
  TransitionRequest conflicting = request.value();
  conflicting.provenance.evidence.push_back(EvidenceRef{EvidenceKind::StorageRecord, digest_of(0x77u),
                                                       "evidence/storage", ObservationSequence()});
  const Result<TransitionReceipt> conflict = runtime.apply_transition(conflicting);
  HL_CHECK_ERROR(conflict, ErrorCode::PlanConflict);
  HL_CHECK(runtime.registry().state_digest() == settled);

  // A different attempt of the same plan is a different fact and is accepted.
  const Result<TransitionRequest> built_fresh = request_for_edge(runtime, other_key, edge);
  HL_REQUIRE(built_fresh.has_value());
  TransitionRequest fresh = built_fresh.value();
  fresh.plan = request.value().plan;
  fresh.attempt = AttemptId::parse("attempt-2").value();
  fresh.provenance.plan = fresh.plan;
  fresh.provenance.attempt = fresh.attempt;
  const Result<TransitionReceipt> second = runtime.apply_transition(fresh);
  HL_REQUIRE(second.has_value());
  HL_CHECK_EQ(second.value().idempotent_replay, false);
  HL_CHECK_EQ(second.value().to, LifecycleState::Staged);
  // Two registrations and two transitions are four distinct plan identities:
  // the refused reuse never entered the ledger.
  HL_CHECK_EQ(runtime.registry().plan_count(), static_cast<std::size_t>(4));

  // A registration replays the same way when its plan identity is repeated.
  CreateRequest registration;
  registration.key.asset = AssetId::parse("asset-c").value();
  registration.key.hardware_generation = HardwareGeneration::first();
  registration.kind = HardwareKind::Compute;
  registration.model = ModelId::parse("model-x").value();
  registration.firmware_generation = FirmwareGeneration::first();
  registration.initial_state = LifecycleState::Ordered;
  registration.provenance =
      provenance_for(authority_bit(AuthorityScope::Procurement), {EvidenceKind::ProcurementRecord});
  registration.plan = registration.provenance.plan;
  registration.attempt = registration.provenance.attempt;
  const Result<CreateReceipt> registered = runtime.create_object(registration);
  HL_REQUIRE(registered.has_value());
  const Result<CreateReceipt> registration_replay = runtime.create_object(registration);
  HL_REQUIRE(registration_replay.has_value());
  HL_CHECK_EQ(registration_replay.value().idempotent_replay, true);
  HL_CHECK_EQ(registration_replay.value().receipt_digest, registered.value().receipt_digest);
  HL_CHECK_EQ(runtime.registry().object_count(), static_cast<std::size_t>(3));
}

// ---------------------------------------------------------------------------
// Deterministic rejection
// ---------------------------------------------------------------------------

HL_TEST(domain_transitions, every_rejected_case_reports_the_deterministic_primary_error) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  const ObjectKey key = created.value().key;
  const Result<CreateReceipt> waiting = create_object_in(runtime, "asset-b");
  HL_REQUIRE(waiting.has_value());
  const ObjectKey waiting_key = waiting.value().key;
  HL_REQUIRE(advance(runtime, waiting_key, path_to_commissioning()).has_value());

  const Edge legal{LifecycleState::Staged, TransitionReason::DeliveryAccepted, AuthorityScope::Logistics,
                   {EvidenceKind::DeliveryReceipt}, false};

  {
    const Result<TransitionRequest> request = request_for_edge(runtime, key, legal);
    HL_REQUIRE(request.has_value());
    TransitionRequest wrong_authority = request.value();
    wrong_authority.provenance.authority = authority_bit(AuthorityScope::Service);
    HL_CHECK_ERROR(runtime.apply_transition(wrong_authority), ErrorCode::InsufficientAuthority);
  }
  {
    const Result<TransitionRequest> request = request_for_edge(runtime, key, legal);
    HL_REQUIRE(request.has_value());
    TransitionRequest no_evidence = request.value();
    no_evidence.provenance.evidence.clear();
    HL_CHECK_ERROR(runtime.apply_transition(no_evidence), ErrorCode::MissingEvidence);
  }
  {
    const Result<TransitionRequest> request = request_for_edge(runtime, key, legal);
    HL_REQUIRE(request.has_value());
    TransitionRequest wrong_reason = request.value();
    wrong_reason.reason = TransitionReason::MaintenanceScheduled;
    HL_CHECK_ERROR(runtime.apply_transition(wrong_reason), ErrorCode::IllegalTransition);
  }
  {
    const Result<TransitionRequest> request = request_for_edge(runtime, key, legal);
    HL_REQUIRE(request.has_value());
    TransitionRequest stale_revision = request.value();
    const Result<Revision> advanced = stale_revision.expected_revision.next();
    HL_REQUIRE(advanced.has_value());
    stale_revision.expected_revision = advanced.value();
    HL_CHECK_ERROR(runtime.apply_transition(stale_revision), ErrorCode::StaleRevision);
  }
  {
    const Result<TransitionRequest> request = request_for_edge(runtime, key, legal);
    HL_REQUIRE(request.has_value());
    TransitionRequest stale_generation = request.value();
    const Result<LifecycleGeneration> advanced = stale_generation.expected_lifecycle_generation.next();
    HL_REQUIRE(advanced.has_value());
    stale_generation.expected_lifecycle_generation = advanced.value();
    HL_CHECK_ERROR(runtime.apply_transition(stale_generation), ErrorCode::StaleLifecycleGeneration);
  }
  {
    const Result<TransitionRequest> request = request_for_edge(runtime, key, legal);
    HL_REQUIRE(request.has_value());
    TransitionRequest stale_epoch = request.value();
    const Result<ControlEpoch> advanced = stale_epoch.expected_control_epoch.next();
    HL_REQUIRE(advanced.has_value());
    stale_epoch.expected_control_epoch = advanced.value();
    HL_CHECK_ERROR(runtime.apply_transition(stale_epoch), ErrorCode::StaleAuthority);
  }
  {
    const Result<TransitionRequest> request = request_for_edge(runtime, key, legal);
    HL_REQUIRE(request.has_value());
    TransitionRequest wrong_state = request.value();
    wrong_state.expected_state = LifecycleState::Staged;
    HL_CHECK_ERROR(runtime.apply_transition(wrong_state), ErrorCode::StateMismatch);
  }
  {
    // Ordered -> Active is not an edge of the table.
    const Result<TransitionRequest> request =
        request_for_edge(runtime, key, {LifecycleState::Active, TransitionReason::CommissioningPassed,
                                        AuthorityScope::Service, {EvidenceKind::CommissioningReport}, false});
    HL_REQUIRE(request.has_value());
    HL_CHECK_ERROR(runtime.apply_transition(request.value()), ErrorCode::IllegalTransition);
  }
  {
    // Entering the service scope with a closed (here: never decided) gate.
    const Result<TransitionRequest> request =
        request_for_edge(runtime, waiting_key, {LifecycleState::Active, TransitionReason::CommissioningPassed,
                                                AuthorityScope::Service, {EvidenceKind::CommissioningReport}, false});
    HL_REQUIRE(request.has_value());
    HL_CHECK_ERROR(runtime.apply_transition(request.value()), ErrorCode::InsufficientAuthority);
  }

  // Every rejection above left both objects exactly where they were.
  const Result<ObjectView> first = runtime.inspect(key);
  HL_REQUIRE(first.has_value());
  HL_CHECK_EQ(first.value().object.state, LifecycleState::Ordered);
  HL_CHECK_EQ(first.value().object.revision, Revision::first());
  const Result<ObjectView> second = runtime.inspect(waiting_key);
  HL_REQUIRE(second.has_value());
  HL_CHECK_EQ(second.value().object.state, LifecycleState::Commissioning);
}

HL_TEST(domain_transitions, the_primary_error_does_not_depend_on_insertion_order) {
  Result<Runtime> first_open = Runtime::open_ephemeral();
  Result<Runtime> second_open = Runtime::open_ephemeral();
  HL_REQUIRE(first_open.has_value());
  HL_REQUIRE(second_open.has_value());
  Runtime first = std::move(first_open.value());
  Runtime second = std::move(second_open.value());

  // The same three objects, inserted in opposite orders.
  const char* const forward[] = {"asset-a", "asset-b", "asset-c"};
  for (const char* asset : forward) {
    HL_REQUIRE(create_object_in(first, asset).has_value());
  }
  for (std::size_t index = 3; index > 0; --index) {
    HL_REQUIRE(create_object_in(second, forward[index - 1]).has_value());
  }
  HL_CHECK_EQ(first.registry().object_count(), static_cast<std::size_t>(3));
  HL_CHECK_EQ(second.registry().object_count(), static_cast<std::size_t>(3));

  ObjectKey key;
  key.asset = AssetId::parse("asset-b").value();
  key.hardware_generation = HardwareGeneration::first();
  HL_REQUIRE(first.registry().find(key) != nullptr);

  const Edge legal{LifecycleState::Staged, TransitionReason::DeliveryAccepted, AuthorityScope::Logistics,
                   {EvidenceKind::DeliveryReceipt}, false};

  // The same invalid request against both registries: the reported code is a
  // function of the request and the state, never of insertion order.
  {
    const Result<TransitionRequest> built = request_for_edge(first, key, legal);
    HL_REQUIRE(built.has_value());
    TransitionRequest wrong_authority = built.value();
    wrong_authority.provenance.authority = authority_bit(AuthorityScope::Retirement);
    const Result<TransitionReceipt> left = first.apply_transition(wrong_authority);
    const Result<TransitionReceipt> right = second.apply_transition(wrong_authority);
    HL_CHECK_ERROR(left, ErrorCode::InsufficientAuthority);
    HL_CHECK_ERROR(right, ErrorCode::InsufficientAuthority);
    HL_CHECK_EQ(left.error().code, right.error().code);
  }
  {
    const Result<TransitionRequest> built = request_for_edge(first, key, legal);
    HL_REQUIRE(built.has_value());
    TransitionRequest no_evidence = built.value();
    no_evidence.provenance.evidence.clear();
    const Result<TransitionReceipt> left = first.apply_transition(no_evidence);
    const Result<TransitionReceipt> right = second.apply_transition(no_evidence);
    HL_CHECK_ERROR(left, ErrorCode::MissingEvidence);
    HL_CHECK_ERROR(right, ErrorCode::MissingEvidence);
    HL_CHECK_EQ(left.error().code, right.error().code);
  }
  {
    const Result<TransitionRequest> built =
        request_for_edge(first, key, {LifecycleState::Maintenance, TransitionReason::MaintenanceScheduled,
                                      AuthorityScope::Maintenance, {EvidenceKind::MaintenanceRecord}, false});
    HL_REQUIRE(built.has_value());
    const Result<TransitionReceipt> left = first.apply_transition(built.value());
    const Result<TransitionReceipt> right = second.apply_transition(built.value());
    HL_CHECK_ERROR(left, ErrorCode::IllegalTransition);
    HL_CHECK_ERROR(right, ErrorCode::IllegalTransition);
    HL_CHECK_EQ(left.error().code, right.error().code);
  }

  // The legal request succeeds in both, and both land on the same receipt shape.
  const Result<TransitionRequest> built = request_for_edge(first, key, legal);
  HL_REQUIRE(built.has_value());
  const Result<TransitionReceipt> left = first.apply_transition(built.value());
  const Result<TransitionReceipt> right = second.apply_transition(built.value());
  HL_REQUIRE(left.has_value());
  HL_REQUIRE(right.has_value());
  HL_CHECK_EQ(left.value().to, right.value().to);
  HL_CHECK_EQ(left.value().revision_after, right.value().revision_after);
  HL_CHECK_EQ(left.value().lifecycle_generation, right.value().lifecycle_generation);
  HL_CHECK_EQ(left.value().commit_sequence, right.value().commit_sequence);
}

HL_TEST(domain_transitions, a_rejected_request_leaves_the_state_digest_unchanged) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  const ObjectKey key = created.value().key;
  const Result<CreateReceipt> successor = create_object_in(runtime, "asset-b");
  HL_REQUIRE(successor.has_value());
  const ObjectKey successor_key = successor.value().key;
  HL_REQUIRE(advance(runtime, successor_key, path_to_installed()).has_value());
  HL_REQUIRE(advance(runtime, key, path_to_retired()).has_value());

  const Digest settled = runtime.registry().state_digest();
  const std::size_t history = runtime.registry().history_entry_count();
  const std::size_t plans = runtime.registry().plan_count();

  // A request that fails validation.
  const Result<TransitionRequest> invalid =
      request_for_edge(runtime, key, {LifecycleState::Removed, TransitionReason::PhysicallyRemoved,
                                      AuthorityScope::Decommissioning, {EvidenceKind::RemovalRecord}, false});
  HL_REQUIRE(invalid.has_value());
  TransitionRequest wrong_authority = invalid.value();
  wrong_authority.provenance.authority = authority_bit(AuthorityScope::Procurement);
  const Result<TransitionReceipt> refused = runtime.apply_transition(wrong_authority);
  HL_CHECK_ERROR(refused, ErrorCode::InsufficientAuthority);
  HL_CHECK(runtime.registry().state_digest() == settled);
  HL_CHECK_EQ(runtime.registry().history_entry_count(), history);
  HL_CHECK_EQ(runtime.registry().plan_count(), plans);

  // A request that fails late, after the successor rule has been examined: the
  // replacement link was never recorded, so no receipt may be issued.
  const Result<TransitionRequest> unlinked =
      request_for_edge(runtime, key, {LifecycleState::Replaced, TransitionReason::SuccessorLinked,
                                      AuthorityScope::Replacement,
                                      {EvidenceKind::ReplacementAuthorization, EvidenceKind::SuccessorRecord}, false});
  HL_REQUIRE(unlinked.has_value());
  TransitionRequest with_successor = unlinked.value();
  with_successor.successor = successor_key;
  const Result<TransitionReceipt> late = runtime.apply_transition(with_successor);
  HL_CHECK_ERROR(late, ErrorCode::MissingSuccessor);
  HL_CHECK(runtime.registry().state_digest() == settled);
  HL_CHECK_EQ(runtime.registry().history_entry_count(), history);
  HL_CHECK_EQ(runtime.registry().plan_count(), plans);

  const Result<ObjectView> view = runtime.inspect(key);
  HL_REQUIRE(view.has_value());
  HL_CHECK_EQ(view.value().object.state, LifecycleState::Retired);
  HL_CHECK_EQ(view.value().object.successor.has_value(), false);
  HL_CHECK(runtime.registry().verify_invariants().has_value());
}

// ---------------------------------------------------------------------------
// State boundaries
// ---------------------------------------------------------------------------

HL_TEST(domain_transitions, maintenance_is_not_removal) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  const ObjectKey key = created.value().key;
  HL_REQUIRE(advance(runtime, key, path_to_maintenance()).has_value());

  const Result<ObjectView> view = runtime.inspect(key);
  HL_REQUIRE(view.has_value());
  HL_CHECK_EQ(view.value().object.state, LifecycleState::Maintenance);
  HL_CHECK_EQ(view.value().object.eligibility_gate, EligibilityGate::Closed);
  HL_CHECK_EQ(occupies_service_scope(LifecycleState::Maintenance), false);
  HL_CHECK_EQ(is_decommissioned(LifecycleState::Maintenance), false);
  HL_CHECK_EQ(is_superseded(LifecycleState::Maintenance), false);
  HL_CHECK_EQ(is_terminal(LifecycleState::Maintenance), false);

  // Maintenance exists to return to service: removal and retirement are not
  // edges of the table, so they are refused by name.
  HL_CHECK_EQ(find_rule(LifecycleState::Maintenance, LifecycleState::Removed), nullptr);
  HL_CHECK_EQ(find_rule(LifecycleState::Maintenance, LifecycleState::Retired), nullptr);
  HL_CHECK_EQ(find_rule(LifecycleState::Maintenance, LifecycleState::Replaced), nullptr);

  const Digest settled = runtime.registry().state_digest();
  const Result<TransitionRequest> removal =
      request_for_edge(runtime, key, {LifecycleState::Removed, TransitionReason::PhysicallyRemoved,
                                      AuthorityScope::Decommissioning, {EvidenceKind::RemovalRecord}, false});
  HL_REQUIRE(removal.has_value());
  HL_CHECK_ERROR(runtime.apply_transition(removal.value()), ErrorCode::IllegalTransition);

  const Result<ObjectView> after = runtime.inspect(key);
  HL_REQUIRE(after.has_value());
  HL_CHECK_EQ(after.value().object.state, LifecycleState::Maintenance);
  HL_CHECK(runtime.registry().state_digest() == settled);
}

HL_TEST(domain_transitions, degraded_is_not_retired) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  const ObjectKey key = created.value().key;
  HL_REQUIRE(advance(runtime, key, path_to_active()).has_value());

  // An acknowledged impairment moves the object inside the service scope; it is
  // neither a retirement nor a removal, and it leaves the gate alone.
  const Result<TransitionReceipt> degraded =
      apply_edge(runtime, key, {LifecycleState::Degraded, TransitionReason::ServiceImpairmentAcknowledged,
                                AuthorityScope::Service, {EvidenceKind::HealthEvidence}, false});
  HL_REQUIRE(degraded.has_value());
  HL_CHECK_EQ(degraded.value().to, LifecycleState::Degraded);
  HL_CHECK_EQ(degraded.value().gate_after, EligibilityGate::Open);
  HL_CHECK_EQ(degraded.value().lifecycle_generation, LifecycleGeneration::from_value(6));

  const Result<ObjectView> view = runtime.inspect(key);
  HL_REQUIRE(view.has_value());
  HL_CHECK_EQ(view.value().object.state, LifecycleState::Degraded);
  HL_CHECK_EQ(occupies_service_scope(LifecycleState::Degraded), true);
  HL_CHECK_EQ(is_decommissioned(LifecycleState::Degraded), false);
  HL_CHECK_EQ(is_terminal(LifecycleState::Degraded), false);
  HL_CHECK_EQ(service_eligibility(view.value().object.state, view.value().object.eligibility_gate),
              ServiceEligibility::Eligible);

  // There is no edge from Degraded to Retired or Removed.
  HL_CHECK_EQ(find_rule(LifecycleState::Degraded, LifecycleState::Retired), nullptr);
  HL_CHECK_EQ(find_rule(LifecycleState::Degraded, LifecycleState::Removed), nullptr);
  HL_CHECK_EQ(find_rule(LifecycleState::Degraded, LifecycleState::Replaced), nullptr);
  for (const LifecycleState target : legal_targets(LifecycleState::Degraded)) {
    HL_CHECK(target != LifecycleState::Retired);
    HL_CHECK(target != LifecycleState::Removed);
    HL_CHECK(target != LifecycleState::Replaced);
  }

  const Result<TransitionRequest> retire =
      request_for_edge(runtime, key, {LifecycleState::Retired, TransitionReason::DecommissionCompleted,
                                      AuthorityScope::Decommissioning, {EvidenceKind::DecommissioningRecord}, false});
  HL_REQUIRE(retire.has_value());
  HL_CHECK_ERROR(runtime.apply_transition(retire.value()), ErrorCode::IllegalTransition);

  // Service returns, and the object is fully active again.
  const Result<TransitionReceipt> restored =
      apply_edge(runtime, key, {LifecycleState::Active, TransitionReason::ServiceRestored, AuthorityScope::Service,
                                {EvidenceKind::ServiceRecord, EvidenceKind::HealthEvidence}, false});
  HL_REQUIRE(restored.has_value());
  HL_CHECK_EQ(restored.value().to, LifecycleState::Active);
  HL_CHECK_EQ(restored.value().gate_after, EligibilityGate::Open);
  HL_CHECK_EQ(restored.value().revision_after, Revision::from_value(8));
  HL_CHECK(runtime.registry().verify_invariants().has_value());
}

HL_TEST(domain_transitions, retired_and_removed_refuse_a_further_transition) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  const ObjectKey key = created.value().key;
  HL_REQUIRE(advance(runtime, key, path_to_removed()).has_value());

  const Result<ObjectView> view = runtime.inspect(key);
  HL_REQUIRE(view.has_value());
  HL_CHECK_EQ(view.value().object.state, LifecycleState::Removed);
  HL_CHECK_EQ(is_terminal(LifecycleState::Removed), true);
  HL_CHECK_EQ(is_decommissioned(LifecycleState::Removed), true);
  HL_CHECK_EQ(legal_targets(LifecycleState::Removed).empty(), true);

  const Digest settled = runtime.registry().state_digest();
  const std::size_t plans = runtime.registry().plan_count();
  const char* const targets[] = {"Ordered", "Staged", "Installed", "Active", "Retired", "Removed"};
  for (const char* target : targets) {
    const Result<LifecycleState> parsed = parse_lifecycle_state(target);
    HL_REQUIRE(parsed.has_value());
    const Result<TransitionRequest> request =
        request_for_edge(runtime, key, {parsed.value(), TransitionReason::PhysicallyRemoved,
                                        AuthorityScope::Decommissioning, {EvidenceKind::RemovalRecord}, false});
    HL_REQUIRE(request.has_value());
    HL_CHECK_ERROR(runtime.apply_transition(request.value()), ErrorCode::IllegalTransition);
  }

  const Result<ObjectView> after = runtime.inspect(key);
  HL_REQUIRE(after.has_value());
  HL_CHECK_EQ(after.value().object.state, LifecycleState::Removed);
  HL_CHECK(runtime.registry().state_digest() == settled);
  HL_CHECK_EQ(runtime.registry().plan_count(), plans);
}