// Hardware Lifecycle - the authoritative in-memory state.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every mutation here is checked before it is accepted, and every derived value
// is produced from a canonical ordering, never from hash order or insertion
// order. A registry is copied whole to give one mutation a transactional
// rollback point, which is why the copy constructor is part of the contract.

#include "hardware_lifecycle/registry.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "hardware_lifecycle/text.hpp"
#include "serialization.hpp"

namespace hardware_lifecycle {
namespace {

[[nodiscard]] std::string key_text(const ObjectKey& key) { return to_string(key); }

}  // namespace

Registry::Registry() = default;

Registry::Registry(const RegistryLimits& limits) : limits_(limits) {}

Result<void> Registry::insert_object(const HardwareObject& object) {
  if (!object.key.asset.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "the object key carries no asset id");
  }
  if (!object.key.hardware_generation.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "the object key carries no hardware generation")
        .with("asset", object.key.asset.value());
  }
  if (!object.revision.valid() || !object.lifecycle_generation.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "an object must carry a revision and a lifecycle generation")
        .with("asset", object.key.asset.value());
  }
  if (objects_.size() >= limits_.max_objects) {
    return Error::make(ErrorCode::LimitExceeded, "the registry holds as many objects as it may")
        .with("maximum", std::to_string(limits_.max_objects));
  }
  if (objects_.find(object.key) != objects_.end()) {
    return Error::make(ErrorCode::ObjectExists, "an object with this key already exists")
        .with("key", key_text(object.key));
  }
  if (requires_location(object.state) && !object.location.has_value()) {
    return Error::make(ErrorCode::MissingLocation, "the object state requires a location and none was supplied")
        .with("state", std::string(to_string(object.state)));
  }
  objects_.emplace(object.key, object);
  return ok();
}

HardwareObject* Registry::find(const ObjectKey& key) noexcept {
  const auto position = objects_.find(key);
  return position == objects_.end() ? nullptr : &position->second;
}

const HardwareObject* Registry::find(const ObjectKey& key) const noexcept {
  const auto position = objects_.find(key);
  return position == objects_.end() ? nullptr : &position->second;
}

bool Registry::contains(const ObjectKey& key) const noexcept { return objects_.find(key) != objects_.end(); }

Result<void> Registry::append_history(HistoryEntry entry) {
  if (!entry.commit_sequence.valid() || !entry.logical_time.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "a history entry needs a commit sequence and a logical time");
  }
  if (!entry.key.asset.valid() || !entry.key.hardware_generation.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "a history entry needs a valid object key");
  }
  if (!entry.lifecycle_generation.valid() || !entry.revision_after.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "a history entry needs a lifecycle generation and a revision");
  }
  if (objects_.find(entry.key) == objects_.end()) {
    return Error::make(ErrorCode::UnknownAsset, "history cannot be appended for an object that does not exist")
        .with("key", key_text(entry.key));
  }
  HistoryLog& log = histories_[entry.key];
  if (log.entries.size() >= limits_.max_history_entries_per_object) {
    return Error::make(ErrorCode::LimitExceeded, "the object holds as many history entries as it may")
        .with("maximum", std::to_string(limits_.max_history_entries_per_object));
  }

  if (log.entries.empty()) {
    if (!entry.is_registration()) {
      return Error::make(ErrorCode::IntegrityFailure, "a history chain must open with a registration entry")
          .with("key", key_text(entry.key));
    }
    if (entry.reason != TransitionReason::ObjectRegistered) {
      return Error::make(ErrorCode::IntegrityFailure, "a registration entry must carry the registration reason")
          .with("key", key_text(entry.key));
    }
    if (entry.revision_before.valid()) {
      return Error::make(ErrorCode::IntegrityFailure, "a registration entry cannot carry a previous revision")
          .with("key", key_text(entry.key));
    }
    if (entry.revision_after.value() != 1ull) {
      return Error::make(ErrorCode::IntegrityFailure, "a registration entry must establish revision 1")
          .with("key", key_text(entry.key));
    }
  } else {
    const HistoryEntry& previous = log.entries.back();
    if (entry.is_registration()) {
      return Error::make(ErrorCode::IntegrityFailure, "only the first history entry may be a registration entry")
          .with("key", key_text(entry.key));
    }
    if (entry.from != previous.to) {
      return Error::make(ErrorCode::IntegrityFailure, "a history entry must continue from the previous target state")
          .with("key", key_text(entry.key))
          .with("expected", std::string(to_string(previous.to)))
          .with("actual", std::string(to_string(entry.from)));
    }
    if (entry.revision_before != previous.revision_after) {
      return Error::make(ErrorCode::IntegrityFailure, "a history entry must continue from the previous revision")
          .with("key", key_text(entry.key));
    }
    if (entry.revision_after.value() != entry.revision_before.value() + 1ull) {
      return Error::make(ErrorCode::IntegrityFailure, "a history entry must advance the revision by exactly one")
          .with("key", key_text(entry.key));
    }
    // A gate decision carries from equal to to and leaves the lifecycle
    // generation alone; every state changing entry advances it by exactly one.
    const std::uint64_t expected_generation = entry.changes_state()
                                                  ? previous.lifecycle_generation.value() + 1ull
                                                  : previous.lifecycle_generation.value();
    if (entry.lifecycle_generation.value() != expected_generation) {
      return Error::make(ErrorCode::IntegrityFailure,
                         "a history entry must carry the lifecycle generation its own kind implies")
          .with("key", key_text(entry.key))
          .with("expected", std::to_string(expected_generation))
          .with("recorded", std::to_string(entry.lifecycle_generation.value()));
    }
    if (!(previous.commit_sequence < entry.commit_sequence)) {
      return Error::make(ErrorCode::IntegrityFailure, "a history entry must advance the commit sequence")
          .with("key", key_text(entry.key));
    }
    if (!(previous.logical_time < entry.logical_time)) {
      return Error::make(ErrorCode::IntegrityFailure, "a history entry must advance the logical time")
          .with("key", key_text(entry.key));
    }
  }

  const Digest expected = compute_entry_chain_digest(log.chain_head, entry);
  if (entry.chain_digest != expected) {
    return Error::make(ErrorCode::IntegrityFailure, "the history chain digest does not match the entry content")
        .with("key", key_text(entry.key));
  }

  log.chain_head = entry.chain_digest;
  log.entries.push_back(std::move(entry));

  HardwareObject* object = find(log.entries.back().key);
  if (object != nullptr) {
    object->history_entries = log.entries.size();
  }
  return ok();
}

const HistoryLog* Registry::history(const ObjectKey& key) const noexcept {
  const auto position = histories_.find(key);
  return position == histories_.end() ? nullptr : &position->second;
}

std::size_t Registry::history_entry_count() const noexcept {
  std::size_t total = 0;
  for (const auto& entry : histories_) {
    total += entry.second.entries.size();
  }
  return total;
}

const HealthSnapshot* Registry::health(const ObjectKey& key) const noexcept {
  const auto position = health_.find(key);
  return position == health_.end() ? nullptr : &position->second;
}

Result<void> Registry::set_health(HealthSnapshot snapshot) {
  if (objects_.find(snapshot.observation.key) == objects_.end()) {
    return Error::make(ErrorCode::UnknownAsset, "a health observation must refer to an existing object")
        .with("key", key_text(snapshot.observation.key));
  }
  if (snapshot.present) {
    if (!snapshot.sequence.valid() || !snapshot.recorded_at.valid()) {
      return Error::make(ErrorCode::MalformedRequest, "a recorded observation needs a sequence and a logical time");
    }
  } else {
    return Error::make(ErrorCode::MalformedRequest, "an absent observation is not stored; omit it instead");
  }
  health_[snapshot.observation.key] = std::move(snapshot);
  return ok();
}

Result<void> Registry::record_plan(const AppliedPlan& plan) {
  if (!plan.key.plan.valid() || !plan.key.attempt.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "an applied plan needs a plan id and an attempt id");
  }
  if (!plan.commit_sequence.valid() || !plan.logical_time.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "an applied plan needs a commit sequence and a logical time");
  }
  if (plans_.size() >= limits_.max_applied_plans) {
    return Error::make(ErrorCode::LimitExceeded, "the applied plan ledger is full")
        .with("maximum", std::to_string(limits_.max_applied_plans));
  }
  if (plans_.find(plan.key) != plans_.end()) {
    return Error::make(ErrorCode::PlanConflict, "this plan and attempt were already applied")
        .with("plan", plan.key.plan.value())
        .with("attempt", plan.key.attempt.value());
  }
  if (compute_receipt_digest(plan.receipt) != std::visit([](const auto& typed) { return typed.receipt_digest; },
                                                         plan.receipt)) {
    return Error::make(ErrorCode::IntegrityFailure, "the ledger receipt does not match its own digest");
  }
  plans_.emplace(plan.key, plan);
  return ok();
}

const AppliedPlan* Registry::find_plan(const PlanId& plan, const AttemptId& attempt) const noexcept {
  PlanKey key;
  key.plan = plan;
  key.attempt = attempt;
  const auto position = plans_.find(key);
  return position == plans_.end() ? nullptr : &position->second;
}

Result<void> Registry::set_watermarks(CommitSequence commit_sequence, LogicalTime logical_time,
                                      ObservationSequence observation_sequence) {
  if (commit_sequence < commit_sequence_ || logical_time < logical_time_ ||
      observation_sequence < observation_sequence_) {
    return Error::make(ErrorCode::IntegrityFailure, "authoritative watermarks must never move backwards")
        .with("commit_sequence", std::to_string(commit_sequence_.value()))
        .with("requested_commit_sequence", std::to_string(commit_sequence.value()));
  }
  commit_sequence_ = commit_sequence;
  logical_time_ = logical_time;
  observation_sequence_ = observation_sequence;
  return ok();
}

Result<ObservationSequence> Registry::next_observation_sequence() const {
  if (!observation_sequence_.valid()) {
    return ObservationSequence::first();
  }
  return observation_sequence_.next();
}

Digest Registry::state_digest() const {
  std::vector<HardwareObject> objects;
  objects.reserve(objects_.size());
  for (const auto& entry : objects_) {
    objects.push_back(entry.second);
  }

  std::vector<HistoryEntry> history;
  for (const auto& entry : histories_) {
    for (const HistoryEntry& item : entry.second.entries) {
      history.push_back(item);
    }
  }

  std::vector<ReplacementRecord> lineage;
  lineage.reserve(lineage_.size());
  for (const auto& entry : lineage_.records()) {
    lineage.push_back(entry.second);
  }

  std::vector<AppliedPlan> plans;
  plans.reserve(plans_.size());
  for (const auto& entry : plans_) {
    plans.push_back(entry.second);
  }

  std::vector<HealthSnapshot> health;
  health.reserve(health_.size());
  for (const auto& entry : health_) {
    health.push_back(entry.second);
  }

  const std::vector<std::uint8_t> image =
      detail::canonical_state_bytes(commit_sequence_, logical_time_, observation_sequence_, objects, history, lineage,
                                    plans, health);
  return Sha256::hash(image.data(), image.size());
}

void Registry::fence_all_authority() noexcept {
  for (auto& entry : objects_) {
    entry.second.authority = AuthorityState::Recovered;
  }
}

Result<void> Registry::verify_invariants() const {
  for (const auto& entry : objects_) {
    const ObjectKey& key = entry.first;
    const HardwareObject& object = entry.second;

    const HistoryLog* log = history(key);
    if (log == nullptr || log->entries.empty()) {
      return Error::make(ErrorCode::IntegrityFailure, "an object without a history chain is not a valid state")
          .with("key", key_text(key));
    }

    const Result<Digest> head = verify_history_chain(*log);
    if (!head.has_value()) {
      return head.error();
    }
    if (head.value() != log->chain_head) {
      return Error::make(ErrorCode::IntegrityFailure, "the recorded chain head is not the chain head of the log")
          .with("key", key_text(key));
    }

    const Result<void> consistent = verify_history_against_object(*log, object);
    if (!consistent.has_value()) {
      return consistent.error();
    }

    if (object.history_entries != log->entries.size()) {
      return Error::make(ErrorCode::IntegrityFailure, "the object's history entry count disagrees with its chain")
          .with("key", key_text(key));
    }
    if (log->entries.back().commit_sequence > commit_sequence_) {
      return Error::make(ErrorCode::IntegrityFailure, "a history entry is ahead of the published commit sequence")
          .with("key", key_text(key));
    }
    if (object.updated_at > logical_time_) {
      return Error::make(ErrorCode::IntegrityFailure, "an object was updated after the authoritative logical time")
          .with("key", key_text(key));
    }
    if (object.successor.has_value()) {
      const ReplacementRecord* link = lineage_.successor_of(key);
      if (link == nullptr || !(link->successor == object.successor.value())) {
        return Error::make(ErrorCode::IntegrityFailure, "an object names a successor with no matching link")
            .with("key", key_text(key));
      }
    }
    if (object.predecessor.has_value()) {
      const ReplacementRecord* link = lineage_.predecessor_of(key);
      if (link == nullptr || !(link->predecessor == object.predecessor.value())) {
        return Error::make(ErrorCode::IntegrityFailure, "an object names a predecessor with no matching link")
            .with("key", key_text(key));
      }
    }
  }

  for (const auto& entry : lineage_.records()) {
    const ReplacementRecord& record = entry.second;
    if (compute_link_digest(record) != record.link_digest) {
      return Error::make(ErrorCode::IntegrityFailure, "a replacement link digest does not match its content")
          .with("predecessor", key_text(record.predecessor));
    }
    if (objects_.find(record.predecessor) == objects_.end() ||
        objects_.find(record.successor) == objects_.end()) {
      return Error::make(ErrorCode::IntegrityFailure, "a replacement link names an object that does not exist")
          .with("predecessor", key_text(record.predecessor));
    }
  }

  for (const auto& entry : health_) {
    if (objects_.find(entry.first) == objects_.end()) {
      return Error::make(ErrorCode::IntegrityFailure, "a health observation refers to an object that does not exist")
          .with("key", key_text(entry.first));
    }
    if (entry.second.present && entry.second.recorded_at > logical_time_) {
      return Error::make(ErrorCode::IntegrityFailure, "an observation is recorded after the authoritative logical time")
          .with("key", key_text(entry.first));
    }
    if (entry.second.sequence > observation_sequence_) {
      return Error::make(ErrorCode::IntegrityFailure,
                         "an observation sequence is ahead of the published observation sequence")
          .with("key", key_text(entry.first));
    }
  }

  return ok();
}

}  // namespace hardware_lifecycle
