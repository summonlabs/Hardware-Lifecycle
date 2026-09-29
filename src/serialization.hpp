#pragma once

// Hardware Lifecycle - internal canonical serialization.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Three things live here:
//   1. the payload layouts of the six durable record kinds, and their replay,
//   2. the field-by-field encoders that the state image digest is built from,
//      shared by the registry and by the export document so that the two can
//      never disagree,
//   3. the canonical byte layouts the request and receipt digests are computed
//      over.
//
// Every digest in this project is domain separated by a fixed ASCII tag, so a
// receipt digest can never collide with a request digest or a state digest by
// construction.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/health.hpp"
#include "hardware_lifecycle/history.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/model.hpp"
#include "hardware_lifecycle/persistence.hpp"
#include "hardware_lifecycle/provenance.hpp"
#include "hardware_lifecycle/registry.hpp"
#include "hardware_lifecycle/replacement.hpp"
#include "hardware_lifecycle/requests.hpp"
#include "hardware_lifecycle/result.hpp"
#include "codec.hpp"

namespace hardware_lifecycle::detail {

// ---------------------------------------------------------------------------
// Durable record payloads
// ---------------------------------------------------------------------------

struct ObjectRegisteredRecord {
  HardwareObject object;
  Provenance provenance;
  Digest request_digest;
  Digest receipt_digest;
  Digest entry_chain_digest;
};

struct TransitionAppliedRecord {
  ObjectKey key;
  LifecycleState from = LifecycleState::Ordered;
  LifecycleState to = LifecycleState::Ordered;
  TransitionReason reason = TransitionReason::Unset;
  Revision revision_before;
  Revision revision_after;
  LifecycleGeneration lifecycle_generation_before;
  LifecycleGeneration lifecycle_generation_after;
  EligibilityGate gate_before = EligibilityGate::Unknown;
  EligibilityGate gate_after = EligibilityGate::Unknown;
  std::optional<Location> location_after;
  std::optional<ObjectKey> successor;
  Provenance provenance;
  Digest request_digest;
  Digest receipt_digest;
  Digest entry_chain_digest;
};

struct SuccessorLinkedRecord {
  ReplacementRecord link;
  Provenance provenance;
  Digest request_digest;
  Digest receipt_digest;
};

struct EligibilityGateChangedRecord {
  ObjectKey key;
  EligibilityGate before = EligibilityGate::Unknown;
  EligibilityGate after = EligibilityGate::Unknown;
  Revision revision_before;
  Revision revision_after;
  Provenance provenance;
  Digest request_digest;
  Digest receipt_digest;
  /// The chain digest of the history entry this gate decision appends. A gate
  /// decision is a durable mutation of the object like any other, so it is
  /// recorded in the object's history chain rather than only in the log.
  Digest entry_chain_digest;
};

struct HealthObservedRecord {
  HealthObservation observation;
  ObservationSequence sequence;
  Provenance provenance;
  Digest request_digest;
  Digest receipt_digest;
};

struct AuthorityAttestedRecord {
  ObjectKey key;
  Revision revision;
  Provenance provenance;
  Digest request_digest;
  Digest receipt_digest;
};

using RecordPayload =
    std::variant<ObjectRegisteredRecord, TransitionAppliedRecord, SuccessorLinkedRecord, EligibilityGateChangedRecord,
                 HealthObservedRecord, AuthorityAttestedRecord>;

[[nodiscard]] RecordKind record_kind_of(const RecordPayload& payload) noexcept;

[[nodiscard]] Result<std::vector<std::uint8_t>> encode_payload(const RecordPayload& payload);

/// Decodes a payload for the given record kind. Trailing bytes after the
/// expected fields are an error; an unknown kind is an error.
[[nodiscard]] Result<RecordPayload> decode_payload(RecordKind kind, std::span<const std::uint8_t> bytes);

/// Replays one record onto a registry. Every cross check the record carries is
/// verified here (chain digests, expected before values, generation continuity);
/// a mismatch is an error, never a repair.
[[nodiscard]] Result<void> apply_payload(Registry& registry, const RecordPayload& payload, CommitSequence sequence,
                                         LogicalTime logical_time);

/// Rebuilds the receipt a record produced, from the record itself plus the
/// sequence and logical time the frame carried. Replay then verifies that the
/// rebuilt receipt hashes to the receipt digest the record stored, so a record
/// can never describe a receipt that was not issued.
[[nodiscard]] Receipt reconstruct_receipt(const RecordPayload& payload, CommitSequence sequence,
                                          LogicalTime logical_time);

// ---------------------------------------------------------------------------
// Canonical field encoders
// ---------------------------------------------------------------------------

void encode_evidence(Writer& writer, const EvidenceRef& evidence);
void encode_provenance(Writer& writer, const Provenance& provenance);
void encode_location(Writer& writer, const Location& location);
void encode_object_key(Writer& writer, const ObjectKey& key);
void encode_object(Writer& writer, const HardwareObject& object);
void encode_history_entry(Writer& writer, const HistoryEntry& entry);
void encode_lineage(Writer& writer, const ReplacementRecord& record);
void encode_receipt(Writer& writer, const Receipt& receipt);
void encode_plan(Writer& writer, const AppliedPlan& plan);
void encode_health(Writer& writer, const HealthSnapshot& snapshot);

// ---------------------------------------------------------------------------
// Canonical documents
// ---------------------------------------------------------------------------

/// Digest input for one complete authoritative state. Objects, history, lineage
/// records, plans and health snapshots are written in canonical order; callers
/// must supply them already ordered (the registry always does, and the export
/// path sorts).
[[nodiscard]] std::vector<std::uint8_t> canonical_state_bytes(CommitSequence commit_sequence, LogicalTime logical_time,
                                                              ObservationSequence observation_sequence,
                                                              const std::vector<HardwareObject>& objects,
                                                              const std::vector<HistoryEntry>& history,
                                                              const std::vector<ReplacementRecord>& lineage,
                                                              const std::vector<AppliedPlan>& plans,
                                                              const std::vector<HealthSnapshot>& health);

[[nodiscard]] std::vector<std::uint8_t> canonical_request_bytes(const CreateRequest& request);
[[nodiscard]] std::vector<std::uint8_t> canonical_request_bytes(const TransitionRequest& request);
[[nodiscard]] std::vector<std::uint8_t> canonical_request_bytes(const ReplacementRequest& request);
[[nodiscard]] std::vector<std::uint8_t> canonical_request_bytes(const EligibilityGateRequest& request);
[[nodiscard]] std::vector<std::uint8_t> canonical_request_bytes(const HealthObservationRequest& request);
[[nodiscard]] std::vector<std::uint8_t> canonical_request_bytes(const AttestationRequest& request);

[[nodiscard]] std::vector<std::uint8_t> canonical_receipt_bytes(const CreateReceipt& receipt);
[[nodiscard]] std::vector<std::uint8_t> canonical_receipt_bytes(const TransitionReceipt& receipt);
[[nodiscard]] std::vector<std::uint8_t> canonical_receipt_bytes(const ReplacementReceipt& receipt);
[[nodiscard]] std::vector<std::uint8_t> canonical_receipt_bytes(const EligibilityGateReceipt& receipt);
[[nodiscard]] std::vector<std::uint8_t> canonical_receipt_bytes(const HealthReceipt& receipt);
[[nodiscard]] std::vector<std::uint8_t> canonical_receipt_bytes(const AttestationReceipt& receipt);
[[nodiscard]] std::vector<std::uint8_t> canonical_receipt_bytes(const Receipt& receipt);

/// Domain tags. Exposed so that tests and the documentation can name them.
inline constexpr std::string_view kStateImageTag = "HLSTATE1";
inline constexpr std::string_view kRequestDigestTag = "HLREQST1";
inline constexpr std::string_view kReceiptDigestTag = "HLRCPT01";
inline constexpr std::string_view kRecordPayloadTag = "HLRECRD1";

}  // namespace hardware_lifecycle::detail