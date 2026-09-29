// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Structural comparison of two exported snapshots.
//
// The diff compares the authoritative state each document declares, by object
// key in canonical order, and never mutates anything: a key that appears on one
// side only is reported as added or removed, never as present but unchanged.
// Two documents are identical only when no object differs and the two state
// digests agree, so a difference the object comparison does not model (a
// watermark, a plan ledger entry, a health observation) still shows up as a
// difference instead of being silently ignored.
//
// The diff does not read the event logs: the state digest is about state and the
// log digest is about the log, and comparing two logs is an import question, not
// a state question.

#include "hardware_lifecycle/diff.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/model.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "json.hpp"

namespace hardware_lifecycle {
namespace {

using detail::JsonValue;

/// The widest change kind spelling, used to keep the change lines aligned.
inline constexpr std::size_t kChangeKindWidth = 16;

[[nodiscard]] JsonValue render_text(std::string_view text) { return JsonValue::make_string(std::string(text)); }

[[nodiscard]] JsonValue render_digest(const Digest& digest) { return render_text(digest.hex()); }

/// A counter as a number, or null when it is not set. The same rule the export
/// document uses: a counter is never written as zero.
template <class Counter>
[[nodiscard]] JsonValue render_counter(const Counter& counter) {
  if (!counter.valid()) {
    return JsonValue::make_null();
  }
  return JsonValue::make_number(counter.value());
}

/// A counter as text; an unset counter is "-" so that a reader can tell it apart
/// from a real zero.
template <class Counter>
[[nodiscard]] std::string counter_text(const Counter& counter) {
  if (!counter.valid()) {
    return "-";
  }
  return std::to_string(counter.value());
}

[[nodiscard]] std::string padded(std::string text, std::size_t width) {
  if (text.size() < width) {
    text.append(width - text.size(), ' ');
  }
  return text;
}

[[nodiscard]] JsonValue render_key(const ObjectKey& key) {
  JsonValue object = JsonValue::make_object();
  object.emplace("asset", render_text(key.asset.value()));
  object.emplace("hardware_generation", render_counter(key.hardware_generation));
  return object;
}

/// Scope spellings of one authority mask, in declaration order, exactly as the
/// export document renders them.
[[nodiscard]] JsonValue render_authority(AuthorityMask mask) {
  JsonValue array = JsonValue::make_array();
  for (std::size_t index = 0; index < kAuthorityScopeCount; ++index) {
    const AuthorityScope scope = static_cast<AuthorityScope>(index);
    if (!mask.intersects(authority_bit(scope))) {
      continue;
    }
    array.push_back(render_text(to_string(authority_bit(scope))));
  }
  return array;
}

[[nodiscard]] JsonValue render_evidence(const EvidenceRef& evidence) {
  JsonValue object = JsonValue::make_object();
  object.emplace("kind", render_text(to_string(evidence.kind)));
  object.emplace("digest", render_digest(evidence.digest));
  object.emplace("source", render_text(evidence.source));
  object.emplace("observed_sequence", render_counter(evidence.observed_sequence));
  return object;
}

[[nodiscard]] JsonValue render_provenance(const Provenance& provenance) {
  JsonValue actor = JsonValue::make_object();
  actor.emplace("id", render_text(provenance.actor.id.value()));
  actor.emplace("kind", render_text(to_string(provenance.actor.kind)));

  JsonValue evidence = JsonValue::make_array();
  for (const EvidenceRef& reference : provenance.evidence) {
    evidence.push_back(render_evidence(reference));
  }

  JsonValue object = JsonValue::make_object();
  object.emplace("actor", std::move(actor));
  object.emplace("authority", render_authority(provenance.authority));
  object.emplace("policy_generation", render_counter(provenance.policy_generation));
  object.emplace("plan", render_text(provenance.plan.value()));
  object.emplace("attempt", render_text(provenance.attempt.value()));
  object.emplace("evidence", std::move(evidence));
  object.emplace("logical_time", render_counter(provenance.logical_time));
  object.emplace("wall_clock",
                 provenance.wall_clock.has_value() ? render_text(provenance.wall_clock.value()) : JsonValue::make_null());
  return object;
}

[[nodiscard]] JsonValue render_lineage(const ReplacementRecord& record) {
  JsonValue value = JsonValue::make_object();
  value.emplace("predecessor", render_key(record.predecessor));
  value.emplace("successor", render_key(record.successor));
  value.emplace("reason", render_text(to_string(record.reason)));
  value.emplace("provenance", render_provenance(record.provenance));
  value.emplace("linked_at", render_counter(record.linked_at));
  value.emplace("commit_sequence", render_counter(record.commit_sequence));
  value.emplace("replacement_generation", render_counter(record.replacement_generation));
  value.emplace("link_digest", render_digest(record.link_digest));
  return value;
}

/// The fields that make two objects the same object as far as a state diff is
/// concerned. The state digest covers everything; this list is what a change is
/// classified by.
[[nodiscard]] bool states_match(const HardwareObject& left, const HardwareObject& right) {
  return left.state == right.state && left.revision == right.revision &&
         left.lifecycle_generation == right.lifecycle_generation && left.eligibility_gate == right.eligibility_gate &&
         left.location == right.location && left.model == right.model && left.kind == right.kind;
}

[[nodiscard]] ObjectChangeKind classify(const HardwareObject& left, const HardwareObject& right) {
  if (left.state != right.state) {
    return ObjectChangeKind::StateChanged;
  }
  if (left.eligibility_gate != right.eligibility_gate) {
    return ObjectChangeKind::GateChanged;
  }
  return ObjectChangeKind::MetadataChanged;
}

/// History entries of a snapshot, counted per object. A snapshot that carries no
/// history reports zero for every object, which is what it can prove.
[[nodiscard]] std::map<ObjectKey, std::size_t> history_counts(const std::vector<HistoryEntry>& history) {
  std::map<ObjectKey, std::size_t> counts;
  for (const HistoryEntry& entry : history) {
    ++counts[entry.key];
  }
  return counts;
}

[[nodiscard]] std::size_t count_for(const std::map<ObjectKey, std::size_t>& counts, const ObjectKey& key) {
  const auto found = counts.find(key);
  return found == counts.end() ? 0 : found->second;
}

}  // namespace

std::string_view to_string(ObjectChangeKind kind) noexcept {
  switch (kind) {
    case ObjectChangeKind::Added:
      return "added";
    case ObjectChangeKind::Removed:
      return "removed";
    case ObjectChangeKind::StateChanged:
      return "state_changed";
    case ObjectChangeKind::MetadataChanged:
      return "metadata_changed";
    case ObjectChangeKind::LineageChanged:
      return "lineage_changed";
    case ObjectChangeKind::GateChanged:
      return "gate_changed";
  }
  return "unknown";
}

Result<SnapshotDiff> diff_snapshots(const Snapshot& before, const Snapshot& after) {
  // A snapshot too large to export is too large to compare: the collection is
  // rejected as a whole, before any element is examined.
  const Result<void> before_bound =
      limits::check_limit("before.objects", before.objects.size(), limits::kMaxSnapshotObjects);
  if (!before_bound.has_value()) {
    return before_bound.error();
  }
  const Result<void> after_bound =
      limits::check_limit("after.objects", after.objects.size(), limits::kMaxSnapshotObjects);
  if (!after_bound.has_value()) {
    return after_bound.error();
  }

  SnapshotDiff diff;
  diff.digest_before = before.header.state_digest;
  diff.digest_after = after.header.state_digest;
  diff.commit_before = before.header.commit_sequence;
  diff.commit_after = after.header.commit_sequence;

  // Indexing is by key, so the comparison never depends on the order in which
  // either document listed its objects. Should a snapshot carry one key twice,
  // the first occurrence is the one that counts, for both sides alike.
  std::map<ObjectKey, const HardwareObject*> before_objects;
  std::map<ObjectKey, const HardwareObject*> after_objects;
  std::set<ObjectKey> keys;
  for (const HardwareObject& object : before.objects) {
    (void)before_objects.emplace(object.key, &object);
    keys.insert(object.key);
  }
  for (const HardwareObject& object : after.objects) {
    (void)after_objects.emplace(object.key, &object);
    keys.insert(object.key);
  }

  const std::map<ObjectKey, std::size_t> before_history = history_counts(before.history);
  const std::map<ObjectKey, std::size_t> after_history = history_counts(after.history);

  for (const ObjectKey& key : keys) {
    const auto left = before_objects.find(key);
    const auto right = after_objects.find(key);
    const bool present_before = left != before_objects.end();
    const bool present_after = right != after_objects.end();

    ObjectChange change;
    change.key = key;
    change.present_before = present_before;
    change.present_after = present_after;
    change.history_before = count_for(before_history, key);
    change.history_after = count_for(after_history, key);

    if (present_before && present_after) {
      const HardwareObject& first = *left->second;
      const HardwareObject& second = *right->second;
      if (states_match(first, second)) {
        ++diff.unchanged;
        continue;
      }
      change.kind = classify(first, second);
      change.state_before = first.state;
      change.state_after = second.state;
      change.revision_before = first.revision;
      change.revision_after = second.revision;
      change.generation_before = first.lifecycle_generation;
      change.generation_after = second.lifecycle_generation;
      ++diff.changed;
      diff.changes.push_back(change);
      continue;
    }

    if (present_after) {
      // Added. The side that does not exist carries no state and no revision, so
      // both state fields describe the object that does exist and the missing
      // revision stays unset rather than becoming a zero.
      const HardwareObject& object = *right->second;
      change.kind = ObjectChangeKind::Added;
      change.state_before = object.state;
      change.state_after = object.state;
      change.revision_before = Revision();
      change.revision_after = object.revision;
      change.generation_before = object.lifecycle_generation;
      change.generation_after = object.lifecycle_generation;
      ++diff.added;
      diff.changes.push_back(change);
      continue;
    }

    // Removed: symmetric with Added.
    const HardwareObject& object = *left->second;
    change.kind = ObjectChangeKind::Removed;
    change.state_before = object.state;
    change.state_after = object.state;
    change.revision_before = object.revision;
    change.revision_after = Revision();
    change.generation_before = object.lifecycle_generation;
    change.generation_after = object.lifecycle_generation;
    ++diff.removed;
    diff.changes.push_back(change);
  }

  // A lineage link is new when its predecessor had no link before. Sorting the
  // candidate records by predecessor keeps the list canonical even for a
  // snapshot that was assembled by hand.
  std::set<ObjectKey> before_predecessors;
  for (const ReplacementRecord& record : before.lineage) {
    before_predecessors.insert(record.predecessor);
  }
  std::vector<ReplacementRecord> candidates = after.lineage;
  std::sort(candidates.begin(), candidates.end(), [](const ReplacementRecord& left_record,
                                                     const ReplacementRecord& right_record) {
    return left_record.predecessor < right_record.predecessor;
  });
  for (const ReplacementRecord& record : candidates) {
    if (before_predecessors.count(record.predecessor) == 0) {
      diff.lineage_added.push_back(record);
    }
  }

  // Identical means exactly: nothing changed and the two states hash the same.
  // A difference the object walk does not model therefore cannot be reported as
  // identical.
  diff.identical = diff.changes.empty() && diff.digest_before == diff.digest_after;
  return diff;
}

std::string to_json(const SnapshotDiff& diff, bool pretty) {
  JsonValue counts = JsonValue::make_object();
  counts.emplace("added", JsonValue::make_number(static_cast<std::uint64_t>(diff.added)));
  counts.emplace("removed", JsonValue::make_number(static_cast<std::uint64_t>(diff.removed)));
  counts.emplace("changed", JsonValue::make_number(static_cast<std::uint64_t>(diff.changed)));
  counts.emplace("unchanged", JsonValue::make_number(static_cast<std::uint64_t>(diff.unchanged)));

  JsonValue changes = JsonValue::make_array();
  for (const ObjectChange& change : diff.changes) {
    JsonValue value = JsonValue::make_object();
    value.emplace("asset", render_text(change.key.asset.value()));
    value.emplace("hardware_generation", render_counter(change.key.hardware_generation));
    value.emplace("kind", render_text(to_string(change.kind)));
    value.emplace("present_before", JsonValue::make_bool(change.present_before));
    value.emplace("present_after", JsonValue::make_bool(change.present_after));
    value.emplace("state_before", render_text(to_string(change.state_before)));
    value.emplace("state_after", render_text(to_string(change.state_after)));
    value.emplace("revision_before", render_counter(change.revision_before));
    value.emplace("revision_after", render_counter(change.revision_after));
    value.emplace("generation_before", render_counter(change.generation_before));
    value.emplace("generation_after", render_counter(change.generation_after));
    value.emplace("history_before", JsonValue::make_number(static_cast<std::uint64_t>(change.history_before)));
    value.emplace("history_after", JsonValue::make_number(static_cast<std::uint64_t>(change.history_after)));
    changes.push_back(std::move(value));
  }

  JsonValue lineage = JsonValue::make_array();
  for (const ReplacementRecord& record : diff.lineage_added) {
    lineage.push_back(render_lineage(record));
  }

  JsonValue document = JsonValue::make_object();
  document.emplace("format", render_text("hardware-lifecycle-diff"));
  document.emplace("identical", JsonValue::make_bool(diff.identical));
  document.emplace("state_digest_before", render_digest(diff.digest_before));
  document.emplace("state_digest_after", render_digest(diff.digest_after));
  document.emplace("commit_sequence_before", render_counter(diff.commit_before));
  document.emplace("commit_sequence_after", render_counter(diff.commit_after));
  document.emplace("counts", std::move(counts));
  document.emplace("changes", std::move(changes));
  document.emplace("lineage_added", std::move(lineage));
  return detail::write_json(document, pretty);
}

std::string describe(const SnapshotDiff& diff) {
  std::string text;
  text += "hardware-lifecycle snapshot diff: ";
  text += std::to_string(diff.changes.size());
  text += diff.changes.size() == 1 ? " change\n" : " changes\n";
  text += "  identical: ";
  text += diff.identical ? "true" : "false";
  text += "\n  before: commit_sequence=";
  text += counter_text(diff.commit_before);
  text += " state_digest=";
  text += diff.digest_before.hex();
  text += "\n  after:  commit_sequence=";
  text += counter_text(diff.commit_after);
  text += " state_digest=";
  text += diff.digest_after.hex();
  text += "\n  counts: added=";
  text += std::to_string(diff.added);
  text += " removed=";
  text += std::to_string(diff.removed);
  text += " changed=";
  text += std::to_string(diff.changed);
  text += " unchanged=";
  text += std::to_string(diff.unchanged);
  text += "\n  lineage added: ";
  text += std::to_string(diff.lineage_added.size());
  text += "\n";

  for (const ObjectChange& change : diff.changes) {
    text += "  ";
    text += padded(std::string(to_string(change.kind)), kChangeKindWidth);
    text += " ";
    text += to_string(change.key);
    text += " present=";
    text += change.present_before ? "true" : "false";
    text += "->";
    text += change.present_after ? "true" : "false";
    text += " state=";
    text += to_string(change.state_before);
    text += "->";
    text += to_string(change.state_after);
    text += " revision=";
    text += counter_text(change.revision_before);
    text += "->";
    text += counter_text(change.revision_after);
    text += " generation=";
    text += counter_text(change.generation_before);
    text += "->";
    text += counter_text(change.generation_after);
    text += " history=";
    text += std::to_string(change.history_before);
    text += "->";
    text += std::to_string(change.history_after);
    text += "\n";
  }
  return text;
}

}  // namespace hardware_lifecycle
