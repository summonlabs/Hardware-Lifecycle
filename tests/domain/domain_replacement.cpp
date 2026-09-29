// Hardware Lifecycle - domain proofs for replacement lineage.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A replacement links two distinct object keys. The predecessor keeps its
// identity, its history and its physical presence; only the successor reference
// and the superseded lifecycle state are added. Self replacement, cycles and
// duplicate links are refused, and the successor must already be installed
// before it may replace anything.

#include <array>
#include <cstddef>
#include <cstdint>
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

[[nodiscard]] Provenance provenance_for(AuthorityMask authority, const std::vector<EvidenceKind>& kinds) {
  Provenance value;
  value.actor.id = ActorId::parse("operator-1").value();
  value.actor.kind = ActorKind::Operator;
  value.authority = authority;
  value.policy_generation = PolicyGeneration::first();
  value.plan = PlanId::parse("plan-" + std::to_string(++plan_counter)).value();
  value.attempt = AttemptId::parse("attempt-1").value();
  for (std::size_t index = 0; index < kinds.size(); ++index) {
    EvidenceRef evidence;
    evidence.kind = kinds[index];
    evidence.digest = digest_of(static_cast<std::uint8_t>(0x30u + index));
    evidence.source = std::string("evidence/") + std::string(to_string(kinds[index]));
    value.evidence.push_back(std::move(evidence));
  }
  return value;
}

[[nodiscard]] ObjectKey key_of(const char* asset, HardwareGeneration generation = HardwareGeneration::first()) {
  ObjectKey key;
  key.asset = AssetId::parse(asset).value();
  key.hardware_generation = generation;
  return key;
}

[[nodiscard]] Location location_of(const char* slot) {
  Location location;
  location.site = SiteId::parse("site-a").value();
  location.rack = RackId::parse("rack-1").value();
  location.slot = SlotId::parse(slot).value();
  return location;
}

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
  request.key = key_of(asset);
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

[[nodiscard]] Result<TransitionReceipt> apply_edge(Runtime& runtime, const ObjectKey& key, const Edge& edge) {
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
  return runtime.apply_transition(request);
}

[[nodiscard]] Result<TransitionReceipt> apply_successor_edge(Runtime& runtime, const ObjectKey& key,
                                                             const ObjectKey& successor) {
  const Result<ObjectView> view = runtime.inspect(key);
  if (!view.has_value()) {
    return view.error();
  }
  TransitionRequest request;
  request.key = key;
  request.expected_state = view.value().object.state;
  request.target_state = LifecycleState::Replaced;
  request.expected_revision = view.value().object.revision;
  request.expected_lifecycle_generation = view.value().object.lifecycle_generation;
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.reason = TransitionReason::SuccessorLinked;
  request.successor = successor;
  request.provenance = provenance_for(authority_bit(AuthorityScope::Replacement),
                                      {EvidenceKind::ReplacementAuthorization, EvidenceKind::SuccessorRecord});
  request.plan = request.provenance.plan;
  request.attempt = request.provenance.attempt;
  return runtime.apply_transition(request);
}

[[nodiscard]] Result<ReplacementReceipt> link(Runtime& runtime, const ObjectKey& predecessor,
                                              const ObjectKey& successor) {
  const Result<ObjectView> view = runtime.inspect(predecessor);
  if (!view.has_value()) {
    return view.error();
  }
  ReplacementRequest request;
  request.predecessor = predecessor;
  request.successor = successor;
  request.expected_revision = view.value().object.revision;
  request.expected_lifecycle_generation = view.value().object.lifecycle_generation;
  request.expected_control_epoch = runtime.registry().control_epoch();
  request.provenance = provenance_for(authority_bit(AuthorityScope::Replacement),
                                      {EvidenceKind::ReplacementAuthorization, EvidenceKind::SuccessorRecord});
  request.plan = request.provenance.plan;
  request.attempt = request.provenance.attempt;
  return runtime.link_replacement(request);
}

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

/// Ordered to Retired through quarantine: no service is ever carried, so the
/// walk needs no gate decision.
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

[[nodiscard]] std::vector<Edge> path_to_maintenance() {
  std::vector<Edge> path = path_to_active();
  path.push_back({LifecycleState::Maintenance, TransitionReason::MaintenanceScheduled, AuthorityScope::Maintenance,
                  {EvidenceKind::MaintenanceRecord}, false});
  return path;
}

/// One durable link record, built the way the runtime builds it.
[[nodiscard]] ReplacementRecord make_link_record(const ObjectKey& predecessor, const ObjectKey& successor,
                                                 std::uint64_t sequence) {
  ReplacementRecord record;
  record.predecessor = predecessor;
  record.successor = successor;
  record.reason = TransitionReason::SuccessorLinked;
  record.provenance = provenance_for(authority_bit(AuthorityScope::Replacement),
                                     {EvidenceKind::ReplacementAuthorization, EvidenceKind::SuccessorRecord});
  record.linked_at = LogicalTime::from_value(sequence);
  record.commit_sequence = CommitSequence::from_value(sequence);
  record.replacement_generation = ReplacementGeneration::first();
  record.link_digest = compute_link_digest(record);
  return record;
}

}  // namespace

// ---------------------------------------------------------------------------
// Link and supersede
// ---------------------------------------------------------------------------

HL_TEST(domain_replacement, link_moves_the_predecessor_to_replaced_and_keeps_its_identity) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> predecessor = create_object_in(runtime, "asset-a");
  HL_REQUIRE(predecessor.has_value());
  const ObjectKey predecessor_key = predecessor.value().key;
  HL_REQUIRE(advance(runtime, predecessor_key, path_to_retired()).has_value());

  const Result<CreateReceipt> successor = create_object_in(runtime, "asset-b");
  HL_REQUIRE(successor.has_value());
  const ObjectKey successor_key = successor.value().key;
  HL_REQUIRE(advance(runtime, successor_key, path_to_installed()).has_value());

  const Result<ObjectView> before = runtime.inspect(predecessor_key);
  HL_REQUIRE(before.has_value());
  const std::uint64_t history_before = before.value().object.history_entries;
  const std::uint64_t generation_before = before.value().object.lifecycle_generation.value();
  const Result<HistoryView> history_before_view = runtime.history(predecessor_key);
  HL_REQUIRE(history_before_view.has_value());
  std::vector<Digest> earlier_chain;
  for (const HistoryEntry& entry : history_before_view.value().entries) {
    earlier_chain.push_back(entry.chain_digest);
  }
  HL_CHECK_EQ(before.value().object.state, LifecycleState::Retired);
  HL_CHECK_EQ(before.value().object.location.has_value(), true);
  HL_CHECK_EQ(before.value().object.successor.has_value(), false);
  HL_CHECK_EQ(before.value().object.predecessor.has_value(), false);
  HL_CHECK_EQ(runtime.all_links().value().size(), static_cast<std::size_t>(0));

  const Result<ReplacementReceipt> linked = link(runtime, predecessor_key, successor_key);
  HL_REQUIRE(linked.has_value());
  HL_CHECK_EQ(linked.value().predecessor, predecessor_key);
  HL_CHECK_EQ(linked.value().successor, successor_key);
  HL_CHECK_EQ(linked.value().replacement_generation, ReplacementGeneration::first());
  HL_CHECK_EQ(linked.value().idempotent_replay, false);
  HL_CHECK_EQ(linked.value().commit_sequence, runtime.registry().commit_sequence());
  HL_CHECK_EQ(compute_receipt_digest(linked.value()), linked.value().receipt_digest);
  HL_CHECK(!linked.value().link_digest.is_zero());
  {
    const Result<std::vector<ReplacementRecord>> records = runtime.all_links();
    HL_REQUIRE(records.has_value());
    HL_REQUIRE(records.value().size() == 1u);
    HL_CHECK_EQ(records.value()[0].predecessor, predecessor_key);
    HL_CHECK_EQ(records.value()[0].successor, successor_key);
    HL_CHECK_EQ(records.value()[0].link_digest, linked.value().link_digest);
    HL_CHECK_EQ(compute_link_digest(records.value()[0]), records.value()[0].link_digest);
  }

  // Linking is not a lifecycle transition: the predecessor is still Retired and
  // its chain has not been touched.
  const Result<ObjectView> linked_view = runtime.inspect(predecessor_key);
  HL_REQUIRE(linked_view.has_value());
  HL_CHECK_EQ(linked_view.value().object.state, LifecycleState::Retired);
  HL_CHECK_EQ(linked_view.value().object.successor.has_value(), true);
  HL_CHECK_EQ(linked_view.value().object.successor.value(), successor_key);
  HL_CHECK_EQ(linked_view.value().object.replacement_generation, ReplacementGeneration::first());
  HL_CHECK_EQ(linked_view.value().object.history_entries, history_before);
  {
    const Result<ObjectView> successor_view = runtime.inspect(successor_key);
    HL_REQUIRE(successor_view.has_value());
    HL_CHECK_EQ(successor_view.value().object.predecessor.has_value(), true);
    HL_CHECK_EQ(successor_view.value().object.predecessor.value(), predecessor_key);
    HL_CHECK_EQ(successor_view.value().object.state, LifecycleState::Installed);
  }

  const Result<TransitionReceipt> replaced = apply_successor_edge(runtime, predecessor_key, successor_key);
  HL_REQUIRE(replaced.has_value());
  HL_CHECK_EQ(replaced.value().from, LifecycleState::Retired);
  HL_CHECK_EQ(replaced.value().to, LifecycleState::Replaced);
  HL_CHECK_EQ(replaced.value().reason, TransitionReason::SuccessorLinked);

  const Result<ObjectView> after = runtime.inspect(predecessor_key);
  HL_REQUIRE(after.has_value());
  // Identity is untouched: same key, same asset, same hardware generation.
  HL_CHECK_EQ(after.value().object.key, predecessor_key);
  HL_CHECK_EQ(after.value().object.key.asset.value(), std::string("asset-a"));
  HL_CHECK_EQ(after.value().object.key.hardware_generation, HardwareGeneration::first());
  HL_CHECK_EQ(after.value().object.state, LifecycleState::Replaced);
  HL_CHECK_EQ(is_superseded(LifecycleState::Replaced), true);
  HL_CHECK_EQ(is_decommissioned(LifecycleState::Replaced), true);
  HL_CHECK_EQ(is_terminal(LifecycleState::Replaced), false);
  HL_CHECK_EQ(is_physical(LifecycleState::Replaced), true);
  // Physical presence is kept: the unit is superseded, not gone.
  HL_CHECK_EQ(after.value().object.location.has_value(), true);
  HL_CHECK_EQ(after.value().object.successor.has_value(), true);
  HL_CHECK_EQ(after.value().object.successor.value(), successor_key);
  // History grows by exactly the entry that records the supersession, and the
  // earlier entries are untouched.
  HL_CHECK_EQ(after.value().object.history_entries, history_before + 1u);
  HL_CHECK_EQ(after.value().object.lifecycle_generation.value(), generation_before + 1u);
  {
    const Result<HistoryView> history_after = runtime.history(predecessor_key);
    HL_REQUIRE(history_after.has_value());
    HL_CHECK_EQ(history_after.value().entries.size(), static_cast<std::size_t>(history_before + 1u));
    HL_REQUIRE(history_after.value().entries.size() >= earlier_chain.size());
    for (std::size_t index = 0; index < earlier_chain.size(); ++index) {
      HL_CHECK_EQ(history_after.value().entries[index].chain_digest, earlier_chain[index]);
    }
    HL_CHECK_EQ(history_after.value().entries.back().to, LifecycleState::Replaced);
    HistoryLog log;
    log.entries = history_after.value().entries;
    log.chain_head = history_after.value().chain_head;
    HL_CHECK(verify_history_chain(log).has_value());
    HL_CHECK(replay_history(log).has_value());
    HL_CHECK_EQ(replay_history(log).value(), LifecycleState::Replaced);
  }
  HL_CHECK(runtime.registry().verify_invariants().has_value());
  HL_CHECK(runtime.verify().has_value());
}

HL_TEST(domain_replacement, the_transition_to_replaced_requires_the_link_first) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> predecessor = create_object_in(runtime, "asset-a");
  HL_REQUIRE(predecessor.has_value());
  const ObjectKey predecessor_key = predecessor.value().key;
  HL_REQUIRE(advance(runtime, predecessor_key, path_to_retired()).has_value());
  const Result<CreateReceipt> successor = create_object_in(runtime, "asset-b");
  HL_REQUIRE(successor.has_value());
  const ObjectKey successor_key = successor.value().key;
  HL_REQUIRE(advance(runtime, successor_key, path_to_installed()).has_value());

  const Digest settled = runtime.registry().state_digest();
  const Result<TransitionReceipt> refused = apply_successor_edge(runtime, predecessor_key, successor_key);
  HL_CHECK_ERROR(refused, ErrorCode::MissingSuccessor);
  HL_CHECK(runtime.registry().state_digest() == settled);
  {
    const Result<ObjectView> view = runtime.inspect(predecessor_key);
    HL_REQUIRE(view.has_value());
    HL_CHECK_EQ(view.value().object.state, LifecycleState::Retired);
    HL_CHECK_EQ(view.value().object.successor.has_value(), false);
  }

  // Recording the link is the only missing fact, so the same request succeeds
  // once it exists.
  const Result<ReplacementReceipt> linked = link(runtime, predecessor_key, successor_key);
  HL_REQUIRE(linked.has_value());
  const Result<TransitionReceipt> replaced = apply_successor_edge(runtime, predecessor_key, successor_key);
  HL_REQUIRE(replaced.has_value());
  HL_CHECK_EQ(replaced.value().to, LifecycleState::Replaced);
}

HL_TEST(domain_replacement, self_replacement_is_refused) {
  const Result<void> pair = validate_replacement_pair(key_of("asset-a"), key_of("asset-a"));
  HL_CHECK_ERROR(pair, ErrorCode::SelfReplacement);

  Registry graph;
  const Result<void> added = graph.lineage().add(make_link_record(key_of("asset-a"), key_of("asset-a"), 1));
  HL_CHECK_ERROR(added, ErrorCode::SelfReplacement);

  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());
  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  HL_REQUIRE(advance(runtime, created.value().key, path_to_retired()).has_value());

  const Digest settled = runtime.registry().state_digest();
  const Result<ReplacementReceipt> linked = link(runtime, created.value().key, created.value().key);
  HL_CHECK_ERROR(linked, ErrorCode::SelfReplacement);
  HL_CHECK(runtime.registry().state_digest() == settled);
  HL_CHECK_EQ(runtime.all_links().value().size(), static_cast<std::size_t>(0));
}

HL_TEST(domain_replacement, cycles_are_refused) {
  // The forest refuses a link that would close a cycle, whichever direction the
  // link is added from.
  Registry graph;
  const Result<void> forward = graph.lineage().add(make_link_record(key_of("asset-a"), key_of("asset-b"), 1));
  HL_REQUIRE(forward.has_value());
  HL_CHECK_EQ(graph.lineage().size(), static_cast<std::size_t>(1));
  const Result<void> backward = graph.lineage().add(make_link_record(key_of("asset-b"), key_of("asset-a"), 2));
  HL_CHECK_ERROR(backward, ErrorCode::LineageCycle);
  HL_CHECK_EQ(graph.lineage().size(), static_cast<std::size_t>(1));

  // The same boundary through the runtime, where the predecessor's lifecycle
  // state has to allow a link first.
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());
  const Result<CreateReceipt> first = create_object_in(runtime, "asset-a");
  HL_REQUIRE(first.has_value());
  const Result<CreateReceipt> second = create_object_in(runtime, "asset-b");
  HL_REQUIRE(second.has_value());
  HL_REQUIRE(advance(runtime, first.value().key, path_to_retired()).has_value());
  HL_REQUIRE(advance(runtime, second.value().key, path_to_retired()).has_value());

  const Result<ReplacementReceipt> linked = link(runtime, first.value().key, second.value().key);
  HL_REQUIRE(linked.has_value());
  const Digest settled = runtime.registry().state_digest();
  const Result<ReplacementReceipt> closing = link(runtime, second.value().key, first.value().key);
  HL_CHECK_ERROR(closing, ErrorCode::LineageCycle);
  HL_CHECK(runtime.registry().state_digest() == settled);
  HL_CHECK_EQ(runtime.all_links().value().size(), static_cast<std::size_t>(1));
  HL_CHECK(runtime.registry().verify_invariants().has_value());
}

HL_TEST(domain_replacement, duplicate_links_are_refused) {
  Registry graph;
  const ReplacementRecord record = make_link_record(key_of("asset-a"), key_of("asset-b"), 1);
  HL_REQUIRE(graph.lineage().add(record).has_value());
  HL_CHECK_ERROR(graph.lineage().add(record), ErrorCode::ObjectExists);

  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());
  const Result<CreateReceipt> first = create_object_in(runtime, "asset-a");
  HL_REQUIRE(first.has_value());
  const Result<CreateReceipt> second = create_object_in(runtime, "asset-b");
  HL_REQUIRE(second.has_value());
  HL_REQUIRE(advance(runtime, first.value().key, path_to_retired()).has_value());
  HL_REQUIRE(advance(runtime, second.value().key, path_to_installed()).has_value());

  const Result<ReplacementReceipt> linked = link(runtime, first.value().key, second.value().key);
  HL_REQUIRE(linked.has_value());
  const Digest settled = runtime.registry().state_digest();
  const Result<ReplacementReceipt> again = link(runtime, first.value().key, second.value().key);
  HL_CHECK_ERROR(again, ErrorCode::ObjectExists);
  HL_CHECK(runtime.registry().state_digest() == settled);
  HL_CHECK_EQ(runtime.all_links().value().size(), static_cast<std::size_t>(1));
}

HL_TEST(domain_replacement, the_successor_must_already_be_installed) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> predecessor = create_object_in(runtime, "asset-a");
  HL_REQUIRE(predecessor.has_value());
  const ObjectKey predecessor_key = predecessor.value().key;
  HL_REQUIRE(advance(runtime, predecessor_key, path_to_retired()).has_value());

  const Result<CreateReceipt> waiting = create_object_in(runtime, "asset-b", LifecycleState::Staged);
  HL_REQUIRE(waiting.has_value());
  const Result<CreateReceipt> ordered = create_object_in(runtime, "asset-c");
  HL_REQUIRE(ordered.has_value());
  HL_CHECK_ERROR(link(runtime, predecessor_key, waiting.value().key), ErrorCode::InvalidSuccessor);
  HL_CHECK_ERROR(link(runtime, predecessor_key, ordered.value().key), ErrorCode::InvalidSuccessor);
  HL_CHECK_EQ(runtime.all_links().value().size(), static_cast<std::size_t>(0));

  // Installed or further along is enough.
  const Result<CreateReceipt> ready = create_object_in(runtime, "asset-d");
  HL_REQUIRE(ready.has_value());
  HL_REQUIRE(advance(runtime, ready.value().key, path_to_installed()).has_value());
  const Result<ReplacementReceipt> linked = link(runtime, predecessor_key, ready.value().key);
  HL_REQUIRE(linked.has_value());
  HL_CHECK_EQ(runtime.all_links().value().size(), static_cast<std::size_t>(1));
}

HL_TEST(domain_replacement, the_link_is_refused_while_the_predecessor_is_in_service_or_maintenance) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> successor = create_object_in(runtime, "asset-z");
  HL_REQUIRE(successor.has_value());
  HL_REQUIRE(advance(runtime, successor.value().key, path_to_installed()).has_value());

  const Result<CreateReceipt> active = create_object_in(runtime, "asset-a");
  HL_REQUIRE(active.has_value());
  HL_REQUIRE(advance(runtime, active.value().key, path_to_active()).has_value());
  HL_CHECK_ERROR(link(runtime, active.value().key, successor.value().key), ErrorCode::IllegalTransition);

  const Result<CreateReceipt> maintained = create_object_in(runtime, "asset-b");
  HL_REQUIRE(maintained.has_value());
  HL_REQUIRE(advance(runtime, maintained.value().key, path_to_maintenance()).has_value());
  HL_CHECK_ERROR(link(runtime, maintained.value().key, successor.value().key), ErrorCode::IllegalTransition);

  // Neither refusal recorded anything.
  HL_CHECK_EQ(runtime.all_links().value().size(), static_cast<std::size_t>(0));
  const Result<ObjectView> view = runtime.inspect(active.value().key);
  HL_REQUIRE(view.has_value());
  HL_CHECK_EQ(view.value().object.successor.has_value(), false);
  HL_CHECK_EQ(view.value().object.state, LifecycleState::Active);
  HL_CHECK(runtime.registry().verify_invariants().has_value());
}

HL_TEST(domain_replacement, all_links_is_ordered_and_every_link_digest_is_stable) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> first = create_object_in(runtime, "asset-a");
  HL_REQUIRE(first.has_value());
  const Result<CreateReceipt> second = create_object_in(runtime, "asset-b");
  HL_REQUIRE(second.has_value());
  const Result<CreateReceipt> third = create_object_in(runtime, "asset-c");
  HL_REQUIRE(third.has_value());
  HL_REQUIRE(advance(runtime, first.value().key, path_to_retired()).has_value());
  HL_REQUIRE(advance(runtime, second.value().key, path_to_retired()).has_value());
  HL_REQUIRE(advance(runtime, third.value().key, path_to_retired()).has_value());

  const Result<ReplacementReceipt> first_link = link(runtime, first.value().key, second.value().key);
  HL_REQUIRE(first_link.has_value());
  const Result<ReplacementReceipt> second_link = link(runtime, second.value().key, third.value().key);
  HL_REQUIRE(second_link.has_value());

  const Result<std::vector<ReplacementRecord>> records = runtime.all_links();
  HL_REQUIRE(records.has_value());
  HL_REQUIRE(records.value().size() == 2u);
  HL_CHECK(records.value()[0].predecessor < records.value()[1].predecessor);
  HL_CHECK_EQ(records.value()[0].predecessor, first.value().key);
  HL_CHECK_EQ(records.value()[1].predecessor, second.value().key);
  HL_CHECK_EQ(records.value()[1].successor, third.value().key);
  for (const ReplacementRecord& record : records.value()) {
    HL_CHECK_EQ(record.reason, TransitionReason::SuccessorLinked);
    HL_CHECK_EQ(compute_link_digest(record), record.link_digest);
    HL_CHECK_EQ(compute_link_digest(record), compute_link_digest(record));
    HL_CHECK(!record.link_digest.is_zero());
  }
  HL_CHECK_EQ(records.value()[0].link_digest, first_link.value().link_digest);
  HL_CHECK_EQ(records.value()[1].link_digest, second_link.value().link_digest);
  HL_CHECK(!(first_link.value().link_digest == second_link.value().link_digest));

  // The chain is walked in link order from either end.
  const Result<LineageView> from_first = runtime.lineage(first.value().key);
  HL_REQUIRE(from_first.has_value());
  HL_REQUIRE(from_first.value().successors.size() == 2u);
  HL_CHECK_EQ(from_first.value().successors[0], second.value().key);
  HL_CHECK_EQ(from_first.value().successors[1], third.value().key);
  HL_CHECK_EQ(from_first.value().links.size(), static_cast<std::size_t>(2));
  const Result<LineageView> from_last = runtime.lineage(third.value().key);
  HL_REQUIRE(from_last.has_value());
  HL_REQUIRE(from_last.value().ancestors.size() == 2u);
  HL_CHECK_EQ(from_last.value().ancestors[0], first.value().key);
  HL_CHECK_EQ(from_last.value().ancestors[1], second.value().key);
  HL_CHECK(runtime.registry().verify_invariants().has_value());
}
