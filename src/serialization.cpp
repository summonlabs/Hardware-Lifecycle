// Hardware Lifecycle - canonical serialization and record replay.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This translation unit is the only place that knows how durable bytes are
// laid out, and it is also the only place that knows how a record changes the
// authoritative state. That is deliberate: the live path and the recovery path
// both call apply_payload, so a committed state and a replayed state cannot
// disagree by construction.

#include "serialization.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

#include "hardware_lifecycle/text.hpp"

namespace hardware_lifecycle::detail {
namespace {

constexpr std::uint8_t u8_of(RecordKind kind) noexcept { return static_cast<std::uint8_t>(kind); }
constexpr std::uint8_t u8_of(LifecycleState state) noexcept { return static_cast<std::uint8_t>(state); }
constexpr std::uint8_t u8_of(TransitionReason reason) noexcept { return static_cast<std::uint8_t>(reason); }
constexpr std::uint8_t u8_of(EligibilityGate gate) noexcept { return static_cast<std::uint8_t>(gate); }
constexpr std::uint8_t u8_of(EvidenceKind kind) noexcept { return static_cast<std::uint8_t>(kind); }
constexpr std::uint8_t u8_of(ActorKind kind) noexcept { return static_cast<std::uint8_t>(kind); }
constexpr std::uint8_t u8_of(HardwareKind kind) noexcept { return static_cast<std::uint8_t>(kind); }
constexpr std::uint8_t u8_of(HealthStatus status) noexcept { return static_cast<std::uint8_t>(status); }
constexpr std::uint8_t u8_of(ReadinessStatus status) noexcept { return static_cast<std::uint8_t>(status); }
constexpr std::uint8_t u8_of(AvailabilityStatus status) noexcept { return static_cast<std::uint8_t>(status); }

[[nodiscard]] Result<void> require_end(const Reader& reader, const char* what) {
  if (!reader.ok()) {
    return reader.error();
  }
  if (!reader.at_end()) {
    return Error::make(ErrorCode::TrailingBytes, std::string("trailing bytes after the ") + what + " payload")
        .with("trailing", std::to_string(reader.remaining()));
  }
  return ok();
}

[[nodiscard]] Result<void> verify_enum_u8(std::uint8_t raw, std::uint8_t count, const char* what) {
  if (raw >= count) {
    return Error::make(ErrorCode::MalformedRequest, std::string("invalid ") + what + " value in encoded data")
        .with(what, std::to_string(raw));
  }
  return ok();
}

void encode_provenance_identity(Writer& writer, const Provenance& provenance) {
  writer.text(provenance.actor.id.value());
  writer.u8(u8_of(provenance.actor.kind));
  writer.u32(provenance.authority.bits());
  writer.u64(provenance.policy_generation.value());
  writer.text(provenance.plan.value());
  writer.text(provenance.attempt.value());
  writer.u32(static_cast<std::uint32_t>(provenance.evidence.size()));
  for (const EvidenceRef& evidence : provenance.evidence) {
    encode_evidence(writer, evidence);
  }
}

[[nodiscard]] Result<Provenance> decode_provenance(Reader& reader) {
  Provenance provenance;
  const Result<ActorId> actor = ActorId::parse(reader.text(limits::kMaxIdentifierBytes));
  if (!actor.has_value()) {
    reader.fail(ErrorCode::MalformedRequest, "invalid actor id in encoded provenance");
    return actor.error();
  }
  provenance.actor.id = actor.value();
  const std::uint8_t actor_kind = reader.u8();
  if (!verify_enum_u8(actor_kind, static_cast<std::uint8_t>(ActorKind::Recovery) + 1u, "actor kind")) {
    reader.fail(ErrorCode::MalformedRequest, "invalid actor kind in encoded provenance");
    return reader.error();
  }
  provenance.actor.kind = static_cast<ActorKind>(actor_kind);
  provenance.authority = AuthorityMask(reader.u32());
  if ((provenance.authority.bits() & ~authority_all().bits()) != 0u) {
    reader.fail(ErrorCode::MalformedRequest, "authority mask carries bits outside the declared scopes");
    return reader.error();
  }
  provenance.policy_generation = PolicyGeneration::from_value(reader.u64());
  const Result<PlanId> plan = PlanId::parse(reader.text(limits::kMaxIdentifierBytes));
  if (!plan.has_value()) {
    reader.fail(ErrorCode::MalformedRequest, "invalid plan id in encoded provenance");
    return plan.error();
  }
  provenance.plan = plan.value();
  const Result<AttemptId> attempt = AttemptId::parse(reader.text(limits::kMaxIdentifierBytes));
  if (!attempt.has_value()) {
    reader.fail(ErrorCode::MalformedRequest, "invalid attempt id in encoded provenance");
    return attempt.error();
  }
  provenance.attempt = attempt.value();
  const std::uint32_t count = reader.u32();
  if (!reader.ok()) {
    return reader.error();
  }
  if (count > limits::kMaxEvidencePerRequest) {
    reader.fail(ErrorCode::LimitExceeded, "encoded provenance carries too many evidence references");
    return reader.error();
  }
  provenance.evidence.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    EvidenceRef evidence;
    const std::uint8_t kind = reader.u8();
    if (!verify_enum_u8(kind, static_cast<std::uint8_t>(EvidenceKind::RecoveryAttestation) + 1u, "evidence kind")) {
      reader.fail(ErrorCode::MalformedRequest, "invalid evidence kind in encoded provenance");
      return reader.error();
    }
    evidence.kind = static_cast<EvidenceKind>(kind);
    evidence.digest = reader.digest();
    evidence.source = reader.text();
    evidence.observed_sequence = ObservationSequence::from_value(reader.u64());
    if (!reader.ok()) {
      return reader.error();
    }
    provenance.evidence.push_back(std::move(evidence));
  }
  provenance.logical_time = LogicalTime::from_value(reader.u64());
  if (reader.presence()) {
    const Result<WallClock> clock = WallClock::parse(reader.text());
    if (!clock.has_value()) {
      reader.fail(ErrorCode::MalformedRequest, "invalid wall clock in encoded provenance");
      return clock.error();
    }
    provenance.wall_clock = clock.value();
  }
  if (!reader.ok()) {
    return reader.error();
  }
  return provenance;
}

}  // namespace

// ---------------------------------------------------------------------------
// Field encoders
// ---------------------------------------------------------------------------

void encode_evidence(Writer& writer, const EvidenceRef& evidence) {
  writer.u8(u8_of(evidence.kind));
  writer.digest(evidence.digest);
  writer.text(evidence.source);
  writer.u64(evidence.observed_sequence.value());
}

void encode_provenance(Writer& writer, const Provenance& provenance) {
  encode_provenance_identity(writer, provenance);
  writer.u64(provenance.logical_time.value());
  writer.optional_text(provenance.wall_clock.has_value(), provenance.wall_clock.value());
}

void encode_location(Writer& writer, const Location& location) {
  writer.text(location.site.value());
  writer.text(location.rack.value());
  writer.text(location.slot.value());
}

void encode_object_key(Writer& writer, const ObjectKey& key) {
  writer.text(key.asset.value());
  writer.u64(key.hardware_generation.value());
}

void encode_object(Writer& writer, const HardwareObject& object) {
  encode_object_key(writer, object.key);
  writer.u8(u8_of(object.kind));
  writer.text(object.model.value());
  writer.u64(object.firmware_generation.value());
  writer.u64(object.lifecycle_generation.value());
  writer.u8(u8_of(object.state));
  writer.u64(object.revision.value());
  writer.u8(object.location.has_value() ? 1u : 0u);
  if (object.location.has_value()) {
    encode_location(writer, object.location.value());
  }
  writer.u8(u8_of(object.eligibility_gate));
  writer.u8(object.predecessor.has_value() ? 1u : 0u);
  if (object.predecessor.has_value()) {
    encode_object_key(writer, object.predecessor.value());
  }
  writer.u8(object.successor.has_value() ? 1u : 0u);
  if (object.successor.has_value()) {
    encode_object_key(writer, object.successor.value());
  }
  writer.u64(object.replacement_generation.value());
  writer.u64(object.created_at.value());
  writer.u64(object.updated_at.value());
  writer.u64(object.history_entries);
  // object.authority is intentionally absent: it is session scoped and is never
  // a durable fact.
}

void encode_history_entry(Writer& writer, const HistoryEntry& entry) {
  writer.u64(entry.commit_sequence.value());
  writer.u64(entry.logical_time.value());
  encode_object_key(writer, entry.key);
  writer.u64(entry.lifecycle_generation.value());
  writer.u64(entry.revision_before.value());
  writer.u64(entry.revision_after.value());
  writer.u8(u8_of(entry.from));
  writer.u8(u8_of(entry.to));
  writer.u8(u8_of(entry.reason));
  encode_provenance(writer, entry.provenance);
  writer.digest(entry.receipt_digest);
  writer.digest(entry.chain_digest);
}

void encode_lineage(Writer& writer, const ReplacementRecord& record) {
  encode_object_key(writer, record.predecessor);
  encode_object_key(writer, record.successor);
  writer.u8(u8_of(record.reason));
  encode_provenance(writer, record.provenance);
  writer.u64(record.linked_at.value());
  writer.u64(record.commit_sequence.value());
  writer.u64(record.replacement_generation.value());
  writer.digest(record.link_digest);
}

void encode_receipt(Writer& writer, const Receipt& receipt) {
  std::visit(
      [&writer](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, CreateReceipt>) {
          writer.u8(1);
          writer.text(typed.plan.value());
          writer.text(typed.attempt.value());
          encode_object_key(writer, typed.key);
          writer.u8(u8_of(typed.state));
          writer.u64(typed.lifecycle_generation.value());
          writer.u64(typed.revision.value());
          writer.u64(typed.commit_sequence.value());
          writer.u64(typed.logical_time.value());
        } else if constexpr (std::is_same_v<T, TransitionReceipt>) {
          writer.u8(2);
          writer.text(typed.plan.value());
          writer.text(typed.attempt.value());
          encode_object_key(writer, typed.key);
          writer.u8(u8_of(typed.from));
          writer.u8(u8_of(typed.to));
          writer.u8(u8_of(typed.reason));
          writer.u64(typed.revision_before.value());
          writer.u64(typed.revision_after.value());
          writer.u64(typed.lifecycle_generation.value());
          writer.u8(u8_of(typed.gate_after));
          writer.u64(typed.commit_sequence.value());
          writer.u64(typed.logical_time.value());
          writer.digest(typed.request_digest);
          // entry_digest is a cross reference derived from this digest and is
          // therefore excluded from it.
        } else if constexpr (std::is_same_v<T, ReplacementReceipt>) {
          writer.u8(3);
          writer.text(typed.plan.value());
          writer.text(typed.attempt.value());
          encode_object_key(writer, typed.predecessor);
          encode_object_key(writer, typed.successor);
          writer.u64(typed.replacement_generation.value());
          writer.u64(typed.commit_sequence.value());
          writer.u64(typed.logical_time.value());
          writer.digest(typed.link_digest);
          writer.digest(typed.request_digest);
        } else if constexpr (std::is_same_v<T, EligibilityGateReceipt>) {
          writer.u8(4);
          writer.text(typed.plan.value());
          writer.text(typed.attempt.value());
          encode_object_key(writer, typed.key);
          writer.u8(u8_of(typed.before));
          writer.u8(u8_of(typed.after));
          writer.u64(typed.revision.value());
          writer.u64(typed.commit_sequence.value());
          writer.u64(typed.logical_time.value());
          writer.digest(typed.request_digest);
        } else if constexpr (std::is_same_v<T, HealthReceipt>) {
          writer.u8(5);
          writer.text(typed.plan.value());
          writer.text(typed.attempt.value());
          encode_object_key(writer, typed.key);
          writer.u64(typed.sequence.value());
          writer.u64(typed.commit_sequence.value());
          writer.u64(typed.logical_time.value());
          writer.digest(typed.request_digest);
        } else {
          writer.u8(6);
          writer.text(typed.plan.value());
          writer.text(typed.attempt.value());
          encode_object_key(writer, typed.key);
          writer.u64(typed.revision.value());
          writer.u64(typed.commit_sequence.value());
          writer.u64(typed.logical_time.value());
          writer.digest(typed.request_digest);
        }
      },
      receipt);
}

void encode_plan(Writer& writer, const AppliedPlan& plan) {
  writer.text(plan.key.plan.value());
  writer.text(plan.key.attempt.value());
  writer.u16(static_cast<std::uint16_t>(plan.kind));
  writer.digest(plan.request_digest);
  encode_receipt(writer, plan.receipt);
  writer.u64(plan.commit_sequence.value());
  writer.u64(plan.logical_time.value());
}

void encode_health(Writer& writer, const HealthSnapshot& snapshot) {
  writer.u8(snapshot.present ? 1u : 0u);
  if (snapshot.present) {
    encode_object_key(writer, snapshot.observation.key);
    writer.u8(u8_of(snapshot.observation.health));
    writer.u8(u8_of(snapshot.observation.readiness));
    writer.u8(u8_of(snapshot.observation.availability));
    writer.digest(snapshot.observation.evidence_digest);
    writer.text(snapshot.observation.source);
    writer.optional_text(snapshot.observation.observed_wall_clock.has_value(),
                         snapshot.observation.observed_wall_clock.value());
  }
  writer.u64(snapshot.sequence.value());
  writer.u64(snapshot.recorded_at.value());
  // snapshot.freshness is derived at read time and is never durable.
}

// ---------------------------------------------------------------------------
// State image
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> canonical_state_bytes(CommitSequence commit_sequence, LogicalTime logical_time,
                                                ObservationSequence observation_sequence,
                                                const std::vector<HardwareObject>& objects,
                                                const std::vector<HistoryEntry>& history,
                                                const std::vector<ReplacementRecord>& lineage,
                                                const std::vector<AppliedPlan>& plans,
                                                const std::vector<HealthSnapshot>& health) {
  Writer writer;
  writer.text(kStateImageTag);
  writer.u64(commit_sequence.value());
  writer.u64(logical_time.value());
  writer.u64(observation_sequence.value());
  writer.u64(static_cast<std::uint64_t>(objects.size()));
  for (const HardwareObject& object : objects) {
    encode_object(writer, object);
  }
  writer.u64(static_cast<std::uint64_t>(history.size()));
  for (const HistoryEntry& entry : history) {
    encode_history_entry(writer, entry);
  }
  writer.u64(static_cast<std::uint64_t>(lineage.size()));
  for (const ReplacementRecord& record : lineage) {
    encode_lineage(writer, record);
  }
  writer.u64(static_cast<std::uint64_t>(plans.size()));
  for (const AppliedPlan& plan : plans) {
    encode_plan(writer, plan);
  }
  writer.u64(static_cast<std::uint64_t>(health.size()));
  for (const HealthSnapshot& snapshot : health) {
    encode_health(writer, snapshot);
  }
  return writer.take();
}

// ---------------------------------------------------------------------------
// Record payloads
// ---------------------------------------------------------------------------

RecordKind record_kind_of(const RecordPayload& payload) noexcept {
  if (std::holds_alternative<ObjectRegisteredRecord>(payload)) {
    return RecordKind::ObjectRegistered;
  }
  if (std::holds_alternative<TransitionAppliedRecord>(payload)) {
    return RecordKind::TransitionApplied;
  }
  if (std::holds_alternative<SuccessorLinkedRecord>(payload)) {
    return RecordKind::SuccessorLinked;
  }
  if (std::holds_alternative<EligibilityGateChangedRecord>(payload)) {
    return RecordKind::EligibilityGateChanged;
  }
  if (std::holds_alternative<HealthObservedRecord>(payload)) {
    return RecordKind::HealthObserved;
  }
  return RecordKind::AuthorityAttested;
}

Result<std::vector<std::uint8_t>> encode_payload(const RecordPayload& payload) {
  Writer writer;
  writer.text(kRecordPayloadTag);
  writer.u8(u8_of(record_kind_of(payload)));
  std::visit(
      [&writer](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, ObjectRegisteredRecord>) {
          encode_object(writer, typed.object);
          encode_provenance(writer, typed.provenance);
          writer.digest(typed.request_digest);
          writer.digest(typed.receipt_digest);
          writer.digest(typed.entry_chain_digest);
        } else if constexpr (std::is_same_v<T, TransitionAppliedRecord>) {
          encode_object_key(writer, typed.key);
          writer.u8(u8_of(typed.from));
          writer.u8(u8_of(typed.to));
          writer.u8(u8_of(typed.reason));
          writer.u64(typed.revision_before.value());
          writer.u64(typed.revision_after.value());
          writer.u64(typed.lifecycle_generation_before.value());
          writer.u64(typed.lifecycle_generation_after.value());
          writer.u8(u8_of(typed.gate_before));
          writer.u8(u8_of(typed.gate_after));
          writer.u8(typed.location_after.has_value() ? 1u : 0u);
          if (typed.location_after.has_value()) {
            encode_location(writer, typed.location_after.value());
          }
          writer.u8(typed.successor.has_value() ? 1u : 0u);
          if (typed.successor.has_value()) {
            encode_object_key(writer, typed.successor.value());
          }
          encode_provenance(writer, typed.provenance);
          writer.digest(typed.request_digest);
          writer.digest(typed.receipt_digest);
          writer.digest(typed.entry_chain_digest);
        } else if constexpr (std::is_same_v<T, SuccessorLinkedRecord>) {
          encode_lineage(writer, typed.link);
          encode_provenance(writer, typed.provenance);
          writer.digest(typed.request_digest);
          writer.digest(typed.receipt_digest);
        } else if constexpr (std::is_same_v<T, EligibilityGateChangedRecord>) {
          encode_object_key(writer, typed.key);
          writer.u8(u8_of(typed.before));
          writer.u8(u8_of(typed.after));
          writer.u64(typed.revision_before.value());
          writer.u64(typed.revision_after.value());
          encode_provenance(writer, typed.provenance);
          writer.digest(typed.request_digest);
          writer.digest(typed.receipt_digest);
          writer.digest(typed.entry_chain_digest);
        } else if constexpr (std::is_same_v<T, HealthObservedRecord>) {
          encode_object_key(writer, typed.observation.key);
          writer.u8(u8_of(typed.observation.health));
          writer.u8(u8_of(typed.observation.readiness));
          writer.u8(u8_of(typed.observation.availability));
          writer.digest(typed.observation.evidence_digest);
          writer.text(typed.observation.source);
          writer.optional_text(typed.observation.observed_wall_clock.has_value(),
                               typed.observation.observed_wall_clock.value());
          writer.u64(typed.sequence.value());
          encode_provenance(writer, typed.provenance);
          writer.digest(typed.request_digest);
          writer.digest(typed.receipt_digest);
        } else {
          encode_object_key(writer, typed.key);
          writer.u64(typed.revision.value());
          encode_provenance(writer, typed.provenance);
          writer.digest(typed.request_digest);
          writer.digest(typed.receipt_digest);
        }
      },
      payload);
  return writer.take();
}

namespace {

[[nodiscard]] Result<ObjectKey> decode_object_key(Reader& reader) {
  const Result<AssetId> asset = AssetId::parse(reader.text(limits::kMaxIdentifierBytes));
  if (!asset.has_value()) {
    return asset.error();
  }
  const std::uint64_t generation = reader.u64();
  if (!reader.ok()) {
    return reader.error();
  }
  if (generation == 0) {
    return Error::make(ErrorCode::MalformedRequest, "hardware generation must be at least 1 in encoded data");
  }
  ObjectKey key;
  key.asset = asset.value();
  key.hardware_generation = HardwareGeneration::from_value(generation);
  return key;
}

[[nodiscard]] Result<Location> decode_location(Reader& reader) {
  Location location;
  const Result<SiteId> site = SiteId::parse(reader.text(limits::kMaxIdentifierBytes));
  if (!site.has_value()) {
    return site.error();
  }
  const Result<RackId> rack = RackId::parse(reader.text(limits::kMaxIdentifierBytes));
  if (!rack.has_value()) {
    return rack.error();
  }
  const Result<SlotId> slot = SlotId::parse(reader.text(limits::kMaxIdentifierBytes));
  if (!slot.has_value()) {
    return slot.error();
  }
  location.site = site.value();
  location.rack = rack.value();
  location.slot = slot.value();
  return location;
}

[[nodiscard]] Result<HardwareObject> decode_object(Reader& reader) {
  HardwareObject object;
  const Result<ObjectKey> key = decode_object_key(reader);
  if (!key.has_value()) {
    return key.error();
  }
  object.key = key.value();
  const std::uint8_t kind = reader.u8();
  if (!verify_enum_u8(kind, static_cast<std::uint8_t>(HardwareKind::Other) + 1u, "hardware kind")) {
    return Error::make(ErrorCode::MalformedRequest, "invalid hardware kind in encoded object");
  }
  object.kind = static_cast<HardwareKind>(kind);
  const Result<ModelId> model = ModelId::parse(reader.text(limits::kMaxIdentifierBytes));
  if (!model.has_value()) {
    return model.error();
  }
  object.model = model.value();
  object.firmware_generation = FirmwareGeneration::from_value(reader.u64());
  object.lifecycle_generation = LifecycleGeneration::from_value(reader.u64());
  const std::uint8_t state = reader.u8();
  if (!verify_enum_u8(state, static_cast<std::uint8_t>(LifecycleState::Removed) + 1u, "lifecycle state")) {
    return Error::make(ErrorCode::MalformedRequest, "invalid lifecycle state in encoded object");
  }
  object.state = static_cast<LifecycleState>(state);
  object.revision = Revision::from_value(reader.u64());
  if (reader.presence()) {
    Result<Location> location = decode_location(reader);
    if (!location.has_value()) {
      return location.error();
    }
    object.location = location.value();
  }
  const std::uint8_t gate = reader.u8();
  if (!verify_enum_u8(gate, static_cast<std::uint8_t>(EligibilityGate::Closed) + 1u, "eligibility gate")) {
    return Error::make(ErrorCode::MalformedRequest, "invalid eligibility gate in encoded object");
  }
  object.eligibility_gate = static_cast<EligibilityGate>(gate);
  if (reader.presence()) {
    Result<ObjectKey> predecessor = decode_object_key(reader);
    if (!predecessor.has_value()) {
      return predecessor.error();
    }
    object.predecessor = predecessor.value();
  }
  if (reader.presence()) {
    Result<ObjectKey> successor = decode_object_key(reader);
    if (!successor.has_value()) {
      return successor.error();
    }
    object.successor = successor.value();
  }
  object.replacement_generation = ReplacementGeneration::from_value(reader.u64());
  object.created_at = LogicalTime::from_value(reader.u64());
  object.updated_at = LogicalTime::from_value(reader.u64());
  object.history_entries = reader.u64();
  if (!reader.ok()) {
    return reader.error();
  }
  if (!object.revision.valid() || !object.lifecycle_generation.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "encoded object carries a zero revision or generation");
  }
  return object;
}

[[nodiscard]] Result<ReplacementRecord> decode_lineage(Reader& reader) {
  ReplacementRecord record;
  const Result<ObjectKey> predecessor = decode_object_key(reader);
  if (!predecessor.has_value()) {
    return predecessor.error();
  }
  const Result<ObjectKey> successor = decode_object_key(reader);
  if (!successor.has_value()) {
    return successor.error();
  }
  record.predecessor = predecessor.value();
  record.successor = successor.value();
  const std::uint8_t reason = reader.u8();
  if (!verify_enum_u8(reason, static_cast<std::uint8_t>(TransitionReason::PhysicallyRemoved) + 1u, "reason")) {
    return Error::make(ErrorCode::MalformedRequest, "invalid reason in encoded lineage record");
  }
  record.reason = static_cast<TransitionReason>(reason);
  Result<Provenance> provenance = decode_provenance(reader);
  if (!provenance.has_value()) {
    return provenance.error();
  }
  record.provenance = provenance.value();
  record.linked_at = LogicalTime::from_value(reader.u64());
  record.commit_sequence = CommitSequence::from_value(reader.u64());
  record.replacement_generation = ReplacementGeneration::from_value(reader.u64());
  record.link_digest = reader.digest();
  if (!reader.ok()) {
    return reader.error();
  }
  return record;
}

}  // namespace

Result<RecordPayload> decode_payload(RecordKind kind, std::span<const std::uint8_t> bytes) {
  Reader reader(bytes, ErrorCode::IntegrityFailure);
  const std::string tag = reader.text(16);
  if (!reader.ok() || tag != kRecordPayloadTag) {
    return Error::make(ErrorCode::IntegrityFailure, "record payload does not carry the expected domain tag");
  }
  const std::uint8_t declared = reader.u8();
  if (!reader.ok() || declared != u8_of(kind)) {
    return Error::make(ErrorCode::IntegrityFailure, "record payload kind disagrees with the record frame kind");
  }

  switch (kind) {
    case RecordKind::ObjectRegistered: {
      ObjectRegisteredRecord record;
      Result<HardwareObject> object = decode_object(reader);
      if (!object.has_value()) {
        return object.error();
      }
      Result<Provenance> provenance = decode_provenance(reader);
      if (!provenance.has_value()) {
        return provenance.error();
      }
      record.object = std::move(object.value());
      record.provenance = std::move(provenance.value());
      record.request_digest = reader.digest();
      record.receipt_digest = reader.digest();
      record.entry_chain_digest = reader.digest();
      Result<void> end = require_end(reader, "object registration");
      if (!end.has_value()) {
        return end.error();
      }
      return RecordPayload(record);
    }
    case RecordKind::TransitionApplied: {
      TransitionAppliedRecord record;
      Result<ObjectKey> key = decode_object_key(reader);
      if (!key.has_value()) {
        return key.error();
      }
      record.key = key.value();
      const std::uint8_t from = reader.u8();
      const std::uint8_t to = reader.u8();
      const std::uint8_t reason = reader.u8();
      if (!verify_enum_u8(from, static_cast<std::uint8_t>(LifecycleState::Removed) + 1u, "from state") ||
          !verify_enum_u8(to, static_cast<std::uint8_t>(LifecycleState::Removed) + 1u, "to state") ||
          !verify_enum_u8(reason, static_cast<std::uint8_t>(TransitionReason::PhysicallyRemoved) + 1u, "reason")) {
        return Error::make(ErrorCode::MalformedRequest, "invalid state or reason in encoded transition");
      }
      record.from = static_cast<LifecycleState>(from);
      record.to = static_cast<LifecycleState>(to);
      record.reason = static_cast<TransitionReason>(reason);
      record.revision_before = Revision::from_value(reader.u64());
      record.revision_after = Revision::from_value(reader.u64());
      record.lifecycle_generation_before = LifecycleGeneration::from_value(reader.u64());
      record.lifecycle_generation_after = LifecycleGeneration::from_value(reader.u64());
      const std::uint8_t gate_before = reader.u8();
      const std::uint8_t gate_after = reader.u8();
      if (!verify_enum_u8(gate_before, static_cast<std::uint8_t>(EligibilityGate::Closed) + 1u, "gate") ||
          !verify_enum_u8(gate_after, static_cast<std::uint8_t>(EligibilityGate::Closed) + 1u, "gate")) {
        return Error::make(ErrorCode::MalformedRequest, "invalid eligibility gate in encoded transition");
      }
      record.gate_before = static_cast<EligibilityGate>(gate_before);
      record.gate_after = static_cast<EligibilityGate>(gate_after);
      if (reader.presence()) {
        Result<Location> location = decode_location(reader);
        if (!location.has_value()) {
          return location.error();
        }
        record.location_after = location.value();
      }
      if (reader.presence()) {
        Result<ObjectKey> successor = decode_object_key(reader);
        if (!successor.has_value()) {
          return successor.error();
        }
        record.successor = successor.value();
      }
      Result<Provenance> provenance = decode_provenance(reader);
      if (!provenance.has_value()) {
        return provenance.error();
      }
      record.provenance = std::move(provenance.value());
      record.request_digest = reader.digest();
      record.receipt_digest = reader.digest();
      record.entry_chain_digest = reader.digest();
      Result<void> end = require_end(reader, "transition");
      if (!end.has_value()) {
        return end.error();
      }
      return RecordPayload(record);
    }
    case RecordKind::SuccessorLinked: {
      SuccessorLinkedRecord record;
      Result<ReplacementRecord> link = decode_lineage(reader);
      if (!link.has_value()) {
        return link.error();
      }
      Result<Provenance> provenance = decode_provenance(reader);
      if (!provenance.has_value()) {
        return provenance.error();
      }
      record.link = std::move(link.value());
      record.provenance = std::move(provenance.value());
      record.request_digest = reader.digest();
      record.receipt_digest = reader.digest();
      Result<void> end = require_end(reader, "successor link");
      if (!end.has_value()) {
        return end.error();
      }
      return RecordPayload(record);
    }
    case RecordKind::EligibilityGateChanged: {
      EligibilityGateChangedRecord record;
      Result<ObjectKey> key = decode_object_key(reader);
      if (!key.has_value()) {
        return key.error();
      }
      record.key = key.value();
      const std::uint8_t before = reader.u8();
      const std::uint8_t after = reader.u8();
      if (!verify_enum_u8(before, static_cast<std::uint8_t>(EligibilityGate::Closed) + 1u, "gate") ||
          !verify_enum_u8(after, static_cast<std::uint8_t>(EligibilityGate::Closed) + 1u, "gate")) {
        return Error::make(ErrorCode::MalformedRequest, "invalid eligibility gate in encoded gate change");
      }
      record.before = static_cast<EligibilityGate>(before);
      record.after = static_cast<EligibilityGate>(after);
      record.revision_before = Revision::from_value(reader.u64());
      record.revision_after = Revision::from_value(reader.u64());
      Result<Provenance> provenance = decode_provenance(reader);
      if (!provenance.has_value()) {
        return provenance.error();
      }
      record.provenance = std::move(provenance.value());
      record.request_digest = reader.digest();
      record.receipt_digest = reader.digest();
      record.entry_chain_digest = reader.digest();
      Result<void> end = require_end(reader, "gate change");
      if (!end.has_value()) {
        return end.error();
      }
      return RecordPayload(record);
    }
    case RecordKind::HealthObserved: {
      HealthObservedRecord record;
      Result<ObjectKey> key = decode_object_key(reader);
      if (!key.has_value()) {
        return key.error();
      }
      record.observation.key = key.value();
      const std::uint8_t health = reader.u8();
      const std::uint8_t readiness = reader.u8();
      const std::uint8_t availability = reader.u8();
      if (!verify_enum_u8(health, static_cast<std::uint8_t>(HealthStatus::Failed) + 1u, "health") ||
          !verify_enum_u8(readiness, static_cast<std::uint8_t>(ReadinessStatus::NotReady) + 1u, "readiness") ||
          !verify_enum_u8(availability, static_cast<std::uint8_t>(AvailabilityStatus::Unavailable) + 1u,
                          "availability")) {
        return Error::make(ErrorCode::MalformedRequest, "invalid health enumeration in encoded observation");
      }
      record.observation.health = static_cast<HealthStatus>(health);
      record.observation.readiness = static_cast<ReadinessStatus>(readiness);
      record.observation.availability = static_cast<AvailabilityStatus>(availability);
      record.observation.evidence_digest = reader.digest();
      record.observation.source = reader.text();
      if (reader.presence()) {
        Result<WallClock> clock = WallClock::parse(reader.text());
        if (!clock.has_value()) {
          return clock.error();
        }
        record.observation.observed_wall_clock = clock.value();
      }
      record.sequence = ObservationSequence::from_value(reader.u64());
      Result<Provenance> provenance = decode_provenance(reader);
      if (!provenance.has_value()) {
        return provenance.error();
      }
      record.provenance = std::move(provenance.value());
      record.request_digest = reader.digest();
      record.receipt_digest = reader.digest();
      Result<void> end = require_end(reader, "health observation");
      if (!end.has_value()) {
        return end.error();
      }
      if (!record.sequence.valid()) {
        return Error::make(ErrorCode::MalformedRequest, "health observation carries a zero sequence");
      }
      return RecordPayload(record);
    }
    case RecordKind::AuthorityAttested:
      break;
  }

  AuthorityAttestedRecord record;
  Result<ObjectKey> key = decode_object_key(reader);
  if (!key.has_value()) {
    return key.error();
  }
  record.key = key.value();
  record.revision = Revision::from_value(reader.u64());
  Result<Provenance> provenance = decode_provenance(reader);
  if (!provenance.has_value()) {
    return provenance.error();
  }
  record.provenance = std::move(provenance.value());
  record.request_digest = reader.digest();
  record.receipt_digest = reader.digest();
  Result<void> end = require_end(reader, "attestation");
  if (!end.has_value()) {
    return end.error();
  }
  if (!record.revision.valid()) {
    return Error::make(ErrorCode::MalformedRequest, "attestation carries a zero revision");
  }
  return RecordPayload(record);
}

// ---------------------------------------------------------------------------
// Receipt reconstruction
// ---------------------------------------------------------------------------

Receipt reconstruct_receipt(const RecordPayload& payload, CommitSequence sequence, LogicalTime logical_time) {
  Receipt receipt;
  std::visit(
      [&](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, ObjectRegisteredRecord>) {
          CreateReceipt typed_receipt;
          typed_receipt.plan = typed.provenance.plan;
          typed_receipt.attempt = typed.provenance.attempt;
          typed_receipt.key = typed.object.key;
          typed_receipt.state = typed.object.state;
          typed_receipt.lifecycle_generation = typed.object.lifecycle_generation;
          typed_receipt.revision = typed.object.revision;
          typed_receipt.commit_sequence = sequence;
          typed_receipt.logical_time = logical_time;
          typed_receipt.receipt_digest = typed.receipt_digest;
          receipt = typed_receipt;
        } else if constexpr (std::is_same_v<T, TransitionAppliedRecord>) {
          TransitionReceipt typed_receipt;
          typed_receipt.plan = typed.provenance.plan;
          typed_receipt.attempt = typed.provenance.attempt;
          typed_receipt.key = typed.key;
          typed_receipt.from = typed.from;
          typed_receipt.to = typed.to;
          typed_receipt.reason = typed.reason;
          typed_receipt.revision_before = typed.revision_before;
          typed_receipt.revision_after = typed.revision_after;
          typed_receipt.lifecycle_generation = typed.lifecycle_generation_after;
          typed_receipt.gate_after = typed.gate_after;
          typed_receipt.commit_sequence = sequence;
          typed_receipt.logical_time = logical_time;
          typed_receipt.request_digest = typed.request_digest;
          typed_receipt.entry_digest = typed.entry_chain_digest;
          typed_receipt.receipt_digest = typed.receipt_digest;
          receipt = typed_receipt;
        } else if constexpr (std::is_same_v<T, SuccessorLinkedRecord>) {
          ReplacementReceipt typed_receipt;
          typed_receipt.plan = typed.provenance.plan;
          typed_receipt.attempt = typed.provenance.attempt;
          typed_receipt.predecessor = typed.link.predecessor;
          typed_receipt.successor = typed.link.successor;
          typed_receipt.replacement_generation = typed.link.replacement_generation;
          typed_receipt.commit_sequence = sequence;
          typed_receipt.logical_time = logical_time;
          typed_receipt.link_digest = typed.link.link_digest;
          typed_receipt.request_digest = typed.request_digest;
          typed_receipt.receipt_digest = typed.receipt_digest;
          receipt = typed_receipt;
        } else if constexpr (std::is_same_v<T, EligibilityGateChangedRecord>) {
          EligibilityGateReceipt typed_receipt;
          typed_receipt.plan = typed.provenance.plan;
          typed_receipt.attempt = typed.provenance.attempt;
          typed_receipt.key = typed.key;
          typed_receipt.before = typed.before;
          typed_receipt.after = typed.after;
          typed_receipt.revision = typed.revision_after;
          typed_receipt.commit_sequence = sequence;
          typed_receipt.logical_time = logical_time;
          typed_receipt.request_digest = typed.request_digest;
          typed_receipt.receipt_digest = typed.receipt_digest;
          receipt = typed_receipt;
        } else if constexpr (std::is_same_v<T, HealthObservedRecord>) {
          HealthReceipt typed_receipt;
          typed_receipt.plan = typed.provenance.plan;
          typed_receipt.attempt = typed.provenance.attempt;
          typed_receipt.key = typed.observation.key;
          typed_receipt.sequence = typed.sequence;
          typed_receipt.commit_sequence = sequence;
          typed_receipt.logical_time = logical_time;
          typed_receipt.request_digest = typed.request_digest;
          typed_receipt.receipt_digest = typed.receipt_digest;
          receipt = typed_receipt;
        } else {
          AttestationReceipt typed_receipt;
          typed_receipt.plan = typed.provenance.plan;
          typed_receipt.attempt = typed.provenance.attempt;
          typed_receipt.key = typed.key;
          typed_receipt.revision = typed.revision;
          typed_receipt.commit_sequence = sequence;
          typed_receipt.logical_time = logical_time;
          typed_receipt.request_digest = typed.request_digest;
          typed_receipt.receipt_digest = typed.receipt_digest;
          receipt = typed_receipt;
        }
      },
      payload);
  return receipt;
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

Result<void> apply_payload(Registry& registry, const RecordPayload& payload, CommitSequence sequence,
                           LogicalTime logical_time) {
  const Receipt receipt = reconstruct_receipt(payload, sequence, logical_time);
  if (compute_receipt_digest(receipt) != std::visit(
          [](const auto& typed) { return typed.receipt_digest; }, payload)) {
    return Error::make(ErrorCode::IntegrityFailure,
                       "the rebuilt receipt does not match the receipt digest the record stored");
  }

  ObservationSequence observation = registry.observation_sequence();

  if (const auto* registered = std::get_if<ObjectRegisteredRecord>(&payload)) {
    HardwareObject object = registered->object;
    object.authority = AuthorityState::Recovered;
    if (registry.contains(object.key)) {
      return Error::make(ErrorCode::ObjectExists, "record registers an object that already exists")
          .with("asset", object.key.asset.value());
    }
    const Result<void> inserted = registry.insert_object(object);
    if (!inserted.has_value()) {
      return inserted.error();
    }
    HistoryEntry entry;
    entry.commit_sequence = sequence;
    entry.logical_time = logical_time;
    entry.key = object.key;
    entry.lifecycle_generation = object.lifecycle_generation;
    entry.revision_after = object.revision;
    entry.from = object.state;
    entry.to = object.state;
    entry.reason = TransitionReason::ObjectRegistered;
    entry.provenance = registered->provenance;
    entry.provenance.logical_time = logical_time;
    entry.receipt_digest = registered->receipt_digest;
    entry.chain_digest = registered->entry_chain_digest;
    const Result<void> appended = registry.append_history(std::move(entry));
    if (!appended.has_value()) {
      return appended.error();
    }
  } else if (const auto* applied = std::get_if<TransitionAppliedRecord>(&payload)) {
    HardwareObject* object = registry.find(applied->key);
    if (object == nullptr) {
      return Error::make(ErrorCode::UnknownAsset, "record refers to an object that does not exist")
          .with("asset", applied->key.asset.value());
    }
    if (object->state != applied->from || object->revision != applied->revision_before ||
        object->lifecycle_generation != applied->lifecycle_generation_before ||
        object->eligibility_gate != applied->gate_before) {
      return Error::make(ErrorCode::IntegrityFailure,
                         "record does not continue from the state the log has replayed to")
          .with("asset", applied->key.asset.value());
    }
    HistoryEntry entry;
    entry.commit_sequence = sequence;
    entry.logical_time = logical_time;
    entry.key = applied->key;
    entry.lifecycle_generation = applied->lifecycle_generation_after;
    entry.revision_before = applied->revision_before;
    entry.revision_after = applied->revision_after;
    entry.from = applied->from;
    entry.to = applied->to;
    entry.reason = applied->reason;
    entry.provenance = applied->provenance;
    entry.provenance.logical_time = logical_time;
    entry.receipt_digest = applied->receipt_digest;
    entry.chain_digest = applied->entry_chain_digest;
    const Result<void> appended = registry.append_history(std::move(entry));
    if (!appended.has_value()) {
      return appended.error();
    }
    object->state = applied->to;
    object->revision = applied->revision_after;
    object->lifecycle_generation = applied->lifecycle_generation_after;
    object->eligibility_gate = applied->gate_after;
    if (applied->location_after.has_value()) {
      object->location = applied->location_after;
    }
    if (applied->successor.has_value()) {
      object->successor = applied->successor;
    }
    object->updated_at = logical_time;
  } else if (const auto* linked = std::get_if<SuccessorLinkedRecord>(&payload)) {
    if (compute_link_digest(linked->link) != linked->link.link_digest) {
      return Error::make(ErrorCode::IntegrityFailure, "replacement link digest does not match its content");
    }
    HardwareObject* predecessor = registry.find(linked->link.predecessor);
    HardwareObject* successor = registry.find(linked->link.successor);
    if (predecessor == nullptr || successor == nullptr) {
      return Error::make(ErrorCode::UnknownAsset, "replacement record refers to an object that does not exist");
    }
    const Result<void> added = registry.lineage().add(linked->link);
    if (!added.has_value()) {
      return added.error();
    }
    predecessor->successor = linked->link.successor;
    predecessor->replacement_generation = linked->link.replacement_generation;
    predecessor->updated_at = logical_time;
    successor->predecessor = linked->link.predecessor;
    successor->updated_at = logical_time;
  } else if (const auto* gate = std::get_if<EligibilityGateChangedRecord>(&payload)) {
    HardwareObject* object = registry.find(gate->key);
    if (object == nullptr) {
      return Error::make(ErrorCode::UnknownAsset, "gate record refers to an object that does not exist");
    }
    if (object->eligibility_gate != gate->before || object->revision != gate->revision_before) {
      return Error::make(ErrorCode::IntegrityFailure, "gate record does not continue from the replayed state");
    }
    HistoryEntry entry;
    entry.commit_sequence = sequence;
    entry.logical_time = logical_time;
    entry.key = gate->key;
    entry.lifecycle_generation = object->lifecycle_generation;
    entry.revision_before = gate->revision_before;
    entry.revision_after = gate->revision_after;
    entry.from = object->state;
    entry.to = object->state;
    entry.reason = gate->after == EligibilityGate::Open ? TransitionReason::EligibilityGateOpened
                                                        : TransitionReason::EligibilityGateClosed;
    entry.provenance = gate->provenance;
    entry.provenance.logical_time = logical_time;
    entry.receipt_digest = gate->receipt_digest;
    entry.chain_digest = gate->entry_chain_digest;
    const Result<void> appended = registry.append_history(std::move(entry));
    if (!appended.has_value()) {
      return appended.error();
    }
    object->eligibility_gate = gate->after;
    object->revision = gate->revision_after;
    object->updated_at = logical_time;
  } else if (const auto* observed = std::get_if<HealthObservedRecord>(&payload)) {
    if (observed->sequence.value() != registry.observation_sequence().value() + 1ull) {
      return Error::make(ErrorCode::IntegrityFailure, "health observation sequence is not contiguous");
    }
    HealthSnapshot snapshot;
    snapshot.present = true;
    snapshot.observation = observed->observation;
    snapshot.sequence = observed->sequence;
    snapshot.recorded_at = logical_time;
    snapshot.freshness = Freshness::Unknown;
    const Result<void> stored = registry.set_health(snapshot);
    if (!stored.has_value()) {
      return stored.error();
    }
    observation = observed->sequence;
  } else {
    const auto& attested = std::get<AuthorityAttestedRecord>(payload);
    const HardwareObject* object = registry.find(attested.key);
    if (object == nullptr) {
      return Error::make(ErrorCode::UnknownAsset, "attestation refers to an object that does not exist");
    }
    if (object->revision != attested.revision) {
      return Error::make(ErrorCode::IntegrityFailure, "attestation does not match the replayed revision");
    }
    // Attestation records are durable evidence that authority was established in
    // a session. They deliberately do not make authority durable.
  }

  AppliedPlan plan;
  plan.key.plan = std::visit([](const auto& typed) { return typed.provenance.plan; }, payload);
  plan.key.attempt = std::visit([](const auto& typed) { return typed.provenance.attempt; }, payload);
  plan.kind = record_kind_of(payload);
  plan.request_digest = std::visit([](const auto& typed) { return typed.request_digest; }, payload);
  plan.receipt = receipt;
  plan.commit_sequence = sequence;
  plan.logical_time = logical_time;
  const Result<void> recorded = registry.record_plan(plan);
  if (!recorded.has_value()) {
    return recorded.error();
  }

  return registry.set_watermarks(sequence, logical_time, observation);
}

// ---------------------------------------------------------------------------
// Digest inputs for requests and receipts
// ---------------------------------------------------------------------------

namespace {

void encode_request_common(Writer& writer, std::uint8_t kind, const Provenance& provenance) {
  writer.text(kRequestDigestTag);
  writer.u8(kind);
  writer.text(provenance.actor.id.value());
  writer.u8(u8_of(provenance.actor.kind));
  writer.u32(provenance.authority.bits());
  writer.u64(provenance.policy_generation.value());
  writer.text(provenance.plan.value());
  writer.text(provenance.attempt.value());
  writer.u32(static_cast<std::uint32_t>(provenance.evidence.size()));
  for (const EvidenceRef& evidence : provenance.evidence) {
    encode_evidence(writer, evidence);
  }
  // The authoritative logical time is assigned by the runtime and the wall clock
  // is advisory, so neither takes part in request identity.
}

}  // namespace

std::vector<std::uint8_t> canonical_request_bytes(const CreateRequest& request) {
  Writer writer;
  encode_request_common(writer, 1, request.provenance);
  encode_object_key(writer, request.key);
  writer.u8(u8_of(request.kind));
  writer.text(request.model.value());
  writer.u64(request.firmware_generation.value());
  writer.u8(u8_of(request.initial_state));
  writer.u8(request.location.has_value() ? 1u : 0u);
  if (request.location.has_value()) {
    encode_location(writer, request.location.value());
  }
  return writer.take();
}

std::vector<std::uint8_t> canonical_request_bytes(const TransitionRequest& request) {
  Writer writer;
  encode_request_common(writer, 2, request.provenance);
  encode_object_key(writer, request.key);
  writer.u64(request.expected_lifecycle_generation.value());
  writer.u64(request.expected_revision.value());
  writer.u64(request.expected_control_epoch.value());
  writer.u8(u8_of(request.expected_state));
  writer.u8(u8_of(request.target_state));
  writer.u8(u8_of(request.reason));
  writer.u8(request.successor.has_value() ? 1u : 0u);
  if (request.successor.has_value()) {
    encode_object_key(writer, request.successor.value());
  }
  writer.u8(request.location.has_value() ? 1u : 0u);
  if (request.location.has_value()) {
    encode_location(writer, request.location.value());
  }
  return writer.take();
}

std::vector<std::uint8_t> canonical_request_bytes(const ReplacementRequest& request) {
  Writer writer;
  encode_request_common(writer, 3, request.provenance);
  encode_object_key(writer, request.predecessor);
  encode_object_key(writer, request.successor);
  writer.u64(request.expected_lifecycle_generation.value());
  writer.u64(request.expected_revision.value());
  writer.u64(request.expected_control_epoch.value());
  return writer.take();
}

std::vector<std::uint8_t> canonical_request_bytes(const EligibilityGateRequest& request) {
  Writer writer;
  encode_request_common(writer, 4, request.provenance);
  encode_object_key(writer, request.key);
  writer.u8(u8_of(request.gate));
  writer.u64(request.expected_lifecycle_generation.value());
  writer.u64(request.expected_revision.value());
  writer.u64(request.expected_control_epoch.value());
  return writer.take();
}

std::vector<std::uint8_t> canonical_request_bytes(const HealthObservationRequest& request) {
  Writer writer;
  encode_request_common(writer, 5, request.provenance);
  encode_object_key(writer, request.key);
  writer.u8(u8_of(request.observation.health));
  writer.u8(u8_of(request.observation.readiness));
  writer.u8(u8_of(request.observation.availability));
  writer.digest(request.observation.evidence_digest);
  writer.text(request.observation.source);
  writer.optional_text(request.observation.observed_wall_clock.has_value(),
                       request.observation.observed_wall_clock.value());
  return writer.take();
}

std::vector<std::uint8_t> canonical_request_bytes(const AttestationRequest& request) {
  Writer writer;
  encode_request_common(writer, 6, request.provenance);
  encode_object_key(writer, request.key);
  writer.u64(request.expected_revision.value());
  writer.u64(request.expected_control_epoch.value());
  return writer.take();
}

namespace {

std::vector<std::uint8_t> receipt_bytes(const Receipt& receipt) {
  Writer writer;
  writer.text(kReceiptDigestTag);
  encode_receipt(writer, receipt);
  return writer.take();
}

}  // namespace

std::vector<std::uint8_t> canonical_receipt_bytes(const CreateReceipt& receipt) {
  return receipt_bytes(Receipt(receipt));
}
std::vector<std::uint8_t> canonical_receipt_bytes(const TransitionReceipt& receipt) {
  return receipt_bytes(Receipt(receipt));
}
std::vector<std::uint8_t> canonical_receipt_bytes(const ReplacementReceipt& receipt) {
  return receipt_bytes(Receipt(receipt));
}
std::vector<std::uint8_t> canonical_receipt_bytes(const EligibilityGateReceipt& receipt) {
  return receipt_bytes(Receipt(receipt));
}
std::vector<std::uint8_t> canonical_receipt_bytes(const HealthReceipt& receipt) {
  return receipt_bytes(Receipt(receipt));
}
std::vector<std::uint8_t> canonical_receipt_bytes(const AttestationReceipt& receipt) {
  return receipt_bytes(Receipt(receipt));
}
std::vector<std::uint8_t> canonical_receipt_bytes(const Receipt& receipt) { return receipt_bytes(receipt); }

}  // namespace hardware_lifecycle::detail