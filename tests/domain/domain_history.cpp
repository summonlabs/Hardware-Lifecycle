// Hardware Lifecycle - domain proofs for the durable lifecycle history chain.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every entry is bound to its predecessor by a chain digest, so a rewritten,
// reordered, dropped or duplicated entry is detected without trusting the
// storage layer, and replaying a chain must land exactly on the authoritative
// object. Nothing here repairs, reorders or fills in an entry.

#include <algorithm>
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
    evidence.digest = digest_of(static_cast<std::uint8_t>(0x40u + index));
    evidence.source = std::string("evidence/") + std::string(to_string(kinds[index]));
    value.evidence.push_back(std::move(evidence));
  }
  return value;
}

[[nodiscard]] ObjectKey key_of(const char* asset) {
  ObjectKey key;
  key.asset = AssetId::parse(asset).value();
  key.hardware_generation = HardwareGeneration::first();
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

[[nodiscard]] Result<CreateReceipt> create_object_in(Runtime& runtime, const char* asset) {
  CreateRequest request;
  request.key = key_of(asset);
  request.kind = HardwareKind::Compute;
  request.model = ModelId::parse("model-x").value();
  request.firmware_generation = FirmwareGeneration::first();
  request.initial_state = LifecycleState::Ordered;
  request.provenance = provenance_for(authority_bit(AuthorityScope::Procurement), {EvidenceKind::ProcurementRecord});
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

[[nodiscard]] std::vector<Edge> path_to_installed() {
  return {{LifecycleState::Staged, TransitionReason::DeliveryAccepted, AuthorityScope::Logistics,
           {EvidenceKind::DeliveryReceipt}, false},
          {LifecycleState::Installed, TransitionReason::InstallationCompleted, AuthorityScope::Installation,
           {EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord}, true}};
}

/// Ordered to Retired through the service scope, so the chain also carries a
/// gate decision entry that changes no state.
[[nodiscard]] std::vector<Edge> path_to_retired() {
  return {{LifecycleState::Staged, TransitionReason::DeliveryAccepted, AuthorityScope::Logistics,
           {EvidenceKind::DeliveryReceipt}, false},
          {LifecycleState::Installed, TransitionReason::InstallationCompleted, AuthorityScope::Installation,
           {EvidenceKind::InstallationRecord, EvidenceKind::LocationRecord}, true},
          {LifecycleState::Commissioning, TransitionReason::CommissioningStarted, AuthorityScope::Commissioning,
           {EvidenceKind::CommissioningReport}, false},
          {LifecycleState::Active, TransitionReason::CommissioningPassed, AuthorityScope::Service,
           {EvidenceKind::CommissioningReport}, false},
          {LifecycleState::Retiring, TransitionReason::RetirementApproved, AuthorityScope::Retirement,
           {EvidenceKind::DrainRecord}, false},
          {LifecycleState::Retired, TransitionReason::DecommissionCompleted, AuthorityScope::Decommissioning,
           {EvidenceKind::DecommissioningRecord}, false}};
}

[[nodiscard]] Result<HistoryLog> log_of(const Runtime& runtime, const ObjectKey& key) {
  const Result<HistoryView> view = runtime.history(key);
  if (!view.has_value()) {
    return view.error();
  }
  HistoryLog log;
  log.entries = view.value().entries;
  log.chain_head = view.value().chain_head;
  return log;
}

[[nodiscard]] std::string note_detail(const Error& error, std::string_view field) {
  for (const FieldNote& note : error.notes) {
    if (note.field == field) {
      return note.detail;
    }
  }
  return std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// Verification and replay
// ---------------------------------------------------------------------------

HL_TEST(domain_history, chain_verification_and_replay_over_a_multi_object_multi_generation_walk) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> carried = create_object_in(runtime, "asset-a");
  HL_REQUIRE(carried.has_value());
  const ObjectKey carried_key = carried.value().key;
  HL_REQUIRE(advance(runtime, carried_key, path_to_retired()).has_value());

  const Result<CreateReceipt> idle = create_object_in(runtime, "asset-b");
  HL_REQUIRE(idle.has_value());
  const ObjectKey idle_key = idle.value().key;
  HL_REQUIRE(advance(runtime, idle_key, path_to_installed()).has_value());

  const Result<ObjectView> carried_view = runtime.inspect(carried_key);
  HL_REQUIRE(carried_view.has_value());
  const Result<HistoryLog> carried_log = log_of(runtime, carried_key);
  HL_REQUIRE(carried_log.has_value());

  // The chain holds one entry per committed fact, including the gate decision.
  HL_CHECK_EQ(carried_log.value().entries.size(), static_cast<std::size_t>(8));
  HL_CHECK_EQ(carried_log.value().entries.size(),
              static_cast<std::size_t>(carried_view.value().object.history_entries));
  HL_CHECK_EQ(carried_log.value().entries.front().is_registration(), true);
  HL_CHECK_EQ(carried_log.value().entries[4].is_gate_decision(), true);
  HL_CHECK_EQ(carried_log.value().entries[4].changes_state(), false);
  HL_CHECK_EQ(carried_log.value().entries[4].lifecycle_generation,
              carried_log.value().entries[3].lifecycle_generation);

  // Every entry is bound to the previous head, revisions advance by one and
  // commit sequences are strictly increasing.
  Digest previous_head;
  std::uint64_t expected_revision = 0;
  for (std::size_t index = 0; index < carried_log.value().entries.size(); ++index) {
    const HistoryEntry& entry = carried_log.value().entries[index];
    HL_CHECK_EQ(entry.chain_digest, compute_entry_chain_digest(previous_head, entry));
    previous_head = entry.chain_digest;
    if (index == 0) {
      HL_CHECK_EQ(entry.revision_after, Revision::first());
    } else {
      const HistoryEntry& earlier = carried_log.value().entries[index - 1];
      HL_CHECK_EQ(entry.from, earlier.to);
      HL_CHECK_EQ(entry.revision_before, earlier.revision_after);
      HL_CHECK_EQ(entry.revision_after.value(), earlier.revision_after.value() + 1u);
      if (entry.changes_state()) {
        HL_CHECK_EQ(entry.lifecycle_generation.value(), earlier.lifecycle_generation.value() + 1u);
      } else {
        HL_CHECK_EQ(entry.lifecycle_generation, earlier.lifecycle_generation);
      }
      HL_CHECK(earlier.commit_sequence < entry.commit_sequence);
      HL_CHECK(earlier.logical_time < entry.logical_time);
    }
    expected_revision = entry.revision_after.value();
  }
  HL_CHECK_EQ(expected_revision, carried_view.value().object.revision.value());

  // Verification returns the head, replay returns the state, and the object
  // agrees with both.
  const Result<Digest> verified = verify_history_chain(carried_log.value());
  HL_REQUIRE(verified.has_value());
  HL_CHECK(verified.value() == carried_log.value().chain_head);
  const Result<LifecycleState> replayed = replay_history(carried_log.value());
  HL_REQUIRE(replayed.has_value());
  HL_CHECK_EQ(replayed.value(), LifecycleState::Retired);
  HL_CHECK_EQ(replayed.value(), carried_view.value().object.state);
  HL_CHECK(verify_history_against_object(carried_log.value(), carried_view.value().object).has_value());

  // The walk is visible as a single path through the state machine.
  const LifecycleState expected_states[] = {LifecycleState::Ordered,  LifecycleState::Staged,
                                            LifecycleState::Installed, LifecycleState::Commissioning,
                                            LifecycleState::Active,   LifecycleState::Retiring,
                                            LifecycleState::Retired};
  HL_REQUIRE(!carried_log.value().entries.empty());
  HL_CHECK_EQ(to_string(carried_log.value().entries.front().from), to_string(expected_states[0]));
  std::size_t state_index = 0;
  for (const HistoryEntry& entry : carried_log.value().entries) {
    if (!entry.changes_state()) {
      // A gate decision stays inside one state.
      HL_CHECK_EQ(to_string(entry.from), to_string(entry.to));
      continue;
    }
    ++state_index;
    HL_REQUIRE(state_index < 7u);
    HL_CHECK_EQ(to_string(entry.from), to_string(expected_states[state_index - 1]));
    HL_CHECK_EQ(to_string(entry.to), to_string(expected_states[state_index]));
  }
  HL_CHECK_EQ(state_index, static_cast<std::size_t>(6));

  // A second object has its own chain: different length, different head.
  const Result<HistoryLog> idle_log = log_of(runtime, idle_key);
  HL_REQUIRE(idle_log.has_value());
  HL_CHECK_EQ(idle_log.value().entries.size(), static_cast<std::size_t>(3));
  HL_CHECK(!(idle_log.value().chain_head == carried_log.value().chain_head));
  HL_CHECK(verify_history_chain(idle_log.value()).has_value());
  const Result<LifecycleState> idle_replay = replay_history(idle_log.value());
  HL_REQUIRE(idle_replay.has_value());
  HL_CHECK_EQ(idle_replay.value(), LifecycleState::Installed);
  HL_CHECK(runtime.registry().verify_invariants().has_value());
}

// ---------------------------------------------------------------------------
// Tampering
// ---------------------------------------------------------------------------

HL_TEST(domain_history, tampering_one_entry_is_detected_at_its_chain_link) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());
  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  HL_REQUIRE(advance(runtime, created.value().key, path_to_retired()).has_value());
  const Result<HistoryLog> original = log_of(runtime, created.value().key);
  HL_REQUIRE(original.has_value());
  HL_REQUIRE(original.value().entries.size() >= 6u);

  // A rewritten target state is caught at the entry that carries it.
  HistoryLog rewritten = original.value();
  rewritten.entries[2].to = LifecycleState::Removed;
  const Result<Digest> rewritten_head = verify_history_chain(rewritten);
  HL_CHECK_ERROR(rewritten_head, ErrorCode::IntegrityFailure);
  if (!rewritten_head.has_value()) {
    HL_CHECK_EQ(note_detail(rewritten_head.error(), "entry"), std::string("2"));
    HL_CHECK_EQ(note_detail(rewritten_head.error(), "field"), std::string("chain_digest"));
  }
  HL_CHECK_ERROR(replay_history(rewritten), ErrorCode::IntegrityFailure);

  // A rewritten receipt digest is caught at that entry as well.
  HistoryLog rehashed = original.value();
  rehashed.entries[1].receipt_digest = digest_of(0xABu);
  const Result<Digest> rehashed_head = verify_history_chain(rehashed);
  HL_CHECK_ERROR(rehashed_head, ErrorCode::IntegrityFailure);
  if (!rehashed_head.has_value()) {
    HL_CHECK_EQ(note_detail(rehashed_head.error(), "entry"), std::string("1"));
    HL_CHECK_EQ(note_detail(rehashed_head.error(), "field"), std::string("chain_digest"));
  }

  // A rewritten provenance field is covered by the same binding.
  HistoryLog provenance = original.value();
  provenance.entries[3].provenance.policy_generation = PolicyGeneration::from_value(7);
  const Result<Digest> provenance_head = verify_history_chain(provenance);
  HL_CHECK_ERROR(provenance_head, ErrorCode::IntegrityFailure);
  if (!provenance_head.has_value()) {
    HL_CHECK_EQ(note_detail(provenance_head.error(), "entry"), std::string("3"));
  }

  // A deleted counter is an absent counter: a zero commit sequence is refused
  // before the chain link is even considered.
  HistoryLog missing_sequence = original.value();
  missing_sequence.entries[5].commit_sequence = CommitSequence();
  const Result<Digest> missing_head = verify_history_chain(missing_sequence);
  HL_CHECK_ERROR(missing_head, ErrorCode::IntegrityFailure);
  if (!missing_head.has_value()) {
    HL_CHECK_EQ(note_detail(missing_head.error(), "entry"), std::string("5"));
    HL_CHECK_EQ(note_detail(missing_head.error(), "field"), std::string("commit_sequence"));
  }

  // The original log is untouched by any of the copies above.
  HL_CHECK(verify_history_chain(original.value()).has_value());
}

HL_TEST(domain_history, dropping_an_entry_is_detected) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());
  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  HL_REQUIRE(advance(runtime, created.value().key, path_to_retired()).has_value());
  const Result<HistoryLog> original = log_of(runtime, created.value().key);
  HL_REQUIRE(original.has_value());
  HL_REQUIRE(original.value().entries.size() >= 6u);

  // Without its opening entry the chain does not start at a registration.
  HistoryLog headless = original.value();
  headless.entries.erase(headless.entries.begin());
  const Result<Digest> headless_head = verify_history_chain(headless);
  HL_CHECK_ERROR(headless_head, ErrorCode::IntegrityFailure);
  if (!headless_head.has_value()) {
    HL_CHECK_EQ(note_detail(headless_head.error(), "entry"), std::string("0"));
    HL_CHECK_EQ(note_detail(headless_head.error(), "field"), std::string("from"));
  }
  HL_CHECK_ERROR(replay_history(headless), ErrorCode::IntegrityFailure);

  // The opening entry must be a registration, not merely a self edge: a gate
  // decision at the front is refused the same way.
  HistoryLog gate_first = original.value();
  HistoryEntry gate = gate_first.entries[4];
  gate_first.entries.erase(gate_first.entries.begin(), gate_first.entries.begin() + 5);
  gate_first.entries.insert(gate_first.entries.begin(), gate);
  HL_CHECK_ERROR(verify_history_chain(gate_first), ErrorCode::IntegrityFailure);

  // A dropped middle entry breaks the link at the point where the gap opens.
  HistoryLog gapped = original.value();
  gapped.entries.erase(gapped.entries.begin() + 2);
  const Result<Digest> gapped_head = verify_history_chain(gapped);
  HL_CHECK_ERROR(gapped_head, ErrorCode::IntegrityFailure);
  if (!gapped_head.has_value()) {
    HL_CHECK_EQ(note_detail(gapped_head.error(), "entry"), std::string("2"));
  }

  // The original still verifies.
  HL_CHECK(verify_history_chain(original.value()).has_value());
}

HL_TEST(domain_history, reordering_two_entries_is_detected) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());
  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  HL_REQUIRE(advance(runtime, created.value().key, path_to_retired()).has_value());
  const Result<HistoryLog> original = log_of(runtime, created.value().key);
  HL_REQUIRE(original.has_value());
  HL_REQUIRE(original.value().entries.size() >= 6u);

  HistoryLog swapped = original.value();
  std::swap(swapped.entries[1], swapped.entries[2]);
  const Result<Digest> swapped_head = verify_history_chain(swapped);
  HL_CHECK_ERROR(swapped_head, ErrorCode::IntegrityFailure);
  if (!swapped_head.has_value()) {
    HL_CHECK_EQ(note_detail(swapped_head.error(), "entry"), std::string("1"));
    HL_CHECK_EQ(note_detail(swapped_head.error(), "field"), std::string("chain_digest"));
  }
  HL_CHECK_ERROR(replay_history(swapped), ErrorCode::IntegrityFailure);

  HistoryLog tail_swapped = original.value();
  const std::size_t last = tail_swapped.entries.size() - 1;
  std::swap(tail_swapped.entries[last - 1], tail_swapped.entries[last]);
  HL_CHECK_ERROR(verify_history_chain(tail_swapped), ErrorCode::IntegrityFailure);

  HistoryLog reversed = original.value();
  std::reverse(reversed.entries.begin(), reversed.entries.end());
  HL_CHECK_ERROR(verify_history_chain(reversed), ErrorCode::IntegrityFailure);
  HL_CHECK_ERROR(replay_history(reversed), ErrorCode::IntegrityFailure);

  HL_CHECK(verify_history_chain(original.value()).has_value());
}

HL_TEST(domain_history, duplicating_an_entry_is_detected) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());
  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  HL_REQUIRE(advance(runtime, created.value().key, path_to_retired()).has_value());
  const Result<HistoryLog> original = log_of(runtime, created.value().key);
  HL_REQUIRE(original.has_value());
  HL_REQUIRE(original.value().entries.size() >= 6u);

  // A repeated entry no longer continues the head the chain has reached, even
  // though the copy carries a perfectly valid chain digest of its own.
  HistoryLog duplicated = original.value();
  duplicated.entries.insert(duplicated.entries.begin() + 2, duplicated.entries[1]);
  const Result<Digest> duplicated_head = verify_history_chain(duplicated);
  HL_CHECK_ERROR(duplicated_head, ErrorCode::IntegrityFailure);
  if (!duplicated_head.has_value()) {
    HL_CHECK_EQ(note_detail(duplicated_head.error(), "entry"), std::string("2"));
    HL_CHECK_EQ(note_detail(duplicated_head.error(), "field"), std::string("chain_digest"));
  }
  HL_CHECK_ERROR(replay_history(duplicated), ErrorCode::IntegrityFailure);

  // Repeating the last entry is refused as well: it binds to a head that is no
  // longer the head.
  HistoryLog repeated_tail = original.value();
  repeated_tail.entries.push_back(repeated_tail.entries.back());
  const Result<Digest> tail_head = verify_history_chain(repeated_tail);
  HL_CHECK_ERROR(tail_head, ErrorCode::IntegrityFailure);
  if (!tail_head.has_value()) {
    HL_CHECK_EQ(note_detail(tail_head.error(), "entry"), std::to_string(original.value().entries.size()));
  }

  HL_CHECK(verify_history_chain(original.value()).has_value());
}

// ---------------------------------------------------------------------------
// The chain and the object
// ---------------------------------------------------------------------------

HL_TEST(domain_history, verify_history_against_object_agrees_with_the_object) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());
  const Result<CreateReceipt> created = create_object_in(runtime, "asset-a");
  HL_REQUIRE(created.has_value());
  HL_REQUIRE(advance(runtime, created.value().key, path_to_retired()).has_value());
  const Result<HistoryLog> log = log_of(runtime, created.value().key);
  HL_REQUIRE(log.has_value());
  const Result<ObjectView> view = runtime.inspect(created.value().key);
  HL_REQUIRE(view.has_value());
  HL_CHECK(verify_history_against_object(log.value(), view.value().object).has_value());

  // A different state is a mismatch, not a repair.
  HardwareObject edited = view.value().object;
  edited.state = LifecycleState::Active;
  HL_CHECK_ERROR(verify_history_against_object(log.value(), edited), ErrorCode::StateMismatch);

  // A different revision, generation, identity or entry count is an integrity
  // failure: the chain does not end where the object claims to be.
  HardwareObject revision = view.value().object;
  revision.revision = Revision::from_value(99);
  HL_CHECK_ERROR(verify_history_against_object(log.value(), revision), ErrorCode::IntegrityFailure);

  HardwareObject generation = view.value().object;
  generation.lifecycle_generation = LifecycleGeneration::from_value(99);
  HL_CHECK_ERROR(verify_history_against_object(log.value(), generation), ErrorCode::IntegrityFailure);

  HardwareObject entries = view.value().object;
  entries.history_entries = 99;
  HL_CHECK_ERROR(verify_history_against_object(log.value(), entries), ErrorCode::IntegrityFailure);

  HardwareObject identity = view.value().object;
  identity.key = key_of("asset-z");
  HL_CHECK_ERROR(verify_history_against_object(log.value(), identity), ErrorCode::IntegrityFailure);

  // An empty log proves no state at all.
  const HistoryLog empty;
  HL_CHECK_ERROR(replay_history(empty), ErrorCode::IntegrityFailure);
  HL_CHECK_ERROR(verify_history_against_object(empty, view.value().object), ErrorCode::IntegrityFailure);
  const Result<Digest> empty_head = verify_history_chain(empty);
  HL_REQUIRE(empty_head.has_value());
  HL_CHECK(empty_head.value().is_zero());
}

HL_TEST(domain_history, the_chain_head_is_the_last_entry_digest) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> first = create_object_in(runtime, "asset-a");
  HL_REQUIRE(first.has_value());
  const Result<CreateReceipt> second = create_object_in(runtime, "asset-b");
  HL_REQUIRE(second.has_value());
  HL_REQUIRE(advance(runtime, first.value().key, path_to_retired()).has_value());
  HL_REQUIRE(advance(runtime, second.value().key, path_to_installed()).has_value());

  const ObjectKey keys[] = {first.value().key, second.value().key};
  for (const ObjectKey& key : keys) {
    const Result<HistoryView> view = runtime.history(key);
    HL_REQUIRE(view.has_value());
    const Result<HistoryLog> log = log_of(runtime, key);
    HL_REQUIRE(log.has_value());
    HL_CHECK_EQ(view.value().chain_head, log.value().entries.back().chain_digest);
    HL_CHECK_EQ(view.value().chain_head, runtime.registry().history(key)->chain_head);

    const Result<Digest> verified = verify_history_chain(log.value());
    HL_REQUIRE(verified.has_value());
    HL_CHECK_EQ(verified.value(), view.value().chain_head);
    HL_CHECK(!view.value().chain_head.is_zero());

    // The head is rebuilt from the raw bytes of the entries, not read from the
    // cached field.
    Digest rebuilt;
    for (const HistoryEntry& entry : log.value().entries) {
      rebuilt = compute_entry_chain_digest(rebuilt, entry);
    }
    HL_CHECK_EQ(rebuilt, view.value().chain_head);
  }
}

HL_TEST(domain_history, one_objects_history_is_unaffected_by_another_objects_entries) {
  Result<Runtime> opened = Runtime::open_ephemeral();
  HL_REQUIRE(opened.has_value());
  Runtime runtime = std::move(opened.value());

  const Result<CreateReceipt> quiet = create_object_in(runtime, "asset-a");
  HL_REQUIRE(quiet.has_value());
  const ObjectKey quiet_key = quiet.value().key;
  HL_REQUIRE(advance(runtime, quiet_key, path_to_installed()).has_value());

  const Result<HistoryLog> quiet_before = log_of(runtime, quiet_key);
  HL_REQUIRE(quiet_before.has_value());
  std::vector<Digest> chain_before;
  for (const HistoryEntry& entry : quiet_before.value().entries) {
    chain_before.push_back(entry.chain_digest);
  }
  const Digest quiet_head = quiet_before.value().chain_head;
  const Result<ObjectView> quiet_view_before = runtime.inspect(quiet_key);
  HL_REQUIRE(quiet_view_before.has_value());
  const Digest quiet_state_before = runtime.registry().state_digest();

  // Another object moves through many entries.
  const Result<CreateReceipt> busy = create_object_in(runtime, "asset-b");
  HL_REQUIRE(busy.has_value());
  HL_REQUIRE(advance(runtime, busy.value().key, path_to_retired()).has_value());

  const Result<HistoryLog> quiet_after = log_of(runtime, quiet_key);
  HL_REQUIRE(quiet_after.has_value());
  HL_CHECK_EQ(quiet_after.value().entries.size(), chain_before.size());
  HL_CHECK_EQ(quiet_after.value().chain_head, quiet_head);
  for (std::size_t index = 0; index < chain_before.size(); ++index) {
    HL_CHECK_EQ(quiet_after.value().entries[index].chain_digest, chain_before[index]);
    HL_CHECK_EQ(quiet_after.value().entries[index].key, quiet_key);
  }
  const Result<ObjectView> quiet_view_after = runtime.inspect(quiet_key);
  HL_REQUIRE(quiet_view_after.has_value());
  HL_CHECK_EQ(quiet_view_after.value().object.state, quiet_view_before.value().object.state);
  HL_CHECK_EQ(quiet_view_after.value().object.revision, quiet_view_before.value().object.revision);
  HL_CHECK_EQ(quiet_view_after.value().object.lifecycle_generation,
              quiet_view_before.value().object.lifecycle_generation);
  HL_CHECK_EQ(quiet_view_after.value().object.history_entries,
              static_cast<std::uint64_t>(chain_before.size()));

  // The registry as a whole moved, which is exactly why the per object chain is
  // the thing that proves which object changed.
  HL_CHECK(!(runtime.registry().state_digest() == quiet_state_before));
  const Result<HistoryLog> busy_log = log_of(runtime, busy.value().key);
  HL_REQUIRE(busy_log.has_value());
  HL_CHECK_EQ(busy_log.value().entries.size(), static_cast<std::size_t>(8));
  for (const HistoryEntry& entry : busy_log.value().entries) {
    HL_CHECK_EQ(entry.key, busy.value().key);
  }
  HL_CHECK(runtime.registry().verify_invariants().has_value());
}
