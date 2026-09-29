// Hardware Lifecycle - domain proofs for the authoritative in memory registry.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The registry is the authority the runtime copies for every mutation, so its
// own rules are proved directly: strict insertion, append only history with
// continuity and chain binding, watermarks that never move backwards, ordered
// deterministic derived values and an invariant check that notices an object
// which was edited behind its own history chain.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "hardware_lifecycle/hardware_lifecycle.hpp"
#include "test_framework.hpp"

namespace {

using namespace hardware_lifecycle;

[[nodiscard]] Location location_of(const char* site, const char* rack, const char* slot) {
  Location location;
  location.site = SiteId::parse(site).value();
  location.rack = RackId::parse(rack).value();
  location.slot = SlotId::parse(slot).value();
  return location;
}

/// One object in the state and at the revision the caller names. A state that
/// implies a facility location gets one unless the caller wants it absent.
[[nodiscard]] HardwareObject make_object(const char* asset, HardwareGeneration generation, LifecycleState state,
                                         Revision revision, bool with_location = true) {
  HardwareObject object;
  object.key.asset = AssetId::parse(asset).value();
  object.key.hardware_generation = generation;
  object.kind = HardwareKind::Compute;
  object.model = ModelId::parse("model-x").value();
  object.firmware_generation = FirmwareGeneration::first();
  object.lifecycle_generation = LifecycleGeneration::from_value(revision.value());
  object.state = state;
  object.revision = revision;
  object.created_at = LogicalTime::first();
  object.updated_at = LogicalTime::first();
  object.authority = AuthorityState::Live;
  if (with_location && requires_location(state)) {
    object.location = location_of("site-a", "rack-1", "slot-1");
  }
  return object;
}

/// The entry that opens a chain: from equal to to, the registration reason and
/// revision one, bound to the zero digest.
[[nodiscard]] HistoryEntry registration_entry(const HardwareObject& object) {
  HistoryEntry entry;
  entry.commit_sequence = CommitSequence::first();
  entry.logical_time = LogicalTime::first();
  entry.key = object.key;
  entry.lifecycle_generation = object.lifecycle_generation;
  entry.revision_after = object.revision;
  entry.from = object.state;
  entry.to = object.state;
  entry.reason = TransitionReason::ObjectRegistered;
  entry.chain_digest = compute_entry_chain_digest(Digest(), entry);
  return entry;
}

/// A state changing entry that continues the chain the previous entry ended.
[[nodiscard]] HistoryEntry state_entry(const HistoryEntry& previous, const Digest& previous_head, LifecycleState from,
                                       LifecycleState to, TransitionReason reason, std::uint64_t sequence,
                                       std::uint64_t time) {
  HistoryEntry entry;
  entry.commit_sequence = CommitSequence::from_value(sequence);
  entry.logical_time = LogicalTime::from_value(time);
  entry.key = previous.key;
  entry.lifecycle_generation = LifecycleGeneration::from_value(previous.lifecycle_generation.value() + 1u);
  entry.revision_before = previous.revision_after;
  entry.revision_after = Revision::from_value(previous.revision_after.value() + 1u);
  entry.from = from;
  entry.to = to;
  entry.reason = reason;
  entry.chain_digest = compute_entry_chain_digest(previous_head, entry);
  return entry;
}

/// A gate decision: a durable mutation that leaves the lifecycle state and the
/// lifecycle generation where they were.
[[nodiscard]] HistoryEntry gate_entry(const HistoryEntry& previous, const Digest& previous_head, LifecycleState state,
                                      TransitionReason reason, std::uint64_t sequence, std::uint64_t time) {
  HistoryEntry entry;
  entry.commit_sequence = CommitSequence::from_value(sequence);
  entry.logical_time = LogicalTime::from_value(time);
  entry.key = previous.key;
  entry.lifecycle_generation = previous.lifecycle_generation;
  entry.revision_before = previous.revision_after;
  entry.revision_after = Revision::from_value(previous.revision_after.value() + 1u);
  entry.from = state;
  entry.to = state;
  entry.reason = reason;
  entry.chain_digest = compute_entry_chain_digest(previous_head, entry);
  return entry;
}

/// The chain head of one object, or the zero digest when it has no chain yet.
[[nodiscard]] Digest registry_head(const Registry& registry, const ObjectKey& key) {
  const HistoryLog* log = registry.history(key);
  return log != nullptr ? log->chain_head : Digest();
}

/// A registry with one object, two chain entries and matching watermarks: the
/// smallest state verify_invariants() accepts.
[[nodiscard]] Registry consistent_registry(hl_test::Context& ctx, HardwareGeneration generation = HardwareGeneration::first()) {
  Registry registry;
  HardwareObject object = make_object("asset-a", generation, LifecycleState::Ordered, Revision::first());
  object.updated_at = LogicalTime::from_value(2);

  const Result<void> inserted = registry.insert_object(object);
  (void)ctx.check(inserted.has_value(), "insert_object", __FILE__, __LINE__);

  const HistoryEntry first = registration_entry(object);
  const Result<void> appended_first = registry.append_history(first);
  (void)ctx.check(appended_first.has_value(), "append_history(registration)", __FILE__, __LINE__);

  const HistoryEntry second = state_entry(first, first.chain_digest, LifecycleState::Ordered,
                                          LifecycleState::Staged, TransitionReason::DeliveryAccepted, 2, 2);
  const Result<void> appended_second = registry.append_history(second);
  (void)ctx.check(appended_second.has_value(), "append_history(transition)", __FILE__, __LINE__);

  const Result<void> watermarks = registry.set_watermarks(CommitSequence::from_value(2), LogicalTime::from_value(2),
                                                          ObservationSequence::first());
  (void)ctx.check(watermarks.has_value(), "set_watermarks", __FILE__, __LINE__);

  // The object follows the chain it just recorded: replay lands on Staged at
  // revision two and generation two, exactly like a committed mutation would.
  HardwareObject* live = registry.find(object.key);
  (void)ctx.check(live != nullptr, "find", __FILE__, __LINE__);
  if (live != nullptr) {
    live->state = LifecycleState::Staged;
    live->revision = Revision::from_value(2);
    live->lifecycle_generation = LifecycleGeneration::from_value(2);
  }
  return registry;
}

}  // namespace

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

HL_TEST(domain_registry, insert_find_and_contains_agree) {
  Registry registry;
  const HardwareObject object = make_object("asset-a", HardwareGeneration::first(), LifecycleState::Ordered,
                                            Revision::first());
  HL_CHECK(registry.insert_object(object).has_value());

  HL_CHECK_EQ(registry.object_count(), static_cast<std::size_t>(1));
  HL_CHECK_EQ(registry.contains(object.key), true);
  HL_CHECK_EQ(registry.contains(make_object("asset-b", HardwareGeneration::first(), LifecycleState::Ordered,
                                            Revision::first()).key),
              false);

  const HardwareObject* found = registry.find(object.key);
  HL_REQUIRE(found != nullptr);
  HL_CHECK(found->key == object.key);
  HL_CHECK_EQ(found->state, LifecycleState::Ordered);
  HL_CHECK_EQ(found->revision, Revision::first());
  HL_CHECK_EQ(found->authority, AuthorityState::Live);
  HL_CHECK_EQ(registry.find(make_object("asset-b", HardwareGeneration::first(), LifecycleState::Ordered,
                                        Revision::first()).key),
              nullptr);
  HL_CHECK_EQ(registry.objects().size(), static_cast<std::size_t>(1));

  // Ordering is by key, never by insertion order.
  Registry ordered;
  const char* const assets[] = {"asset-c", "asset-a", "asset-b"};
  for (const char* asset : assets) {
    const HardwareObject item = make_object(asset, HardwareGeneration::first(), LifecycleState::Ordered,
                                            Revision::first());
    HL_CHECK(ordered.insert_object(item).has_value());
  }
  std::vector<std::string> seen_assets;
  for (const auto& entry : ordered.objects()) {
    seen_assets.push_back(entry.first.asset.value());
  }
  HL_REQUIRE(seen_assets.size() == 3u);
  HL_CHECK_EQ(seen_assets[0], std::string("asset-a"));
  HL_CHECK_EQ(seen_assets[1], std::string("asset-b"));
  HL_CHECK_EQ(seen_assets[2], std::string("asset-c"));
}

HL_TEST(domain_registry, duplicate_insert_is_object_exists_and_changes_nothing) {
  Registry registry;
  const HardwareObject object = make_object("asset-a", HardwareGeneration::first(), LifecycleState::Ordered,
                                            Revision::first());
  HL_REQUIRE(registry.insert_object(object).has_value());
  const Digest before = registry.state_digest();

  HL_CHECK_ERROR(registry.insert_object(object), ErrorCode::ObjectExists);
  HL_CHECK_EQ(registry.object_count(), static_cast<std::size_t>(1));
  HL_CHECK(registry.state_digest() == before);

  // A second hardware generation is a different key, not a duplicate.
  const HardwareObject replacement = make_object("asset-a", HardwareGeneration::from_value(2), LifecycleState::Ordered,
                                                 Revision::first());
  HL_CHECK(registry.insert_object(replacement).has_value());
  HL_CHECK_EQ(registry.object_count(), static_cast<std::size_t>(2));
  HL_CHECK(registry.find(object.key) != registry.find(replacement.key));

  // An incomplete identity is refused before anything is stored.
  HardwareObject invalid = make_object("asset-b", HardwareGeneration::first(), LifecycleState::Ordered,
                                       Revision::first());
  invalid.key.asset = AssetId();
  HL_CHECK_ERROR(registry.insert_object(invalid), ErrorCode::MalformedRequest);

  invalid = make_object("asset-b", HardwareGeneration::first(), LifecycleState::Ordered, Revision::first());
  invalid.key.hardware_generation = HardwareGeneration();
  HL_CHECK_ERROR(registry.insert_object(invalid), ErrorCode::MalformedRequest);

  invalid = make_object("asset-b", HardwareGeneration::first(), LifecycleState::Ordered, Revision::first());
  invalid.revision = Revision();
  HL_CHECK_ERROR(registry.insert_object(invalid), ErrorCode::MalformedRequest);

  invalid = make_object("asset-b", HardwareGeneration::first(), LifecycleState::Ordered, Revision::first());
  invalid.lifecycle_generation = LifecycleGeneration();
  HL_CHECK_ERROR(registry.insert_object(invalid), ErrorCode::MalformedRequest);

  HL_CHECK_EQ(registry.object_count(), static_cast<std::size_t>(2));
}

HL_TEST(domain_registry, an_object_whose_state_requires_a_location_cannot_be_inserted_without_one) {
  for (const LifecycleState state : all_lifecycle_states()) {
    const std::string label(to_string(state));

    Registry without;
    const HardwareObject bare = make_object("asset-loc", HardwareGeneration::first(), state, Revision::first(), false);
    const Result<void> inserted_bare = without.insert_object(bare);
    if (requires_location(state)) {
      HL_CHECK_MSG(!inserted_bare.has_value(), label.c_str());
      if (!inserted_bare.has_value()) {
        HL_CHECK_EQ(inserted_bare.error().code, ErrorCode::MissingLocation);
      }
    } else {
      HL_CHECK_MSG(inserted_bare.has_value(), label.c_str());
    }

    Registry with;
    const HardwareObject placed = make_object("asset-loc", HardwareGeneration::first(), state, Revision::first(), true);
    HL_CHECK_MSG(with.insert_object(placed).has_value(), label.c_str());
    const HardwareObject* found = with.find(placed.key);
    HL_REQUIRE(found != nullptr);
    if (requires_location(state)) {
      HL_CHECK_MSG(found->location.has_value(), label.c_str());
    }
  }
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

HL_TEST(domain_registry, append_history_enforces_the_registration_opening_entry) {
  Registry registry;
  const HardwareObject object = make_object("asset-a", HardwareGeneration::first(), LifecycleState::Ordered,
                                            Revision::first());
  HL_REQUIRE(registry.insert_object(object).has_value());

  // Unknown object first: history is never written for an object that is absent.
  HistoryEntry orphan = registration_entry(object);
  orphan.key = make_object("asset-missing", HardwareGeneration::first(), LifecycleState::Ordered, Revision::first()).key;
  orphan.chain_digest = compute_entry_chain_digest(Digest(), orphan);
  HL_CHECK_ERROR(registry.append_history(orphan), ErrorCode::UnknownAsset);

  // A chain does not open with a state change.
  HistoryEntry transition = registration_entry(object);
  transition.to = LifecycleState::Staged;
  transition.reason = TransitionReason::DeliveryAccepted;
  transition.chain_digest = compute_entry_chain_digest(Digest(), transition);
  HL_CHECK_ERROR(registry.append_history(transition), ErrorCode::IntegrityFailure);

  // A gate decision is not a registration either.
  HistoryEntry gate = registration_entry(object);
  gate.reason = TransitionReason::EligibilityGateOpened;
  gate.chain_digest = compute_entry_chain_digest(Digest(), gate);
  HL_CHECK_ERROR(registry.append_history(gate), ErrorCode::IntegrityFailure);

  // A registration entry establishes revision one and has no predecessor.
  HistoryEntry with_before = registration_entry(object);
  with_before.revision_before = Revision::first();
  with_before.chain_digest = compute_entry_chain_digest(Digest(), with_before);
  HL_CHECK_ERROR(registry.append_history(with_before), ErrorCode::IntegrityFailure);

  HistoryEntry wrong_revision = registration_entry(object);
  wrong_revision.revision_after = Revision::from_value(2);
  wrong_revision.chain_digest = compute_entry_chain_digest(Digest(), wrong_revision);
  HL_CHECK_ERROR(registry.append_history(wrong_revision), ErrorCode::IntegrityFailure);

  const HistoryEntry opening = registration_entry(object);
  const Digest opening_head = opening.chain_digest;
  HL_REQUIRE(registry.append_history(opening).has_value());
  HL_REQUIRE(registry.history(object.key) != nullptr);
  HL_CHECK_EQ(registry.history(object.key)->entries.size(), static_cast<std::size_t>(1));
  HL_CHECK_EQ(registry.history(object.key)->chain_head, opening_head);
  HL_CHECK_EQ(registry.find(object.key)->history_entries, static_cast<std::uint64_t>(1));

  // Only the first entry may be a registration entry.
  HistoryEntry second_registration = registration_entry(object);
  second_registration.commit_sequence = CommitSequence::from_value(2);
  second_registration.logical_time = LogicalTime::from_value(2);
  second_registration.chain_digest = compute_entry_chain_digest(opening_head, second_registration);
  HL_CHECK_ERROR(registry.append_history(second_registration), ErrorCode::IntegrityFailure);
  HL_CHECK_EQ(registry.history(object.key)->entries.size(), static_cast<std::size_t>(1));
}

HL_TEST(domain_registry, append_history_enforces_per_entry_continuity) {
  Registry base;
  const HardwareObject object = make_object("asset-a", HardwareGeneration::first(), LifecycleState::Ordered,
                                            Revision::first());
  HL_REQUIRE(base.insert_object(object).has_value());
  const HistoryEntry opening = registration_entry(object);
  HL_REQUIRE(base.append_history(opening).has_value());
  const HistoryEntry running = state_entry(opening, registry_head(base, object.key), LifecycleState::Ordered,
                                           LifecycleState::Staged, TransitionReason::DeliveryAccepted, 2, 2);
  HL_REQUIRE(base.append_history(running).has_value());
  const Digest head = registry_head(base, object.key);
  HL_CHECK_EQ(base.history(object.key)->entries.size(), static_cast<std::size_t>(2));

  // Each violation is attempted against an unmodified copy, so the error under
  // test is the only rule the entry breaks.
  const auto rejects = [&](HistoryEntry entry) {
    Registry attempt = base;
    entry.chain_digest = compute_entry_chain_digest(head, entry);
    const Result<void> appended = attempt.append_history(entry);
    HL_CHECK_ERROR(appended, ErrorCode::IntegrityFailure);
    HL_CHECK_EQ(attempt.history(running.key)->entries.size(), static_cast<std::size_t>(2));
  };

  HistoryEntry wrong_from = state_entry(running, head, LifecycleState::Installed, LifecycleState::Commissioning,
                                        TransitionReason::CommissioningStarted, 3, 3);
  rejects(wrong_from);

  HistoryEntry wrong_before = state_entry(running, head, LifecycleState::Staged, LifecycleState::Installed,
                                          TransitionReason::InstallationCompleted, 3, 3);
  wrong_before.revision_before = Revision::from_value(9);
  rejects(wrong_before);

  HistoryEntry skipped_revision = state_entry(running, head, LifecycleState::Staged, LifecycleState::Installed,
                                              TransitionReason::InstallationCompleted, 3, 3);
  skipped_revision.revision_after = Revision::from_value(4);
  rejects(skipped_revision);

  HistoryEntry skipped_generation = state_entry(running, head, LifecycleState::Staged, LifecycleState::Installed,
                                                TransitionReason::InstallationCompleted, 3, 3);
  skipped_generation.lifecycle_generation = LifecycleGeneration::from_value(4);
  rejects(skipped_generation);

  HistoryEntry frozen_commit = state_entry(running, head, LifecycleState::Staged, LifecycleState::Installed,
                                           TransitionReason::InstallationCompleted, 2, 3);
  rejects(frozen_commit);

  HistoryEntry frozen_time = state_entry(running, head, LifecycleState::Staged, LifecycleState::Installed,
                                         TransitionReason::InstallationCompleted, 3, 2);
  rejects(frozen_time);

  // A gate decision leaves the lifecycle generation alone; advancing it is an
  // integrity failure, keeping it is accepted.
  HistoryEntry advanced_gate = gate_entry(running, head, LifecycleState::Staged,
                                          TransitionReason::EligibilityGateOpened, 3, 3);
  advanced_gate.lifecycle_generation = LifecycleGeneration::from_value(3);
  rejects(advanced_gate);

  Registry accepted = base;
  const HistoryEntry gate = gate_entry(running, head, LifecycleState::Staged, TransitionReason::EligibilityGateOpened,
                                       3, 3);
  HL_REQUIRE(accepted.append_history(gate).has_value());
  HL_CHECK_EQ(accepted.history(object.key)->entries.size(), static_cast<std::size_t>(3));
  HL_CHECK(accepted.history(object.key)->entries.back().is_gate_decision());
  HL_CHECK_EQ(accepted.history(object.key)->entries.back().lifecycle_generation, running.lifecycle_generation);
  HL_CHECK_EQ(accepted.history(object.key)->entries.back().revision_after, Revision::from_value(3));
}

HL_TEST(domain_registry, append_history_requires_the_chain_digest_to_match) {
  Registry base;
  const HardwareObject object = make_object("asset-a", HardwareGeneration::first(), LifecycleState::Ordered,
                                            Revision::first());
  HL_REQUIRE(base.insert_object(object).has_value());
  const HistoryEntry opening = registration_entry(object);
  const Digest opening_head = opening.chain_digest;
  HL_REQUIRE(base.append_history(opening).has_value());

  HistoryEntry entry = state_entry(opening, opening_head, LifecycleState::Ordered, LifecycleState::Staged,
                                   TransitionReason::DeliveryAccepted, 2, 2);

  // A tampered chain digest is refused, and the chain keeps its previous head.
  HistoryEntry tampered = entry;
  tampered.chain_digest = Digest();
  HL_CHECK_ERROR(base.append_history(tampered), ErrorCode::IntegrityFailure);
  HL_CHECK_EQ(base.history(object.key)->entries.size(), static_cast<std::size_t>(1));
  HL_CHECK_EQ(base.history(object.key)->chain_head, opening_head);

  // A digest computed over the wrong predecessor is refused too.
  HistoryEntry wrong_head = entry;
  wrong_head.chain_digest = compute_entry_chain_digest(tampered.chain_digest, wrong_head);
  HL_CHECK_ERROR(base.append_history(wrong_head), ErrorCode::IntegrityFailure);
  HL_CHECK_EQ(base.history(object.key)->entries.size(), static_cast<std::size_t>(1));

  // The correct entry is accepted, and appending it twice fails: the second copy
  // no longer continues the chain.
  HL_REQUIRE(base.append_history(entry).has_value());
  HL_CHECK_EQ(base.history(object.key)->chain_head, entry.chain_digest);
  HL_CHECK_ERROR(base.append_history(entry), ErrorCode::IntegrityFailure);
  HL_CHECK_EQ(base.history(object.key)->entries.size(), static_cast<std::size_t>(2));
  HL_CHECK_EQ(base.history_entry_count(), static_cast<std::size_t>(2));
}

// ---------------------------------------------------------------------------
// Watermarks and derived values
// ---------------------------------------------------------------------------

HL_TEST(domain_registry, set_watermarks_never_moves_backwards) {
  Registry registry;
  HL_CHECK(registry.set_watermarks(CommitSequence::from_value(5), LogicalTime::from_value(5),
                                   ObservationSequence::from_value(5))
               .has_value());
  HL_CHECK_EQ(registry.commit_sequence(), CommitSequence::from_value(5));
  HL_CHECK_EQ(registry.logical_time(), LogicalTime::from_value(5));
  HL_CHECK_EQ(registry.observation_sequence(), ObservationSequence::from_value(5));

  // Standing still is not moving backwards.
  HL_CHECK(registry.set_watermarks(CommitSequence::from_value(5), LogicalTime::from_value(5),
                                   ObservationSequence::from_value(5))
               .has_value());
  HL_CHECK(registry.set_watermarks(CommitSequence::from_value(6), LogicalTime::from_value(7),
                                   ObservationSequence::from_value(9))
               .has_value());
  HL_CHECK_EQ(registry.commit_sequence(), CommitSequence::from_value(6));
  HL_CHECK_EQ(registry.logical_time(), LogicalTime::from_value(7));
  HL_CHECK_EQ(registry.observation_sequence(), ObservationSequence::from_value(9));

  const Result<void> rewind_commit =
      registry.set_watermarks(CommitSequence::from_value(5), LogicalTime::from_value(7),
                              ObservationSequence::from_value(9));
  HL_CHECK_ERROR(rewind_commit, ErrorCode::IntegrityFailure);
  const Result<void> rewind_time =
      registry.set_watermarks(CommitSequence::from_value(6), LogicalTime::from_value(6),
                              ObservationSequence::from_value(9));
  HL_CHECK_ERROR(rewind_time, ErrorCode::IntegrityFailure);
  const Result<void> rewind_observation =
      registry.set_watermarks(CommitSequence::from_value(6), LogicalTime::from_value(7),
                              ObservationSequence::from_value(8));
  HL_CHECK_ERROR(rewind_observation, ErrorCode::IntegrityFailure);

  // A refused rewind leaves every watermark exactly where it was.
  HL_CHECK_EQ(registry.commit_sequence(), CommitSequence::from_value(6));
  HL_CHECK_EQ(registry.logical_time(), LogicalTime::from_value(7));
  HL_CHECK_EQ(registry.observation_sequence(), ObservationSequence::from_value(9));

  // A fresh registry has no observation yet: the first sequence is one, never
  // zero and never a stale reuse.
  const Registry fresh;
  const Result<ObservationSequence> next = fresh.next_observation_sequence();
  HL_REQUIRE(next.has_value());
  HL_CHECK_EQ(next.value(), ObservationSequence::first());
  const Result<ObservationSequence> after = registry.next_observation_sequence();
  HL_REQUIRE(after.has_value());
  HL_CHECK_EQ(after.value(), ObservationSequence::from_value(10));
}

HL_TEST(domain_registry, state_digest_is_stable_across_insertion_orders) {
  const char* const assets[] = {"asset-a", "asset-b", "asset-c"};

  Registry forward;
  Registry backward;
  for (std::size_t index = 0; index < 3; ++index) {
    const HardwareObject object = make_object(assets[index], HardwareGeneration::from_value(index + 1),
                                              LifecycleState::Staged, Revision::first());
    HL_CHECK(forward.insert_object(object).has_value());
  }
  for (std::size_t index = 3; index > 0; --index) {
    const HardwareObject object = make_object(assets[index - 1], HardwareGeneration::from_value(index),
                                              LifecycleState::Staged, Revision::first());
    HL_CHECK(backward.insert_object(object).has_value());
  }

  HL_CHECK_EQ(forward.object_count(), static_cast<std::size_t>(3));
  HL_CHECK_EQ(backward.object_count(), static_cast<std::size_t>(3));
  HL_CHECK(forward.state_digest() == backward.state_digest());
  HL_CHECK(forward.state_digest() == forward.state_digest());

  // The digest covers real state: a single differing field is a different state.
  Registry changed = forward;
  HardwareObject* object = changed.find(make_object("asset-b", HardwareGeneration::from_value(2),
                                                    LifecycleState::Staged, Revision::first()).key);
  HL_REQUIRE(object != nullptr);
  object->revision = Revision::from_value(9);
  HL_CHECK(!(changed.state_digest() == forward.state_digest()));

  Registry regrouped = forward;
  HardwareObject* moved = regrouped.find(make_object("asset-c", HardwareGeneration::from_value(3),
                                                     LifecycleState::Staged, Revision::first()).key);
  HL_REQUIRE(moved != nullptr);
  moved->location = location_of("site-z", "rack-9", "slot-9");
  HL_CHECK(!(regrouped.state_digest() == forward.state_digest()));
}

// ---------------------------------------------------------------------------
// Invariants and fencing
// ---------------------------------------------------------------------------

HL_TEST(domain_registry, verify_invariants_accepts_a_consistent_registry_and_rejects_edits_behind_the_chain) {
  Registry registry = consistent_registry(hl_ctx);
  const ObjectKey key = make_object("asset-a", HardwareGeneration::first(), LifecycleState::Ordered,
                                    Revision::first()).key;
  HL_REQUIRE(registry.verify_invariants().has_value());
  HL_REQUIRE(registry.history(key) != nullptr);
  HL_CHECK_EQ(registry.history(key)->entries.size(), static_cast<std::size_t>(2));

  // Editing the object without appending the entry that would justify the edit
  // is exactly the corruption the chain exists to detect.
  Registry changed_state = registry;
  HL_REQUIRE(changed_state.find(key) != nullptr);
  changed_state.find(key)->state = LifecycleState::Active;
  const Result<void> state_failure = changed_state.verify_invariants();
  HL_CHECK_ERROR(state_failure, ErrorCode::StateMismatch);

  Registry changed_revision = registry;
  HL_REQUIRE(changed_revision.find(key) != nullptr);
  changed_revision.find(key)->revision = Revision::from_value(3);
  HL_CHECK_ERROR(changed_revision.verify_invariants(), ErrorCode::IntegrityFailure);

  Registry changed_count = registry;
  HL_REQUIRE(changed_count.find(key) != nullptr);
  changed_count.find(key)->history_entries = 5;
  HL_CHECK_ERROR(changed_count.verify_invariants(), ErrorCode::IntegrityFailure);

  Registry changed_time = registry;
  HL_REQUIRE(changed_time.find(key) != nullptr);
  changed_time.find(key)->updated_at = LogicalTime::from_value(3);
  HL_CHECK_ERROR(changed_time.verify_invariants(), ErrorCode::IntegrityFailure);

  // An object with no chain at all is not a valid state.
  Registry orphan;
  const HardwareObject bare = make_object("asset-b", HardwareGeneration::first(), LifecycleState::Ordered,
                                          Revision::first());
  HL_REQUIRE(orphan.insert_object(bare).has_value());
  HL_CHECK_ERROR(orphan.verify_invariants(), ErrorCode::IntegrityFailure);
}

HL_TEST(domain_registry, fence_all_authority_demotes_every_object_without_touching_the_state_image) {
  Registry registry;
  const char* const assets[] = {"asset-a", "asset-b"};
  for (const char* asset : assets) {
    const HardwareObject object = make_object(asset, HardwareGeneration::first(), LifecycleState::Ordered,
                                              Revision::first());
    HL_CHECK(registry.insert_object(object).has_value());
  }
  for (const auto& entry : registry.objects()) {
    HL_CHECK_EQ(entry.second.authority, AuthorityState::Live);
  }

  // Authority is session scoped and is not part of the durable state image, so
  // fencing every object must not move the state digest.
  const Digest before = registry.state_digest();
  registry.fence_all_authority();
  for (const auto& entry : registry.objects()) {
    HL_CHECK_EQ(entry.second.authority, AuthorityState::Recovered);
  }
  HL_CHECK(registry.state_digest() == before);
  HL_CHECK_EQ(registry.object_count(), static_cast<std::size_t>(2));
}