// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The canonical JSON export document and its strict decode.
//
// The document carries the authoritative durable event log verbatim, and the
// objects, history, lineage, plans and health arrays are derived views of the
// state those events produced. Import is therefore provable rather than
// approximate: the importer decodes each event payload strictly, replays it
// through the ordinary commit path and refuses the document unless the state it
// produces hashes to the digest the document declares. The derived views are
// cross checked against the replayed state by the importer; this reader only
// requires that each of them is internally well formed and names objects of the
// document.
//
// The state digest covers the state, never the log: compute_snapshot_state_digest
// hashes exactly detail::canonical_state_bytes over the derived arrays, so two
// documents that describe the same state have the same state digest whether or
// not they can be replayed. The log has its own digest, computed by the store.
//
// Two things are deliberately absent because neither is a durable fact: session
// scoped authority, which an import re-establishes by attestation, and health
// freshness, which is derived from the authoritative logical clock at read time.
//
// Writing is faithful and reading is strict. Every field of a snapshot has a
// rendering, so to_json() never drops, repairs or invents a value; from_json()
// is where the document is held to the format, and it rejects a document instead
// of defaulting any part of it. Omitted sections are written as empty arrays and
// unset counters as null, so no reader has to distinguish an absent member from
// an empty one. Top level member order is fixed as: format, format_version,
// library_version, state_digest, commit_sequence, logical_time,
// observation_sequence, control_epoch, incarnation, counts, events, event_count,
// importable, objects, history, lineage, plans, health.

#include "hardware_lifecycle/export.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "hardware_lifecycle/compatibility.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/lifecycle.hpp"
#include "hardware_lifecycle/model.hpp"
#include "hardware_lifecycle/text.hpp"
#include "hardware_lifecycle/version.hpp"
#include "json.hpp"
#include "serialization.hpp"

namespace hardware_lifecycle {
namespace {

using detail::JsonValue;

/// The one format tag this build writes and accepts.
inline constexpr std::string_view kDocumentFormat = "hardware-lifecycle-snapshot";

/// The six durable record kinds, in declaration order. The plan ledger's kind
/// member and every event kind are spelled from this list, which is the same
/// source the writer spells them from, so the two can never disagree. The kinds
/// have no textual parser of their own in the public interface.
inline constexpr std::array<RecordKind, 6> kRecordKinds = {
    RecordKind::ObjectRegistered, RecordKind::TransitionApplied, RecordKind::SuccessorLinked,
    RecordKind::EligibilityGateChanged, RecordKind::HealthObserved, RecordKind::AuthorityAttested};

/// Positions of the collections in the observed and declared count arrays. The
/// two arrays are indexed by these so that a count can never be compared with
/// the wrong collection.
inline constexpr std::size_t kObjects = 0;
inline constexpr std::size_t kHistory = 1;
inline constexpr std::size_t kLineage = 2;
inline constexpr std::size_t kPlans = 3;
inline constexpr std::size_t kHealth = 4;
inline constexpr std::size_t kEvents = 5;
inline constexpr std::size_t kCollections = 6;

// ---------------------------------------------------------------------------
// JSON paths
// ---------------------------------------------------------------------------

/// Path of an object member, for diagnostics: "objects[3].location.site".
[[nodiscard]] std::string member_path(std::string_view parent, std::string_view key) {
  std::string path(parent);
  if (!path.empty()) {
    path.push_back('.');
  }
  path.append(key);
  return path;
}

/// Path of an array element.
[[nodiscard]] std::string element_path(std::string_view parent, std::size_t index) {
  std::string path(parent);
  path.push_back('[');
  path.append(std::to_string(index));
  path.push_back(']');
  return path;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

[[nodiscard]] JsonValue render_text(std::string_view text) { return JsonValue::make_string(std::string(text)); }

/// The one counter rule of the document: a real counter is a number, an unset
/// counter is null, and a counter is never rendered as zero.
template <class Counter>
[[nodiscard]] JsonValue render_counter(const Counter& counter) {
  if (!counter.valid()) {
    return JsonValue::make_null();
  }
  return JsonValue::make_number(counter.value());
}

[[nodiscard]] JsonValue render_digest(const Digest& digest) { return render_text(digest.hex()); }

[[nodiscard]] JsonValue render_key(const ObjectKey& key) {
  JsonValue object = JsonValue::make_object();
  object.emplace("asset", render_text(key.asset.value()));
  object.emplace("hardware_generation", render_counter(key.hardware_generation));
  return object;
}

[[nodiscard]] JsonValue render_optional_key(const std::optional<ObjectKey>& key) {
  if (!key.has_value()) {
    return JsonValue::make_null();
  }
  return render_key(key.value());
}

[[nodiscard]] JsonValue render_location(const std::optional<Location>& location) {
  if (!location.has_value()) {
    return JsonValue::make_null();
  }
  JsonValue object = JsonValue::make_object();
  object.emplace("site", render_text(location->site.value()));
  object.emplace("rack", render_text(location->rack.value()));
  object.emplace("slot", render_text(location->slot.value()));
  return object;
}

[[nodiscard]] JsonValue render_wall_clock(const WallClock& clock) {
  if (!clock.has_value()) {
    return JsonValue::make_null();
  }
  return render_text(clock.value());
}

/// Scope spellings of one authority mask, in declaration order. The array is
/// built from the scopes themselves, one mask bit at a time, so it stays machine
/// readable and can never disagree with the mask it renders.
[[nodiscard]] std::vector<std::string> authority_scopes(AuthorityMask mask) {
  std::vector<std::string> scopes;
  for (std::size_t index = 0; index < kAuthorityScopeCount; ++index) {
    const AuthorityScope scope = static_cast<AuthorityScope>(index);
    if (!mask.intersects(authority_bit(scope))) {
      continue;
    }
    scopes.push_back(to_string(authority_bit(scope)));
  }
  return scopes;
}

[[nodiscard]] JsonValue render_authority(AuthorityMask mask) {
  JsonValue array = JsonValue::make_array();
  for (const std::string& scope : authority_scopes(mask)) {
    array.push_back(render_text(scope));
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
  object.emplace("wall_clock", render_wall_clock(provenance.wall_clock));
  return object;
}

/// One receipt as a tagged union. The tag is the first member, so a reader sees
/// which shape follows before it decodes anything else.
[[nodiscard]] JsonValue render_receipt(const Receipt& receipt) {
  return std::visit(
      [](const auto& typed) -> JsonValue {
        using Shape = std::decay_t<decltype(typed)>;
        JsonValue object = JsonValue::make_object();
        if constexpr (std::is_same_v<Shape, CreateReceipt>) {
          object.emplace("type", render_text("create"));
          object.emplace("plan", render_text(typed.plan.value()));
          object.emplace("attempt", render_text(typed.attempt.value()));
          object.emplace("key", render_key(typed.key));
          object.emplace("state", render_text(to_string(typed.state)));
          object.emplace("lifecycle_generation", render_counter(typed.lifecycle_generation));
          object.emplace("revision", render_counter(typed.revision));
          object.emplace("commit_sequence", render_counter(typed.commit_sequence));
          object.emplace("logical_time", render_counter(typed.logical_time));
          object.emplace("receipt_digest", render_digest(typed.receipt_digest));
          object.emplace("idempotent_replay", JsonValue::make_bool(typed.idempotent_replay));
        } else if constexpr (std::is_same_v<Shape, TransitionReceipt>) {
          object.emplace("type", render_text("transition"));
          object.emplace("plan", render_text(typed.plan.value()));
          object.emplace("attempt", render_text(typed.attempt.value()));
          object.emplace("key", render_key(typed.key));
          object.emplace("from", render_text(to_string(typed.from)));
          object.emplace("to", render_text(to_string(typed.to)));
          object.emplace("reason", render_text(to_string(typed.reason)));
          object.emplace("revision_before", render_counter(typed.revision_before));
          object.emplace("revision_after", render_counter(typed.revision_after));
          object.emplace("lifecycle_generation", render_counter(typed.lifecycle_generation));
          object.emplace("gate_after", render_text(to_string(typed.gate_after)));
          object.emplace("commit_sequence", render_counter(typed.commit_sequence));
          object.emplace("logical_time", render_counter(typed.logical_time));
          object.emplace("request_digest", render_digest(typed.request_digest));
          object.emplace("entry_digest", render_digest(typed.entry_digest));
          object.emplace("receipt_digest", render_digest(typed.receipt_digest));
          object.emplace("idempotent_replay", JsonValue::make_bool(typed.idempotent_replay));
        } else if constexpr (std::is_same_v<Shape, ReplacementReceipt>) {
          object.emplace("type", render_text("replacement"));
          object.emplace("plan", render_text(typed.plan.value()));
          object.emplace("attempt", render_text(typed.attempt.value()));
          object.emplace("predecessor", render_key(typed.predecessor));
          object.emplace("successor", render_key(typed.successor));
          object.emplace("replacement_generation", render_counter(typed.replacement_generation));
          object.emplace("commit_sequence", render_counter(typed.commit_sequence));
          object.emplace("logical_time", render_counter(typed.logical_time));
          object.emplace("link_digest", render_digest(typed.link_digest));
          object.emplace("request_digest", render_digest(typed.request_digest));
          object.emplace("receipt_digest", render_digest(typed.receipt_digest));
          object.emplace("idempotent_replay", JsonValue::make_bool(typed.idempotent_replay));
        } else if constexpr (std::is_same_v<Shape, EligibilityGateReceipt>) {
          object.emplace("type", render_text("gate"));
          object.emplace("plan", render_text(typed.plan.value()));
          object.emplace("attempt", render_text(typed.attempt.value()));
          object.emplace("key", render_key(typed.key));
          object.emplace("before", render_text(to_string(typed.before)));
          object.emplace("after", render_text(to_string(typed.after)));
          object.emplace("revision", render_counter(typed.revision));
          object.emplace("commit_sequence", render_counter(typed.commit_sequence));
          object.emplace("logical_time", render_counter(typed.logical_time));
          object.emplace("request_digest", render_digest(typed.request_digest));
          object.emplace("receipt_digest", render_digest(typed.receipt_digest));
          object.emplace("idempotent_replay", JsonValue::make_bool(typed.idempotent_replay));
        } else if constexpr (std::is_same_v<Shape, HealthReceipt>) {
          object.emplace("type", render_text("health"));
          object.emplace("plan", render_text(typed.plan.value()));
          object.emplace("attempt", render_text(typed.attempt.value()));
          object.emplace("key", render_key(typed.key));
          object.emplace("sequence", render_counter(typed.sequence));
          object.emplace("commit_sequence", render_counter(typed.commit_sequence));
          object.emplace("logical_time", render_counter(typed.logical_time));
          object.emplace("request_digest", render_digest(typed.request_digest));
          object.emplace("receipt_digest", render_digest(typed.receipt_digest));
          object.emplace("idempotent_replay", JsonValue::make_bool(typed.idempotent_replay));
        } else {
          static_assert(std::is_same_v<Shape, AttestationReceipt>);
          object.emplace("type", render_text("attestation"));
          object.emplace("plan", render_text(typed.plan.value()));
          object.emplace("attempt", render_text(typed.attempt.value()));
          object.emplace("key", render_key(typed.key));
          object.emplace("revision", render_counter(typed.revision));
          object.emplace("commit_sequence", render_counter(typed.commit_sequence));
          object.emplace("logical_time", render_counter(typed.logical_time));
          object.emplace("request_digest", render_digest(typed.request_digest));
          object.emplace("receipt_digest", render_digest(typed.receipt_digest));
          object.emplace("idempotent_replay", JsonValue::make_bool(typed.idempotent_replay));
        }
        return object;
      },
      receipt);
}

[[nodiscard]] JsonValue render_object(const HardwareObject& object) {
  JsonValue value = JsonValue::make_object();
  value.emplace("asset", render_text(object.key.asset.value()));
  value.emplace("hardware_generation", render_counter(object.key.hardware_generation));
  value.emplace("kind", render_text(to_string(object.kind)));
  value.emplace("model", render_text(object.model.value()));
  value.emplace("firmware_generation", render_counter(object.firmware_generation));
  value.emplace("lifecycle_generation", render_counter(object.lifecycle_generation));
  value.emplace("state", render_text(to_string(object.state)));
  value.emplace("revision", render_counter(object.revision));
  value.emplace("location", render_location(object.location));
  value.emplace("eligibility_gate", render_text(to_string(object.eligibility_gate)));
  value.emplace("predecessor", render_optional_key(object.predecessor));
  value.emplace("successor", render_optional_key(object.successor));
  value.emplace("replacement_generation", render_counter(object.replacement_generation));
  value.emplace("created_at", render_counter(object.created_at));
  value.emplace("updated_at", render_counter(object.updated_at));
  value.emplace("history_entries", JsonValue::make_number(object.history_entries));
  return value;
}

[[nodiscard]] JsonValue render_history(const HistoryEntry& entry) {
  JsonValue value = JsonValue::make_object();
  value.emplace("commit_sequence", render_counter(entry.commit_sequence));
  value.emplace("logical_time", render_counter(entry.logical_time));
  value.emplace("asset", render_text(entry.key.asset.value()));
  value.emplace("hardware_generation", render_counter(entry.key.hardware_generation));
  value.emplace("lifecycle_generation", render_counter(entry.lifecycle_generation));
  value.emplace("revision_before", render_counter(entry.revision_before));
  value.emplace("revision_after", render_counter(entry.revision_after));
  value.emplace("from", render_text(to_string(entry.from)));
  value.emplace("to", render_text(to_string(entry.to)));
  value.emplace("reason", render_text(to_string(entry.reason)));
  value.emplace("provenance", render_provenance(entry.provenance));
  value.emplace("receipt_digest", render_digest(entry.receipt_digest));
  value.emplace("chain_digest", render_digest(entry.chain_digest));
  return value;
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

[[nodiscard]] JsonValue render_plan(const AppliedPlan& plan) {
  JsonValue value = JsonValue::make_object();
  value.emplace("plan", render_text(plan.key.plan.value()));
  value.emplace("attempt", render_text(plan.key.attempt.value()));
  value.emplace("kind", render_text(to_string(plan.kind)));
  value.emplace("request_digest", render_digest(plan.request_digest));
  value.emplace("commit_sequence", render_counter(plan.commit_sequence));
  value.emplace("logical_time", render_counter(plan.logical_time));
  value.emplace("receipt", render_receipt(plan.receipt));
  return value;
}

[[nodiscard]] JsonValue render_observation(const HealthObservation& observation) {
  JsonValue value = JsonValue::make_object();
  value.emplace("health", render_text(to_string(observation.health)));
  value.emplace("readiness", render_text(to_string(observation.readiness)));
  value.emplace("availability", render_text(to_string(observation.availability)));
  value.emplace("evidence_digest", render_digest(observation.evidence_digest));
  value.emplace("source", render_text(observation.source));
  value.emplace("observed_wall_clock", render_wall_clock(observation.observed_wall_clock));
  return value;
}

[[nodiscard]] JsonValue render_health(const HealthSnapshot& snapshot) {
  JsonValue value = JsonValue::make_object();
  value.emplace("asset", render_text(snapshot.observation.key.asset.value()));
  value.emplace("hardware_generation", render_counter(snapshot.observation.key.hardware_generation));
  value.emplace("present", JsonValue::make_bool(snapshot.present));
  // When no observation has ever been recorded, the observation content is not a
  // durable fact and is rendered as null; the sequence and the recording time
  // are still emitted and are null when they were never set.
  value.emplace("observation", snapshot.present ? render_observation(snapshot.observation) : JsonValue::make_null());
  value.emplace("sequence", render_counter(snapshot.sequence));
  value.emplace("recorded_at", render_counter(snapshot.recorded_at));
  return value;
}

/// One durable record, verbatim. The payload is opaque here: it is the domain
/// codec's bytes, carried so that an import can replay the record exactly.
[[nodiscard]] JsonValue render_event(const ExportedRecord& record) {
  JsonValue value = JsonValue::make_object();
  value.emplace("kind", render_text(to_string(record.kind)));
  value.emplace("sequence", render_counter(record.sequence));
  value.emplace("logical_time", render_counter(record.logical_time));
  value.emplace("payload", render_text(to_hex_lower(record.payload.data(), record.payload.size())));
  return value;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

/// Sticky rejection latch. The first failure wins and every later check is a
/// no-op, so a diagnostic never depends on which check happened to run second.
class Failure {
 public:
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

  bool at(ErrorCode code, std::string message, std::string_view path) {
    return raise(Error::make(code, std::move(message)), path);
  }

  bool raise(Error error, std::string_view path) {
    if (!ok_) {
      return false;
    }
    error_ = std::move(error);
    (void)error_.with("path", std::string(path));
    ok_ = false;
    return false;
  }

 private:
  bool ok_ = true;
  Error error_;
};

/// One JSON object under decode together with the members it has consumed. A
/// member that is never consumed is rejected: a field name that this reader does
/// not know must be an error, never a silently ignored extra.
class Members {
 public:
  Members(Failure& failure, const JsonValue& value, std::string_view path)
      : failure_(&failure), value_(&value), path_(path) {
    if (value.is_object()) {
      consumed_.assign(value.fields().size(), 0);
    } else {
      (void)failure_->at(ErrorCode::MalformedRequest, "expected a JSON object", path_);
    }
  }

  Members(const Members&) = delete;
  Members& operator=(const Members&) = delete;
  Members(Members&&) = delete;
  Members& operator=(Members&&) = delete;
  ~Members() = default;

  [[nodiscard]] const JsonValue* take(std::string_view key) {
    if (!failure_->ok() || !value_->is_object()) {
      return nullptr;
    }
    const std::vector<std::pair<std::string, JsonValue>>& fields = value_->fields();
    for (std::size_t index = 0; index < fields.size(); ++index) {
      if (fields[index].first == key) {
        consumed_[index] = 1;
        return &fields[index].second;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const JsonValue* need(std::string_view key) {
    const JsonValue* found = take(key);
    if (found == nullptr && failure_->ok()) {
      (void)failure_->at(ErrorCode::MalformedRequest, "missing required member", member_path(path_, key));
    }
    return found;
  }

  [[nodiscard]] bool sealed() {
    if (!failure_->ok() || !value_->is_object()) {
      return false;
    }
    const std::vector<std::pair<std::string, JsonValue>>& fields = value_->fields();
    for (std::size_t index = 0; index < fields.size(); ++index) {
      if (consumed_[index] == 0) {
        return failure_->at(ErrorCode::MalformedRequest, "unknown member", member_path(path_, fields[index].first));
      }
    }
    return true;
  }

 private:
  Failure* failure_;
  const JsonValue* value_;
  std::string path_;
  std::vector<char> consumed_;
};

[[nodiscard]] std::string_view require_string(Failure& failure, const JsonValue& value, std::string_view path) {
  if (!value.is_string()) {
    (void)failure.at(ErrorCode::MalformedRequest, "expected a JSON string", path);
    return {};
  }
  return value.as_string();
}

[[nodiscard]] std::uint64_t require_number(Failure& failure, const JsonValue& value, std::string_view path) {
  if (!value.is_number()) {
    (void)failure.at(ErrorCode::MalformedRequest, "expected a JSON number", path);
    return 0;
  }
  return value.as_number();
}

[[nodiscard]] bool require_bool(Failure& failure, const JsonValue& value, std::string_view path) {
  if (!value.is_bool()) {
    (void)failure.at(ErrorCode::MalformedRequest, "expected a JSON boolean", path);
    return false;
  }
  return value.as_bool();
}

/// A counter is a number of at least one, or null for "not set". A zero is
/// rejected: the document has exactly one spelling for an unset counter.
template <class Counter>
[[nodiscard]] Counter decode_counter(Failure& failure, const JsonValue& value, std::string_view path) {
  if (value.is_null()) {
    return Counter();
  }
  if (!value.is_number()) {
    (void)failure.at(ErrorCode::MalformedRequest, "expected a number or null", path);
    return Counter();
  }
  const std::uint64_t raw = value.as_number();
  if (raw == 0) {
    (void)failure.at(ErrorCode::MalformedRequest, "a counter is at least 1; an unset counter is null", path);
    return Counter();
  }
  return Counter::from_value(raw);
}

template <class Id>
[[nodiscard]] Id decode_id(Failure& failure, const JsonValue& value, std::string_view path) {
  const std::string_view text = require_string(failure, value, path);
  if (!failure.ok()) {
    return Id();
  }
  const Result<Id> parsed = Id::parse(text);
  if (!parsed.has_value()) {
    (void)failure.raise(parsed.error(), path);
    return Id();
  }
  return parsed.value();
}

[[nodiscard]] Digest decode_digest(Failure& failure, const JsonValue& value, std::string_view path) {
  const std::string_view text = require_string(failure, value, path);
  if (!failure.ok()) {
    return Digest();
  }
  const Result<Digest> parsed = Digest::from_hex(text);
  if (!parsed.has_value()) {
    (void)failure.raise(parsed.error(), path);
    return Digest();
  }
  return parsed.value();
}

template <class Enum>
using EnumParse = Result<Enum> (*)(std::string_view);

template <class Enum>
[[nodiscard]] Enum decode_enum(Failure& failure, const JsonValue& value, std::string_view path, EnumParse<Enum> parse) {
  const std::string_view text = require_string(failure, value, path);
  if (!failure.ok()) {
    return Enum{};
  }
  const Result<Enum> parsed = parse(text);
  if (!parsed.has_value()) {
    (void)failure.raise(parsed.error(), path);
    return Enum{};
  }
  return parsed.value();
}

/// The record kind of one applied plan or one event. The numeric parser is the
/// wrong shape here, so the spelling is resolved against the canonical rendering
/// of each kind, which is the same source the writer spells it from.
[[nodiscard]] RecordKind decode_record_kind(Failure& failure, const JsonValue& value, std::string_view path) {
  const std::string_view text = require_string(failure, value, path);
  if (!failure.ok()) {
    return RecordKind::ObjectRegistered;
  }
  for (const RecordKind candidate : kRecordKinds) {
    if (to_string(candidate) == text) {
      return candidate;
    }
  }
  (void)failure.at(ErrorCode::MalformedRequest, "unknown record kind spelling", path);
  return RecordKind::ObjectRegistered;
}

/// A record payload, carried as lowercase hexadecimal. An odd length, a byte
/// that is not hexadecimal and a payload above the record bound are all
/// rejected; nothing is truncated.
[[nodiscard]] std::vector<std::uint8_t> decode_payload(Failure& failure, const JsonValue& value,
                                                       std::string_view path) {
  const std::string_view text = require_string(failure, value, path);
  if (!failure.ok()) {
    return {};
  }
  if (text.empty()) {
    (void)failure.at(ErrorCode::MalformedRequest, "a record payload must not be empty", path);
    return {};
  }
  const Result<std::vector<std::uint8_t>> decoded = from_hex(text, limits::kMaxRecordPayloadBytes);
  if (!decoded.has_value()) {
    (void)failure.raise(decoded.error(), path);
    return {};
  }
  return decoded.value();
}

[[nodiscard]] ExportedRecord decode_event(Failure& failure, const JsonValue& value, std::string_view path) {
  Members members(failure, value, path);
  ExportedRecord record;
  if (const JsonValue* kind = members.need("kind"); kind != nullptr) {
    record.kind = decode_record_kind(failure, *kind, member_path(path, "kind"));
  }
  if (const JsonValue* sequence = members.need("sequence"); sequence != nullptr) {
    record.sequence = decode_counter<CommitSequence>(failure, *sequence, member_path(path, "sequence"));
  }
  if (const JsonValue* logical_time = members.need("logical_time"); logical_time != nullptr) {
    record.logical_time = decode_counter<LogicalTime>(failure, *logical_time, member_path(path, "logical_time"));
  }
  if (const JsonValue* payload = members.need("payload"); payload != nullptr) {
    record.payload = decode_payload(failure, *payload, member_path(path, "payload"));
  }
  (void)members.sealed();
  if (failure.ok() && (!record.sequence.valid() || !record.logical_time.valid())) {
    (void)failure.at(ErrorCode::MalformedRequest, "an event needs a real commit sequence and logical time", path);
  }
  return record;
}

[[nodiscard]] ObjectKey decode_key(Failure& failure, const JsonValue& value, std::string_view path) {
  Members members(failure, value, path);
  ObjectKey key;
  if (const JsonValue* asset = members.need("asset"); asset != nullptr) {
    key.asset = decode_id<AssetId>(failure, *asset, member_path(path, "asset"));
  }
  if (const JsonValue* generation = members.need("hardware_generation"); generation != nullptr) {
    key.hardware_generation =
        decode_counter<HardwareGeneration>(failure, *generation, member_path(path, "hardware_generation"));
  }
  (void)members.sealed();
  if (failure.ok() && !key.valid()) {
    (void)failure.at(ErrorCode::MalformedRequest,
                     "a lifecycle hardware key needs a valid asset id and hardware generation", path);
  }
  return key;
}

[[nodiscard]] std::optional<Location> decode_location(Failure& failure, const JsonValue& value, std::string_view path) {
  if (value.is_null()) {
    return std::nullopt;
  }
  Members members(failure, value, path);
  Location location;
  if (const JsonValue* site = members.need("site"); site != nullptr) {
    location.site = decode_id<SiteId>(failure, *site, member_path(path, "site"));
  }
  if (const JsonValue* rack = members.need("rack"); rack != nullptr) {
    location.rack = decode_id<RackId>(failure, *rack, member_path(path, "rack"));
  }
  if (const JsonValue* slot = members.need("slot"); slot != nullptr) {
    location.slot = decode_id<SlotId>(failure, *slot, member_path(path, "slot"));
  }
  (void)members.sealed();
  return location;
}

[[nodiscard]] WallClock decode_wall_clock(Failure& failure, const JsonValue& value, std::string_view path) {
  if (value.is_null()) {
    return WallClock();
  }
  const std::string_view text = require_string(failure, value, path);
  if (!failure.ok()) {
    return WallClock();
  }
  const Result<WallClock> parsed = WallClock::parse(text);
  if (!parsed.has_value()) {
    (void)failure.raise(parsed.error(), path);
    return WallClock();
  }
  return parsed.value();
}

/// The authority array as a mask. Scopes are unique and ascend in declaration
/// order, which is exactly the order the writer emits them in.
[[nodiscard]] AuthorityMask decode_authority(Failure& failure, const JsonValue& value, std::string_view path) {
  if (!value.is_array()) {
    (void)failure.at(ErrorCode::MalformedRequest, "expected an array of authority scopes", path);
    return AuthorityMask();
  }
  AuthorityMask mask;
  unsigned previous = 0;
  bool seen = false;
  const std::vector<JsonValue>& items = value.items();
  for (std::size_t index = 0; index < items.size(); ++index) {
    const std::string element = element_path(path, index);
    const Result<AuthorityScope> parsed = parse_authority_scope(require_string(failure, items[index], element));
    if (!failure.ok()) {
      return mask;
    }
    if (!parsed.has_value()) {
      (void)failure.raise(parsed.error(), element);
      return mask;
    }
    const AuthorityMask bit = authority_bit(parsed.value());
    if (mask.contains(bit)) {
      (void)failure.at(ErrorCode::MalformedRequest, "an authority scope is repeated", element);
      return mask;
    }
    const unsigned raw = static_cast<unsigned>(parsed.value());
    if (seen && raw <= previous) {
      (void)failure.at(ErrorCode::MalformedRequest, "authority scopes must ascend in declaration order", element);
      return mask;
    }
    previous = raw;
    seen = true;
    mask |= bit;
  }
  return mask;
}

[[nodiscard]] EvidenceRef decode_evidence(Failure& failure, const JsonValue& value, std::string_view path) {
  Members members(failure, value, path);
  EvidenceRef evidence;
  if (const JsonValue* kind = members.need("kind"); kind != nullptr) {
    evidence.kind = decode_enum<EvidenceKind>(failure, *kind, member_path(path, "kind"), parse_evidence_kind);
  }
  if (const JsonValue* digest = members.need("digest"); digest != nullptr) {
    evidence.digest = decode_digest(failure, *digest, member_path(path, "digest"));
  }
  if (const JsonValue* source = members.need("source"); source != nullptr) {
    evidence.source = std::string(require_string(failure, *source, member_path(path, "source")));
  }
  if (const JsonValue* sequence = members.need("observed_sequence"); sequence != nullptr) {
    evidence.observed_sequence =
        decode_counter<ObservationSequence>(failure, *sequence, member_path(path, "observed_sequence"));
  }
  (void)members.sealed();
  return evidence;
}

[[nodiscard]] Provenance decode_provenance(Failure& failure, const JsonValue& value, std::string_view path) {
  Members members(failure, value, path);
  Provenance provenance;
  if (const JsonValue* actor = members.need("actor"); actor != nullptr) {
    const std::string actor_path = member_path(path, "actor");
    Members actor_members(failure, *actor, actor_path);
    if (const JsonValue* id = actor_members.need("id"); id != nullptr) {
      provenance.actor.id = decode_id<ActorId>(failure, *id, member_path(actor_path, "id"));
    }
    if (const JsonValue* kind = actor_members.need("kind"); kind != nullptr) {
      provenance.actor.kind = decode_enum<ActorKind>(failure, *kind, member_path(actor_path, "kind"), parse_actor_kind);
    }
    (void)actor_members.sealed();
  }
  if (const JsonValue* authority = members.need("authority"); authority != nullptr) {
    provenance.authority = decode_authority(failure, *authority, member_path(path, "authority"));
  }
  if (const JsonValue* generation = members.need("policy_generation"); generation != nullptr) {
    provenance.policy_generation =
        decode_counter<PolicyGeneration>(failure, *generation, member_path(path, "policy_generation"));
  }
  if (const JsonValue* plan = members.need("plan"); plan != nullptr) {
    provenance.plan = decode_id<PlanId>(failure, *plan, member_path(path, "plan"));
  }
  if (const JsonValue* attempt = members.need("attempt"); attempt != nullptr) {
    provenance.attempt = decode_id<AttemptId>(failure, *attempt, member_path(path, "attempt"));
  }
  if (const JsonValue* evidence = members.need("evidence"); evidence != nullptr) {
    const std::string evidence_path = member_path(path, "evidence");
    if (!evidence->is_array()) {
      (void)failure.at(ErrorCode::MalformedRequest, "expected a JSON array", evidence_path);
    } else {
      const std::vector<JsonValue>& items = evidence->items();
      provenance.evidence.reserve(items.size());
      for (std::size_t index = 0; index < items.size(); ++index) {
        provenance.evidence.push_back(decode_evidence(failure, items[index], element_path(evidence_path, index)));
        if (!failure.ok()) {
          break;
        }
      }
    }
  }
  if (const JsonValue* logical_time = members.need("logical_time"); logical_time != nullptr) {
    provenance.logical_time = decode_counter<LogicalTime>(failure, *logical_time, member_path(path, "logical_time"));
  }
  if (const JsonValue* clock = members.need("wall_clock"); clock != nullptr) {
    provenance.wall_clock = decode_wall_clock(failure, *clock, member_path(path, "wall_clock"));
  }
  (void)members.sealed();
  return provenance;
}

[[nodiscard]] Receipt decode_receipt(Failure& failure, const JsonValue& value, std::string_view path) {
  Members members(failure, value, path);
  const JsonValue* tag = members.need("type");
  std::string type;
  if (tag != nullptr) {
    type = std::string(require_string(failure, *tag, member_path(path, "type")));
  }
  if (!failure.ok()) {
    return CreateReceipt{};
  }

  if (type == "create") {
    CreateReceipt receipt;
    if (const JsonValue* item = members.need("plan"); item != nullptr) {
      receipt.plan = decode_id<PlanId>(failure, *item, member_path(path, "plan"));
    }
    if (const JsonValue* item = members.need("attempt"); item != nullptr) {
      receipt.attempt = decode_id<AttemptId>(failure, *item, member_path(path, "attempt"));
    }
    if (const JsonValue* item = members.need("key"); item != nullptr) {
      receipt.key = decode_key(failure, *item, member_path(path, "key"));
    }
    if (const JsonValue* item = members.need("state"); item != nullptr) {
      receipt.state = decode_enum<LifecycleState>(failure, *item, member_path(path, "state"), parse_lifecycle_state);
    }
    if (const JsonValue* item = members.need("lifecycle_generation"); item != nullptr) {
      receipt.lifecycle_generation =
          decode_counter<LifecycleGeneration>(failure, *item, member_path(path, "lifecycle_generation"));
    }
    if (const JsonValue* item = members.need("revision"); item != nullptr) {
      receipt.revision = decode_counter<Revision>(failure, *item, member_path(path, "revision"));
    }
    if (const JsonValue* item = members.need("commit_sequence"); item != nullptr) {
      receipt.commit_sequence = decode_counter<CommitSequence>(failure, *item, member_path(path, "commit_sequence"));
    }
    if (const JsonValue* item = members.need("logical_time"); item != nullptr) {
      receipt.logical_time = decode_counter<LogicalTime>(failure, *item, member_path(path, "logical_time"));
    }
    if (const JsonValue* item = members.need("receipt_digest"); item != nullptr) {
      receipt.receipt_digest = decode_digest(failure, *item, member_path(path, "receipt_digest"));
    }
    if (const JsonValue* item = members.need("idempotent_replay"); item != nullptr) {
      receipt.idempotent_replay = require_bool(failure, *item, member_path(path, "idempotent_replay"));
    }
    (void)members.sealed();
    return receipt;
  }

  if (type == "transition") {
    TransitionReceipt receipt;
    if (const JsonValue* item = members.need("plan"); item != nullptr) {
      receipt.plan = decode_id<PlanId>(failure, *item, member_path(path, "plan"));
    }
    if (const JsonValue* item = members.need("attempt"); item != nullptr) {
      receipt.attempt = decode_id<AttemptId>(failure, *item, member_path(path, "attempt"));
    }
    if (const JsonValue* item = members.need("key"); item != nullptr) {
      receipt.key = decode_key(failure, *item, member_path(path, "key"));
    }
    if (const JsonValue* item = members.need("from"); item != nullptr) {
      receipt.from = decode_enum<LifecycleState>(failure, *item, member_path(path, "from"), parse_lifecycle_state);
    }
    if (const JsonValue* item = members.need("to"); item != nullptr) {
      receipt.to = decode_enum<LifecycleState>(failure, *item, member_path(path, "to"), parse_lifecycle_state);
    }
    if (const JsonValue* item = members.need("reason"); item != nullptr) {
      receipt.reason =
          decode_enum<TransitionReason>(failure, *item, member_path(path, "reason"), parse_transition_reason);
    }
    if (const JsonValue* item = members.need("revision_before"); item != nullptr) {
      receipt.revision_before = decode_counter<Revision>(failure, *item, member_path(path, "revision_before"));
    }
    if (const JsonValue* item = members.need("revision_after"); item != nullptr) {
      receipt.revision_after = decode_counter<Revision>(failure, *item, member_path(path, "revision_after"));
    }
    if (const JsonValue* item = members.need("lifecycle_generation"); item != nullptr) {
      receipt.lifecycle_generation =
          decode_counter<LifecycleGeneration>(failure, *item, member_path(path, "lifecycle_generation"));
    }
    if (const JsonValue* item = members.need("gate_after"); item != nullptr) {
      receipt.gate_after =
          decode_enum<EligibilityGate>(failure, *item, member_path(path, "gate_after"), parse_eligibility_gate);
    }
    if (const JsonValue* item = members.need("commit_sequence"); item != nullptr) {
      receipt.commit_sequence = decode_counter<CommitSequence>(failure, *item, member_path(path, "commit_sequence"));
    }
    if (const JsonValue* item = members.need("logical_time"); item != nullptr) {
      receipt.logical_time = decode_counter<LogicalTime>(failure, *item, member_path(path, "logical_time"));
    }
    if (const JsonValue* item = members.need("request_digest"); item != nullptr) {
      receipt.request_digest = decode_digest(failure, *item, member_path(path, "request_digest"));
    }
    if (const JsonValue* item = members.need("entry_digest"); item != nullptr) {
      receipt.entry_digest = decode_digest(failure, *item, member_path(path, "entry_digest"));
    }
    if (const JsonValue* item = members.need("receipt_digest"); item != nullptr) {
      receipt.receipt_digest = decode_digest(failure, *item, member_path(path, "receipt_digest"));
    }
    if (const JsonValue* item = members.need("idempotent_replay"); item != nullptr) {
      receipt.idempotent_replay = require_bool(failure, *item, member_path(path, "idempotent_replay"));
    }
    (void)members.sealed();
    return receipt;
  }

  if (type == "replacement") {
    ReplacementReceipt receipt;
    if (const JsonValue* item = members.need("plan"); item != nullptr) {
      receipt.plan = decode_id<PlanId>(failure, *item, member_path(path, "plan"));
    }
    if (const JsonValue* item = members.need("attempt"); item != nullptr) {
      receipt.attempt = decode_id<AttemptId>(failure, *item, member_path(path, "attempt"));
    }
    if (const JsonValue* item = members.need("predecessor"); item != nullptr) {
      receipt.predecessor = decode_key(failure, *item, member_path(path, "predecessor"));
    }
    if (const JsonValue* item = members.need("successor"); item != nullptr) {
      receipt.successor = decode_key(failure, *item, member_path(path, "successor"));
    }
    if (const JsonValue* item = members.need("replacement_generation"); item != nullptr) {
      receipt.replacement_generation =
          decode_counter<ReplacementGeneration>(failure, *item, member_path(path, "replacement_generation"));
    }
    if (const JsonValue* item = members.need("commit_sequence"); item != nullptr) {
      receipt.commit_sequence = decode_counter<CommitSequence>(failure, *item, member_path(path, "commit_sequence"));
    }
    if (const JsonValue* item = members.need("logical_time"); item != nullptr) {
      receipt.logical_time = decode_counter<LogicalTime>(failure, *item, member_path(path, "logical_time"));
    }
    if (const JsonValue* item = members.need("link_digest"); item != nullptr) {
      receipt.link_digest = decode_digest(failure, *item, member_path(path, "link_digest"));
    }
    if (const JsonValue* item = members.need("request_digest"); item != nullptr) {
      receipt.request_digest = decode_digest(failure, *item, member_path(path, "request_digest"));
    }
    if (const JsonValue* item = members.need("receipt_digest"); item != nullptr) {
      receipt.receipt_digest = decode_digest(failure, *item, member_path(path, "receipt_digest"));
    }
    if (const JsonValue* item = members.need("idempotent_replay"); item != nullptr) {
      receipt.idempotent_replay = require_bool(failure, *item, member_path(path, "idempotent_replay"));
    }
    (void)members.sealed();
    return receipt;
  }

  if (type == "gate") {
    EligibilityGateReceipt receipt;
    if (const JsonValue* item = members.need("plan"); item != nullptr) {
      receipt.plan = decode_id<PlanId>(failure, *item, member_path(path, "plan"));
    }
    if (const JsonValue* item = members.need("attempt"); item != nullptr) {
      receipt.attempt = decode_id<AttemptId>(failure, *item, member_path(path, "attempt"));
    }
    if (const JsonValue* item = members.need("key"); item != nullptr) {
      receipt.key = decode_key(failure, *item, member_path(path, "key"));
    }
    if (const JsonValue* item = members.need("before"); item != nullptr) {
      receipt.before = decode_enum<EligibilityGate>(failure, *item, member_path(path, "before"), parse_eligibility_gate);
    }
    if (const JsonValue* item = members.need("after"); item != nullptr) {
      receipt.after = decode_enum<EligibilityGate>(failure, *item, member_path(path, "after"), parse_eligibility_gate);
    }
    if (const JsonValue* item = members.need("revision"); item != nullptr) {
      receipt.revision = decode_counter<Revision>(failure, *item, member_path(path, "revision"));
    }
    if (const JsonValue* item = members.need("commit_sequence"); item != nullptr) {
      receipt.commit_sequence = decode_counter<CommitSequence>(failure, *item, member_path(path, "commit_sequence"));
    }
    if (const JsonValue* item = members.need("logical_time"); item != nullptr) {
      receipt.logical_time = decode_counter<LogicalTime>(failure, *item, member_path(path, "logical_time"));
    }
    if (const JsonValue* item = members.need("request_digest"); item != nullptr) {
      receipt.request_digest = decode_digest(failure, *item, member_path(path, "request_digest"));
    }
    if (const JsonValue* item = members.need("receipt_digest"); item != nullptr) {
      receipt.receipt_digest = decode_digest(failure, *item, member_path(path, "receipt_digest"));
    }
    if (const JsonValue* item = members.need("idempotent_replay"); item != nullptr) {
      receipt.idempotent_replay = require_bool(failure, *item, member_path(path, "idempotent_replay"));
    }
    (void)members.sealed();
    return receipt;
  }

  if (type == "health") {
    HealthReceipt receipt;
    if (const JsonValue* item = members.need("plan"); item != nullptr) {
      receipt.plan = decode_id<PlanId>(failure, *item, member_path(path, "plan"));
    }
    if (const JsonValue* item = members.need("attempt"); item != nullptr) {
      receipt.attempt = decode_id<AttemptId>(failure, *item, member_path(path, "attempt"));
    }
    if (const JsonValue* item = members.need("key"); item != nullptr) {
      receipt.key = decode_key(failure, *item, member_path(path, "key"));
    }
    if (const JsonValue* item = members.need("sequence"); item != nullptr) {
      receipt.sequence = decode_counter<ObservationSequence>(failure, *item, member_path(path, "sequence"));
    }
    if (const JsonValue* item = members.need("commit_sequence"); item != nullptr) {
      receipt.commit_sequence = decode_counter<CommitSequence>(failure, *item, member_path(path, "commit_sequence"));
    }
    if (const JsonValue* item = members.need("logical_time"); item != nullptr) {
      receipt.logical_time = decode_counter<LogicalTime>(failure, *item, member_path(path, "logical_time"));
    }
    if (const JsonValue* item = members.need("request_digest"); item != nullptr) {
      receipt.request_digest = decode_digest(failure, *item, member_path(path, "request_digest"));
    }
    if (const JsonValue* item = members.need("receipt_digest"); item != nullptr) {
      receipt.receipt_digest = decode_digest(failure, *item, member_path(path, "receipt_digest"));
    }
    if (const JsonValue* item = members.need("idempotent_replay"); item != nullptr) {
      receipt.idempotent_replay = require_bool(failure, *item, member_path(path, "idempotent_replay"));
    }
    (void)members.sealed();
    return receipt;
  }

  if (type == "attestation") {
    AttestationReceipt receipt;
    if (const JsonValue* item = members.need("plan"); item != nullptr) {
      receipt.plan = decode_id<PlanId>(failure, *item, member_path(path, "plan"));
    }
    if (const JsonValue* item = members.need("attempt"); item != nullptr) {
      receipt.attempt = decode_id<AttemptId>(failure, *item, member_path(path, "attempt"));
    }
    if (const JsonValue* item = members.need("key"); item != nullptr) {
      receipt.key = decode_key(failure, *item, member_path(path, "key"));
    }
    if (const JsonValue* item = members.need("revision"); item != nullptr) {
      receipt.revision = decode_counter<Revision>(failure, *item, member_path(path, "revision"));
    }
    if (const JsonValue* item = members.need("commit_sequence"); item != nullptr) {
      receipt.commit_sequence = decode_counter<CommitSequence>(failure, *item, member_path(path, "commit_sequence"));
    }
    if (const JsonValue* item = members.need("logical_time"); item != nullptr) {
      receipt.logical_time = decode_counter<LogicalTime>(failure, *item, member_path(path, "logical_time"));
    }
    if (const JsonValue* item = members.need("request_digest"); item != nullptr) {
      receipt.request_digest = decode_digest(failure, *item, member_path(path, "request_digest"));
    }
    if (const JsonValue* item = members.need("receipt_digest"); item != nullptr) {
      receipt.receipt_digest = decode_digest(failure, *item, member_path(path, "receipt_digest"));
    }
    if (const JsonValue* item = members.need("idempotent_replay"); item != nullptr) {
      receipt.idempotent_replay = require_bool(failure, *item, member_path(path, "idempotent_replay"));
    }
    (void)members.sealed();
    return receipt;
  }

  (void)failure.at(ErrorCode::MalformedRequest, "unknown receipt type tag", member_path(path, "type"));
  return CreateReceipt{};
}

[[nodiscard]] HardwareObject decode_object(Failure& failure, const JsonValue& value, std::string_view path) {
  Members members(failure, value, path);
  HardwareObject object;
  if (const JsonValue* asset = members.need("asset"); asset != nullptr) {
    object.key.asset = decode_id<AssetId>(failure, *asset, member_path(path, "asset"));
  }
  if (const JsonValue* generation = members.need("hardware_generation"); generation != nullptr) {
    object.key.hardware_generation =
        decode_counter<HardwareGeneration>(failure, *generation, member_path(path, "hardware_generation"));
  }
  if (failure.ok() && !object.key.valid()) {
    (void)failure.at(ErrorCode::MalformedRequest,
                     "a lifecycle hardware key needs a valid asset id and hardware generation", path);
  }
  if (const JsonValue* kind = members.need("kind"); kind != nullptr) {
    object.kind = decode_enum<HardwareKind>(failure, *kind, member_path(path, "kind"), parse_hardware_kind);
  }
  if (const JsonValue* model = members.need("model"); model != nullptr) {
    object.model = decode_id<ModelId>(failure, *model, member_path(path, "model"));
  }
  if (const JsonValue* firmware = members.need("firmware_generation"); firmware != nullptr) {
    object.firmware_generation =
        decode_counter<FirmwareGeneration>(failure, *firmware, member_path(path, "firmware_generation"));
  }
  if (const JsonValue* generation = members.need("lifecycle_generation"); generation != nullptr) {
    object.lifecycle_generation =
        decode_counter<LifecycleGeneration>(failure, *generation, member_path(path, "lifecycle_generation"));
  }
  if (const JsonValue* state = members.need("state"); state != nullptr) {
    object.state = decode_enum<LifecycleState>(failure, *state, member_path(path, "state"), parse_lifecycle_state);
  }
  if (const JsonValue* revision = members.need("revision"); revision != nullptr) {
    object.revision = decode_counter<Revision>(failure, *revision, member_path(path, "revision"));
  }
  if (const JsonValue* location = members.need("location"); location != nullptr) {
    object.location = decode_location(failure, *location, member_path(path, "location"));
  }
  if (const JsonValue* gate = members.need("eligibility_gate"); gate != nullptr) {
    object.eligibility_gate =
        decode_enum<EligibilityGate>(failure, *gate, member_path(path, "eligibility_gate"), parse_eligibility_gate);
  }
  if (const JsonValue* predecessor = members.need("predecessor"); predecessor != nullptr) {
    if (predecessor->is_null()) {
      object.predecessor = std::nullopt;
    } else {
      object.predecessor = decode_key(failure, *predecessor, member_path(path, "predecessor"));
    }
  }
  if (const JsonValue* successor = members.need("successor"); successor != nullptr) {
    if (successor->is_null()) {
      object.successor = std::nullopt;
    } else {
      object.successor = decode_key(failure, *successor, member_path(path, "successor"));
    }
  }
  if (const JsonValue* generation = members.need("replacement_generation"); generation != nullptr) {
    object.replacement_generation =
        decode_counter<ReplacementGeneration>(failure, *generation, member_path(path, "replacement_generation"));
  }
  if (const JsonValue* created = members.need("created_at"); created != nullptr) {
    object.created_at = decode_counter<LogicalTime>(failure, *created, member_path(path, "created_at"));
  }
  if (const JsonValue* updated = members.need("updated_at"); updated != nullptr) {
    object.updated_at = decode_counter<LogicalTime>(failure, *updated, member_path(path, "updated_at"));
  }
  if (const JsonValue* entries = members.need("history_entries"); entries != nullptr) {
    object.history_entries = require_number(failure, *entries, member_path(path, "history_entries"));
  }
  (void)members.sealed();
  return object;
}

[[nodiscard]] HistoryEntry decode_history(Failure& failure, const JsonValue& value, std::string_view path) {
  Members members(failure, value, path);
  HistoryEntry entry;
  if (const JsonValue* sequence = members.need("commit_sequence"); sequence != nullptr) {
    entry.commit_sequence = decode_counter<CommitSequence>(failure, *sequence, member_path(path, "commit_sequence"));
  }
  if (const JsonValue* logical_time = members.need("logical_time"); logical_time != nullptr) {
    entry.logical_time = decode_counter<LogicalTime>(failure, *logical_time, member_path(path, "logical_time"));
  }
  if (const JsonValue* asset = members.need("asset"); asset != nullptr) {
    entry.key.asset = decode_id<AssetId>(failure, *asset, member_path(path, "asset"));
  }
  if (const JsonValue* generation = members.need("hardware_generation"); generation != nullptr) {
    entry.key.hardware_generation =
        decode_counter<HardwareGeneration>(failure, *generation, member_path(path, "hardware_generation"));
  }
  if (failure.ok() && !entry.key.valid()) {
    (void)failure.at(ErrorCode::MalformedRequest,
                     "a lifecycle hardware key needs a valid asset id and hardware generation", path);
  }
  if (const JsonValue* generation = members.need("lifecycle_generation"); generation != nullptr) {
    entry.lifecycle_generation =
        decode_counter<LifecycleGeneration>(failure, *generation, member_path(path, "lifecycle_generation"));
  }
  if (const JsonValue* before = members.need("revision_before"); before != nullptr) {
    entry.revision_before = decode_counter<Revision>(failure, *before, member_path(path, "revision_before"));
  }
  if (const JsonValue* after = members.need("revision_after"); after != nullptr) {
    entry.revision_after = decode_counter<Revision>(failure, *after, member_path(path, "revision_after"));
  }
  if (const JsonValue* from = members.need("from"); from != nullptr) {
    entry.from = decode_enum<LifecycleState>(failure, *from, member_path(path, "from"), parse_lifecycle_state);
  }
  if (const JsonValue* to = members.need("to"); to != nullptr) {
    entry.to = decode_enum<LifecycleState>(failure, *to, member_path(path, "to"), parse_lifecycle_state);
  }
  if (const JsonValue* reason = members.need("reason"); reason != nullptr) {
    entry.reason = decode_enum<TransitionReason>(failure, *reason, member_path(path, "reason"), parse_transition_reason);
  }
  if (const JsonValue* provenance = members.need("provenance"); provenance != nullptr) {
    entry.provenance = decode_provenance(failure, *provenance, member_path(path, "provenance"));
  }
  if (const JsonValue* digest = members.need("receipt_digest"); digest != nullptr) {
    entry.receipt_digest = decode_digest(failure, *digest, member_path(path, "receipt_digest"));
  }
  if (const JsonValue* digest = members.need("chain_digest"); digest != nullptr) {
    entry.chain_digest = decode_digest(failure, *digest, member_path(path, "chain_digest"));
  }
  (void)members.sealed();
  return entry;
}

[[nodiscard]] ReplacementRecord decode_lineage(Failure& failure, const JsonValue& value, std::string_view path) {
  Members members(failure, value, path);
  ReplacementRecord record;
  if (const JsonValue* predecessor = members.need("predecessor"); predecessor != nullptr) {
    record.predecessor = decode_key(failure, *predecessor, member_path(path, "predecessor"));
  }
  if (const JsonValue* successor = members.need("successor"); successor != nullptr) {
    record.successor = decode_key(failure, *successor, member_path(path, "successor"));
  }
  if (const JsonValue* reason = members.need("reason"); reason != nullptr) {
    record.reason = decode_enum<TransitionReason>(failure, *reason, member_path(path, "reason"), parse_transition_reason);
  }
  if (const JsonValue* provenance = members.need("provenance"); provenance != nullptr) {
    record.provenance = decode_provenance(failure, *provenance, member_path(path, "provenance"));
  }
  if (const JsonValue* linked = members.need("linked_at"); linked != nullptr) {
    record.linked_at = decode_counter<LogicalTime>(failure, *linked, member_path(path, "linked_at"));
  }
  if (const JsonValue* sequence = members.need("commit_sequence"); sequence != nullptr) {
    record.commit_sequence = decode_counter<CommitSequence>(failure, *sequence, member_path(path, "commit_sequence"));
  }
  if (const JsonValue* generation = members.need("replacement_generation"); generation != nullptr) {
    record.replacement_generation =
        decode_counter<ReplacementGeneration>(failure, *generation, member_path(path, "replacement_generation"));
  }
  if (const JsonValue* digest = members.need("link_digest"); digest != nullptr) {
    record.link_digest = decode_digest(failure, *digest, member_path(path, "link_digest"));
  }
  (void)members.sealed();
  return record;
}

[[nodiscard]] AppliedPlan decode_plan(Failure& failure, const JsonValue& value, std::string_view path) {
  Members members(failure, value, path);
  AppliedPlan plan;
  if (const JsonValue* id = members.need("plan"); id != nullptr) {
    plan.key.plan = decode_id<PlanId>(failure, *id, member_path(path, "plan"));
  }
  if (const JsonValue* attempt = members.need("attempt"); attempt != nullptr) {
    plan.key.attempt = decode_id<AttemptId>(failure, *attempt, member_path(path, "attempt"));
  }
  if (const JsonValue* kind = members.need("kind"); kind != nullptr) {
    plan.kind = decode_record_kind(failure, *kind, member_path(path, "kind"));
  }
  if (const JsonValue* digest = members.need("request_digest"); digest != nullptr) {
    plan.request_digest = decode_digest(failure, *digest, member_path(path, "request_digest"));
  }
  if (const JsonValue* sequence = members.need("commit_sequence"); sequence != nullptr) {
    plan.commit_sequence = decode_counter<CommitSequence>(failure, *sequence, member_path(path, "commit_sequence"));
  }
  if (const JsonValue* logical_time = members.need("logical_time"); logical_time != nullptr) {
    plan.logical_time = decode_counter<LogicalTime>(failure, *logical_time, member_path(path, "logical_time"));
  }
  if (const JsonValue* receipt = members.need("receipt"); receipt != nullptr) {
    plan.receipt = decode_receipt(failure, *receipt, member_path(path, "receipt"));
  }
  (void)members.sealed();
  return plan;
}

/// Decodes the observation content into an observation whose key is already
/// known: the health entry carries the key once, at the entry level.
void decode_observation(Failure& failure, const JsonValue& value, std::string_view path, HealthObservation& observation) {
  Members members(failure, value, path);
  if (const JsonValue* health = members.need("health"); health != nullptr) {
    observation.health = decode_enum<HealthStatus>(failure, *health, member_path(path, "health"), parse_health_status);
  }
  if (const JsonValue* readiness = members.need("readiness"); readiness != nullptr) {
    observation.readiness =
        decode_enum<ReadinessStatus>(failure, *readiness, member_path(path, "readiness"), parse_readiness_status);
  }
  if (const JsonValue* availability = members.need("availability"); availability != nullptr) {
    observation.availability = decode_enum<AvailabilityStatus>(failure, *availability, member_path(path, "availability"),
                                                               parse_availability_status);
  }
  if (const JsonValue* digest = members.need("evidence_digest"); digest != nullptr) {
    observation.evidence_digest = decode_digest(failure, *digest, member_path(path, "evidence_digest"));
  }
  if (const JsonValue* source = members.need("source"); source != nullptr) {
    observation.source = std::string(require_string(failure, *source, member_path(path, "source")));
  }
  if (const JsonValue* clock = members.need("observed_wall_clock"); clock != nullptr) {
    observation.observed_wall_clock = decode_wall_clock(failure, *clock, member_path(path, "observed_wall_clock"));
  }
  (void)members.sealed();
}

[[nodiscard]] HealthSnapshot decode_health(Failure& failure, const JsonValue& value, std::string_view path) {
  Members members(failure, value, path);
  HealthSnapshot snapshot;
  if (const JsonValue* asset = members.need("asset"); asset != nullptr) {
    snapshot.observation.key.asset = decode_id<AssetId>(failure, *asset, member_path(path, "asset"));
  }
  if (const JsonValue* generation = members.need("hardware_generation"); generation != nullptr) {
    snapshot.observation.key.hardware_generation =
        decode_counter<HardwareGeneration>(failure, *generation, member_path(path, "hardware_generation"));
  }
  if (failure.ok() && !snapshot.observation.key.valid()) {
    (void)failure.at(ErrorCode::MalformedRequest,
                     "a lifecycle hardware key needs a valid asset id and hardware generation", path);
  }
  if (const JsonValue* present = members.need("present"); present != nullptr) {
    snapshot.present = require_bool(failure, *present, member_path(path, "present"));
  }
  if (const JsonValue* observation = members.need("observation"); observation != nullptr) {
    if (!failure.ok()) {
      // The presence flag is already known to be unusable; nothing to compare.
    } else if (snapshot.present) {
      if (observation->is_null()) {
        (void)failure.at(ErrorCode::MalformedRequest,
                         "an observation is required when the snapshot is present", member_path(path, "observation"));
      } else {
        decode_observation(failure, *observation, member_path(path, "observation"), snapshot.observation);
      }
    } else if (!observation->is_null()) {
      (void)failure.at(ErrorCode::MalformedRequest,
                       "the observation content is not durable when the snapshot is not present",
                       member_path(path, "observation"));
    }
  }
  if (const JsonValue* sequence = members.need("sequence"); sequence != nullptr) {
    snapshot.sequence = decode_counter<ObservationSequence>(failure, *sequence, member_path(path, "sequence"));
  }
  if (const JsonValue* recorded = members.need("recorded_at"); recorded != nullptr) {
    snapshot.recorded_at = decode_counter<LogicalTime>(failure, *recorded, member_path(path, "recorded_at"));
  }
  (void)members.sealed();
  return snapshot;
}

/// Rejects a value that is not a JSON array, and reports a missing one.
[[nodiscard]] bool ensure_array(Failure& failure, const JsonValue* value, std::string_view path) {
  if (value == nullptr) {
    return false;
  }
  if (!value->is_array()) {
    return failure.at(ErrorCode::MalformedRequest, "expected a JSON array", path);
  }
  return true;
}

[[nodiscard]] bool ensure_object(Failure& failure, const JsonValue* value, std::string_view path) {
  if (value == nullptr) {
    return false;
  }
  if (!value->is_object()) {
    return failure.at(ErrorCode::MalformedRequest, "expected a JSON object", path);
  }
  return true;
}

/// The declared counts, and the numbers they must equal.
[[nodiscard]] bool check_counts(Failure& failure, const JsonValue& counts, const std::size_t observed[kCollections]) {
  static constexpr std::array<std::string_view, kCollections> kNames = {"objects", "history", "lineage",
                                                                        "plans",   "health",  "events"};
  Members members(failure, counts, "counts");
  std::array<std::uint64_t, kCollections> declared = {0, 0, 0, 0, 0, 0};
  for (std::size_t index = 0; index < kNames.size(); ++index) {
    if (const JsonValue* value = members.need(kNames[index]); value != nullptr) {
      declared[index] = require_number(failure, *value, member_path("counts", kNames[index]));
    }
  }
  (void)members.sealed();
  if (!failure.ok()) {
    return false;
  }
  for (std::size_t index = 0; index < kNames.size(); ++index) {
    if (declared[index] != static_cast<std::uint64_t>(observed[index])) {
      return failure.at(ErrorCode::MalformedRequest, "declared count does not match the number of entries",
                        member_path("counts", kNames[index]));
    }
  }
  return true;
}

/// Bounds that hold the document as a whole, checked before any element is
/// examined. An over long collection is rejected as a collection.
[[nodiscard]] bool check_bounds(Failure& failure, const std::size_t observed[kCollections]) {
  const std::size_t object_count = observed[kObjects];
  const Result<void> object_bound = limits::check_limit("objects", object_count, limits::kMaxSnapshotObjects);
  if (!object_bound.has_value()) {
    return failure.raise(object_bound.error(), "objects");
  }
  const Result<void> plan_bound = limits::check_limit("plans", observed[kPlans], limits::kMaxAppliedPlans);
  if (!plan_bound.has_value()) {
    return failure.raise(plan_bound.error(), "plans");
  }
  if (observed[kLineage] > object_count) {
    return failure.at(ErrorCode::LimitExceeded, "more lineage links than objects", "lineage");
  }
  if (observed[kHealth] > object_count) {
    return failure.at(ErrorCode::LimitExceeded, "more health snapshots than objects", "health");
  }
  const std::size_t per_object = limits::kMaxHistoryEntriesPerObject;
  const std::size_t ceiling = (std::numeric_limits<std::size_t>::max)();
  const std::size_t max_history = object_count > ceiling / per_object ? ceiling : object_count * per_object;
  if (observed[kHistory] > max_history) {
    return failure.at(ErrorCode::LimitExceeded, "more history entries than the object set can hold", "history");
  }
  return true;
}

/// One history entry names an object of this document, and no two entries of one
/// object share a commit sequence.
[[nodiscard]] bool check_history_links(Failure& failure, const std::vector<HistoryEntry>& history,
                                        const std::set<ObjectKey>& objects) {
  std::set<std::pair<ObjectKey, CommitSequence>> seen;
  for (std::size_t index = 0; index < history.size(); ++index) {
    const std::string path = element_path("history", index);
    if (objects.count(history[index].key) == 0) {
      return failure.at(ErrorCode::MalformedRequest, "history entry names an object that is not in the document",
                        path);
    }
    if (!seen.insert(std::make_pair(history[index].key, history[index].commit_sequence)).second) {
      return failure.at(ErrorCode::MalformedRequest, "duplicate history entry for one commit sequence", path);
    }
  }
  return true;
}

[[nodiscard]] bool check_lineage_links(Failure& failure, const std::vector<ReplacementRecord>& lineage,
                                       const std::set<ObjectKey>& objects) {
  std::set<ObjectKey> seen;
  for (std::size_t index = 0; index < lineage.size(); ++index) {
    const std::string path = element_path("lineage", index);
    if (objects.count(lineage[index].predecessor) == 0 || objects.count(lineage[index].successor) == 0) {
      return failure.at(ErrorCode::MalformedRequest, "a lineage link names an object that is not in the document",
                        path);
    }
    if (!seen.insert(lineage[index].predecessor).second) {
      return failure.at(ErrorCode::MalformedRequest, "duplicate lineage link for one predecessor", path);
    }
  }
  return true;
}

[[nodiscard]] bool check_health_links(Failure& failure, const std::vector<HealthSnapshot>& health,
                                      const std::set<ObjectKey>& objects) {
  std::set<ObjectKey> seen;
  for (std::size_t index = 0; index < health.size(); ++index) {
    const std::string path = element_path("health", index);
    const ObjectKey& key = health[index].observation.key;
    if (objects.count(key) == 0) {
      return failure.at(ErrorCode::MalformedRequest, "a health snapshot names an object that is not in the document",
                        path);
    }
    if (!seen.insert(key).second) {
      return failure.at(ErrorCode::MalformedRequest, "duplicate health snapshot for one object", path);
    }
  }
  return true;
}

[[nodiscard]] bool check_plan_keys(Failure& failure, const std::vector<AppliedPlan>& plans) {
  std::set<PlanKey> seen;
  for (std::size_t index = 0; index < plans.size(); ++index) {
    if (!seen.insert(plans[index].key).second) {
      return failure.at(ErrorCode::MalformedRequest, "duplicate plan attempt", element_path("plans", index));
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Snapshot capture
// ---------------------------------------------------------------------------

Snapshot snapshot_of(const Registry& registry, const std::vector<ExportedRecord>& events, ControlEpoch control_epoch,
                     IncarnationId incarnation, const ExportOptions& options) {
  Snapshot snapshot;
  snapshot.header.format_version = kExportFormatVersion;
  snapshot.header.library_version = std::string(library_version_string());
  snapshot.header.state_digest = registry.state_digest();
  snapshot.header.commit_sequence = registry.commit_sequence();
  snapshot.header.logical_time = registry.logical_time();
  snapshot.header.observation_sequence = registry.observation_sequence();
  // The control epoch and the incarnation are the store's, not the registry's:
  // only the process that holds the durable log knows which fencing epoch it
  // published and which incarnation loaded it. They are supplied by the caller
  // and are never guessed here.
  snapshot.header.control_epoch = control_epoch;
  snapshot.header.incarnation = incarnation;

  // The event log is what makes an import provable. A snapshot captured without
  // it describes a state but cannot be replayed, and says so: a request to leave
  // the log out is honoured and reported, never filled in from another source.
  if (options.include_events) {
    snapshot.events = events;
  }
  snapshot.header.event_count = snapshot.events.size();
  snapshot.header.importable = !snapshot.events.empty();

  snapshot.objects.reserve(registry.objects().size());
  for (const std::pair<const ObjectKey, HardwareObject>& entry : registry.objects()) {
    HardwareObject object = entry.second;
    // Authority is session scoped and is not a durable fact: an imported object
    // is Recovered until an explicit attestation re-establishes it in the
    // importing process.
    object.authority = AuthorityState::Recovered;
    snapshot.objects.push_back(std::move(object));
  }

  if (options.include_history) {
    for (const std::pair<const ObjectKey, HistoryLog>& log : registry.histories()) {
      snapshot.history.insert(snapshot.history.end(), log.second.entries.begin(), log.second.entries.end());
    }
  }
  if (options.include_lineage) {
    for (const std::pair<const ObjectKey, ReplacementRecord>& record : registry.lineage().records()) {
      snapshot.lineage.push_back(record.second);
    }
  }
  if (options.include_plans) {
    for (const std::pair<const PlanKey, AppliedPlan>& plan : registry.plans()) {
      snapshot.plans.push_back(plan.second);
    }
  }
  if (options.include_health) {
    for (const std::pair<const ObjectKey, HealthSnapshot>& entry : registry.health_snapshots()) {
      HealthSnapshot copy = entry.second;
      // Freshness is a projection of the observation and the authoritative
      // logical clock at read time, never a stored fact.
      copy.freshness = Freshness::Unknown;
      snapshot.health.push_back(std::move(copy));
    }
  }

  // The counts describe what this document actually carries, so a snapshot
  // captured without one of the optional sections declares zero for it rather
  // than the size of a registry the reader cannot see.
  snapshot.header.object_count = snapshot.objects.size();
  snapshot.header.history_entry_count = snapshot.history.size();
  snapshot.header.lineage_link_count = snapshot.lineage.size();
  snapshot.header.applied_plan_count = snapshot.plans.size();
  return snapshot;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

Result<std::string> to_json(const Snapshot& snapshot, const ExportOptions& options) {
  JsonValue counts = JsonValue::make_object();
  counts.emplace("objects", JsonValue::make_number(static_cast<std::uint64_t>(snapshot.objects.size())));
  counts.emplace("history", JsonValue::make_number(static_cast<std::uint64_t>(snapshot.history.size())));
  counts.emplace("lineage", JsonValue::make_number(static_cast<std::uint64_t>(snapshot.lineage.size())));
  counts.emplace("plans", JsonValue::make_number(static_cast<std::uint64_t>(snapshot.plans.size())));
  counts.emplace("health", JsonValue::make_number(static_cast<std::uint64_t>(snapshot.health.size())));
  counts.emplace("events", JsonValue::make_number(static_cast<std::uint64_t>(snapshot.events.size())));

  JsonValue events = JsonValue::make_array();
  for (const ExportedRecord& record : snapshot.events) {
    events.push_back(render_event(record));
  }
  JsonValue objects = JsonValue::make_array();
  for (const HardwareObject& object : snapshot.objects) {
    objects.push_back(render_object(object));
  }
  JsonValue history = JsonValue::make_array();
  for (const HistoryEntry& entry : snapshot.history) {
    history.push_back(render_history(entry));
  }
  JsonValue lineage = JsonValue::make_array();
  for (const ReplacementRecord& record : snapshot.lineage) {
    lineage.push_back(render_lineage(record));
  }
  JsonValue plans = JsonValue::make_array();
  for (const AppliedPlan& plan : snapshot.plans) {
    plans.push_back(render_plan(plan));
  }
  JsonValue health = JsonValue::make_array();
  for (const HealthSnapshot& entry : snapshot.health) {
    health.push_back(render_health(entry));
  }

  JsonValue document = JsonValue::make_object();
  document.emplace("format", render_text(kDocumentFormat));
  // The document names the layout it actually has and the library build that
  // produced it. The in-memory header is not consulted for these two: a snapshot
  // may be assembled by hand or decoded from an older build, and neither may
  // make a document claim a layout or a producer it does not have.
  document.emplace("format_version", JsonValue::make_number(kExportFormatVersion));
  document.emplace("library_version", render_text(library_version_string()));
  document.emplace("state_digest", render_digest(snapshot.header.state_digest));
  document.emplace("commit_sequence", render_counter(snapshot.header.commit_sequence));
  document.emplace("logical_time", render_counter(snapshot.header.logical_time));
  document.emplace("observation_sequence", render_counter(snapshot.header.observation_sequence));
  document.emplace("control_epoch", render_counter(snapshot.header.control_epoch));
  document.emplace("incarnation", render_counter(snapshot.header.incarnation));
  document.emplace("counts", std::move(counts));
  document.emplace("events", std::move(events));
  document.emplace("event_count", JsonValue::make_number(static_cast<std::uint64_t>(snapshot.events.size())));
  // Importability describes this document, so it is derived from what the
  // document carries: a document with the durable log can be replayed, and one
  // without it cannot.
  document.emplace("importable", JsonValue::make_bool(!snapshot.events.empty()));
  document.emplace("objects", std::move(objects));
  document.emplace("history", std::move(history));
  document.emplace("lineage", std::move(lineage));
  document.emplace("plans", std::move(plans));
  document.emplace("health", std::move(health));
  return detail::write_json(document, options.pretty);
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

Result<Snapshot> from_json(std::string_view document) {
  const Result<JsonValue> parsed = detail::parse_json(document);
  if (!parsed.has_value()) {
    return parsed.error();
  }

  Failure failure;
  const JsonValue& root = parsed.value();
  Members members(failure, root, std::string_view());

  if (const JsonValue* value = members.need("format"); value != nullptr) {
    const std::string_view text = require_string(failure, *value, "format");
    if (failure.ok() && text != kDocumentFormat) {
      (void)failure.at(ErrorCode::MalformedRequest, "not a hardware lifecycle snapshot document", "format");
    }
  }

  std::uint16_t format_version = 0;
  if (const JsonValue* value = members.need("format_version"); value != nullptr) {
    const std::uint64_t raw = require_number(failure, *value, "format_version");
    if (failure.ok()) {
      if (raw > static_cast<std::uint64_t>((std::numeric_limits<std::uint16_t>::max)())) {
        (void)failure.at(ErrorCode::MalformedRequest, "format version is out of range", "format_version");
      } else {
        format_version = static_cast<std::uint16_t>(raw);
        const Result<void> supported =
            check_supported("export", kExportFormatVersion, kExportFormatVersionMinReadable, format_version);
        if (!supported.has_value()) {
          (void)failure.raise(supported.error(), "format_version");
        }
      }
    }
  }

  std::string library_version;
  if (const JsonValue* value = members.need("library_version"); value != nullptr) {
    library_version = std::string(require_string(failure, *value, "library_version"));
  }

  Digest state_digest;
  if (const JsonValue* value = members.need("state_digest"); value != nullptr) {
    state_digest = decode_digest(failure, *value, "state_digest");
  }

  CommitSequence commit_sequence;
  if (const JsonValue* value = members.need("commit_sequence"); value != nullptr) {
    commit_sequence = decode_counter<CommitSequence>(failure, *value, "commit_sequence");
  }

  LogicalTime logical_time;
  if (const JsonValue* value = members.need("logical_time"); value != nullptr) {
    logical_time = decode_counter<LogicalTime>(failure, *value, "logical_time");
  }

  ObservationSequence observation_sequence;
  if (const JsonValue* value = members.need("observation_sequence"); value != nullptr) {
    observation_sequence = decode_counter<ObservationSequence>(failure, *value, "observation_sequence");
  }

  ControlEpoch control_epoch;
  if (const JsonValue* value = members.need("control_epoch"); value != nullptr) {
    control_epoch = decode_counter<ControlEpoch>(failure, *value, "control_epoch");
  }

  IncarnationId incarnation;
  if (const JsonValue* value = members.need("incarnation"); value != nullptr) {
    incarnation = decode_counter<IncarnationId>(failure, *value, "incarnation");
  }

  std::size_t event_count = 0;
  if (const JsonValue* value = members.need("event_count"); value != nullptr) {
    event_count = static_cast<std::size_t>(require_number(failure, *value, "event_count"));
  }

  bool importable = false;
  if (const JsonValue* value = members.need("importable"); value != nullptr) {
    importable = require_bool(failure, *value, "importable");
  }

  const JsonValue* counts_json = members.need("counts");
  const JsonValue* events_json = members.need("events");
  const JsonValue* objects_json = members.need("objects");
  const JsonValue* history_json = members.need("history");
  const JsonValue* lineage_json = members.need("lineage");
  const JsonValue* plans_json = members.need("plans");
  const JsonValue* health_json = members.need("health");

  // Structure before content: an unknown member is reported before any element
  // is decoded, so a typo never hides behind a deeper failure.
  (void)members.sealed();

  const bool structure_ok = ensure_object(failure, counts_json, "counts") &&
                            ensure_array(failure, events_json, "events") &&
                            ensure_array(failure, objects_json, "objects") &&
                            ensure_array(failure, history_json, "history") &&
                            ensure_array(failure, lineage_json, "lineage") &&
                            ensure_array(failure, plans_json, "plans") &&
                            ensure_array(failure, health_json, "health");

  if (structure_ok) {
    const std::size_t observed[kCollections] = {objects_json->items().size(), history_json->items().size(),
                                                lineage_json->items().size(), plans_json->items().size(),
                                                health_json->items().size(),  events_json->items().size()};
    if (check_bounds(failure, observed) && check_counts(failure, *counts_json, observed)) {
      // The declared event count and the importability flag describe the same
      // fact as the event array: a document that carries a log is importable,
      // and one that does not is not.
      if (event_count != observed[kEvents]) {
        (void)failure.at(ErrorCode::MalformedRequest, "declared event count does not match the event array",
                         "event_count");
      } else if (importable != (observed[kEvents] != 0)) {
        (void)failure.at(ErrorCode::MalformedRequest,
                         "importable must be true exactly when the document carries its event log", "importable");
      }
    }
  }

  std::vector<ExportedRecord> events;
  std::vector<HardwareObject> objects;
  std::vector<HistoryEntry> history;
  std::vector<ReplacementRecord> lineage;
  std::vector<AppliedPlan> plans;
  std::vector<HealthSnapshot> health;
  std::set<ObjectKey> object_keys;

  if (failure.ok()) {
    const std::vector<JsonValue>& items = events_json->items();
    events.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
      events.push_back(decode_event(failure, items[index], element_path("events", index)));
      if (!failure.ok()) {
        break;
      }
    }
  }

  if (failure.ok()) {
    const std::vector<JsonValue>& items = objects_json->items();
    objects.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
      const std::string path = element_path("objects", index);
      HardwareObject object = decode_object(failure, items[index], path);
      if (!failure.ok()) {
        break;
      }
      if (!object_keys.insert(object.key).second) {
        (void)failure.at(ErrorCode::MalformedRequest, "duplicate object key", path);
        break;
      }
      objects.push_back(std::move(object));
    }
  }

  if (failure.ok()) {
    const std::vector<JsonValue>& items = history_json->items();
    history.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
      history.push_back(decode_history(failure, items[index], element_path("history", index)));
      if (!failure.ok()) {
        break;
      }
    }
    (void)check_history_links(failure, history, object_keys);
  }

  if (failure.ok()) {
    const std::vector<JsonValue>& items = lineage_json->items();
    lineage.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
      lineage.push_back(decode_lineage(failure, items[index], element_path("lineage", index)));
      if (!failure.ok()) {
        break;
      }
    }
    (void)check_lineage_links(failure, lineage, object_keys);
  }

  if (failure.ok()) {
    const std::vector<JsonValue>& items = plans_json->items();
    plans.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
      plans.push_back(decode_plan(failure, items[index], element_path("plans", index)));
      if (!failure.ok()) {
        break;
      }
    }
    (void)check_plan_keys(failure, plans);
  }

  if (failure.ok()) {
    const std::vector<JsonValue>& items = health_json->items();
    health.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
      health.push_back(decode_health(failure, items[index], element_path("health", index)));
      if (!failure.ok()) {
        break;
      }
    }
    (void)check_health_links(failure, health, object_keys);
  }

  if (!failure.ok()) {
    return failure.error();
  }

  Snapshot snapshot;
  snapshot.header.format_version = format_version;
  snapshot.header.library_version = std::move(library_version);
  snapshot.header.state_digest = state_digest;
  snapshot.header.commit_sequence = commit_sequence;
  snapshot.header.logical_time = logical_time;
  snapshot.header.observation_sequence = observation_sequence;
  snapshot.header.control_epoch = control_epoch;
  snapshot.header.incarnation = incarnation;
  snapshot.header.object_count = objects.size();
  snapshot.header.history_entry_count = history.size();
  snapshot.header.lineage_link_count = lineage.size();
  snapshot.header.applied_plan_count = plans.size();
  snapshot.header.event_count = events.size();
  snapshot.header.importable = importable;
  // The document's order is preserved: the reader is a faithful decode, and the
  // canonical orderings belong to the digest and diff paths, which sort the
  // copies they hash or compare.
  snapshot.events = std::move(events);
  snapshot.objects = std::move(objects);
  snapshot.history = std::move(history);
  snapshot.lineage = std::move(lineage);
  snapshot.plans = std::move(plans);
  snapshot.health = std::move(health);
  return snapshot;
}

// ---------------------------------------------------------------------------
// State digest
// ---------------------------------------------------------------------------

Digest compute_snapshot_state_digest(const Snapshot& snapshot) {
  // The state digest covers the state, never the log. The event log has its own
  // digest, computed by the store over the records it published, and two
  // documents that describe the same state must agree here whether or not either
  // of them can be replayed.
  std::vector<HardwareObject> objects = snapshot.objects;
  std::sort(objects.begin(), objects.end(),
            [](const HardwareObject& left, const HardwareObject& right) { return left.key < right.key; });

  std::vector<HistoryEntry> history = snapshot.history;
  std::sort(history.begin(), history.end(), [](const HistoryEntry& left, const HistoryEntry& right) {
    if (left.key != right.key) {
      return left.key < right.key;
    }
    return left.commit_sequence < right.commit_sequence;
  });

  std::vector<ReplacementRecord> lineage = snapshot.lineage;
  std::sort(lineage.begin(), lineage.end(), [](const ReplacementRecord& left, const ReplacementRecord& right) {
    return left.predecessor < right.predecessor;
  });

  std::vector<AppliedPlan> plans = snapshot.plans;
  std::sort(plans.begin(), plans.end(),
            [](const AppliedPlan& left, const AppliedPlan& right) { return left.key < right.key; });

  std::vector<HealthSnapshot> health = snapshot.health;
  std::sort(health.begin(), health.end(), [](const HealthSnapshot& left, const HealthSnapshot& right) {
    return left.observation.key < right.observation.key;
  });

  const std::vector<std::uint8_t> bytes =
      detail::canonical_state_bytes(snapshot.header.commit_sequence, snapshot.header.logical_time,
                                    snapshot.header.observation_sequence, objects, history, lineage, plans, health);
  return Sha256::hash(bytes.data(), bytes.size());
}

}  // namespace hardware_lifecycle
