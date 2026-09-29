#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Export and import of the complete authoritative state as one canonical JSON
// document. Import is not a bulk file load: the document is decoded strictly,
// then replayed through the ordinary commit path, record by record, in global
// commit order. Importing an export must reproduce the same state digest, and
// the importer refuses the document rather than accepting a divergent result.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/health.hpp"
#include "hardware_lifecycle/history.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/model.hpp"
#include "hardware_lifecycle/persistence.hpp"
#include "hardware_lifecycle/registry.hpp"
#include "hardware_lifecycle/replacement.hpp"
#include "hardware_lifecycle/requests.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

struct ExportOptions {
  /// Include the authoritative durable event log. Without it the document
  /// describes a state but cannot be imported, and says so.
  bool include_events = true;
  bool include_history = true;
  bool include_lineage = true;
  bool include_plans = true;
  bool include_health = true;
  bool pretty = true;
};

/// Self description of one exported document.
struct SnapshotHeader {
  std::uint16_t format_version = 0;
  std::string library_version;
  Digest state_digest;
  CommitSequence commit_sequence;
  LogicalTime logical_time;
  ObservationSequence observation_sequence;
  ControlEpoch control_epoch;
  IncarnationId incarnation;
  std::size_t object_count = 0;
  std::size_t history_entry_count = 0;
  std::size_t lineage_link_count = 0;
  std::size_t applied_plan_count = 0;
  std::size_t event_count = 0;
  /// False when the runtime that produced the document had no durable log, so
  /// the document describes a state but cannot be imported.
  bool importable = false;
};

/// One authoritative durable record, verbatim.
///
/// The event log is what makes an import provable rather than approximate: the
/// importer decodes each record payload strictly, applies it through the
/// ordinary commit path in order and refuses the document unless the state it
/// produces hashes to the digest the document declares. The arrays that follow
/// are derived views for humans, tooling and diffing; they are cross checked
/// against the replayed state so a hand edited document cannot slip through.
struct ExportedRecord {
  RecordKind kind = RecordKind::ObjectRegistered;
  CommitSequence sequence;
  LogicalTime logical_time;
  std::vector<std::uint8_t> payload;
};

struct Snapshot {
  SnapshotHeader header;
  /// Empty for a runtime that has no durable log, which is reported as such
  /// rather than as an empty state.
  std::vector<ExportedRecord> events;
  std::vector<HardwareObject> objects;
  std::vector<HistoryEntry> history;
  std::vector<ReplacementRecord> lineage;
  std::vector<AppliedPlan> plans;
  std::vector<HealthSnapshot> health;
};

/// Captures the registry. Events are emitted in durable commit order; objects
/// and history are emitted in canonical key order as derived views. The event
/// log is supplied by the caller because only the durable store knows it.
[[nodiscard]] HARDWARE_LIFECYCLE_API Snapshot snapshot_of(const Registry& registry,
                                                         const std::vector<ExportedRecord>& events,
                                                         ControlEpoch control_epoch, IncarnationId incarnation,
                                                         const ExportOptions& options);

/// Canonical JSON. Field order, whitespace and number formatting are fixed, so
/// two equal snapshots produce byte identical documents.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<std::string> to_json(const Snapshot& snapshot,
                                                                 const ExportOptions& options);

/// Strict JSON decode. Rejects: unknown or missing fields, duplicate keys,
/// trailing content, invalid enum spellings, zero counters where a real counter
/// is required, out of range numbers, nesting deeper than the limit, documents
/// larger than the limit and digests that are not 64 hex characters.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<Snapshot> from_json(std::string_view document);

/// Digest that import must reproduce. It covers exactly the same semantic
/// content as the registry state image and is independent of formatting.
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest compute_snapshot_state_digest(const Snapshot& snapshot);

struct ImportOptions {
  /// Refuse the document unless the state it describes is reproduced exactly.
  /// There is no mode in which a divergent import is accepted.
  bool verify_state_digest = true;
  bool verify_receipts = true;
};

struct ImportReport {
  std::size_t events = 0;
  std::size_t objects = 0;
  std::size_t history_entries = 0;
  std::size_t lineage_links = 0;
  std::size_t applied_plans = 0;
  std::size_t health_observations = 0;
  Digest declared_state_digest;
  Digest produced_state_digest;
  CommitSequence commit_sequence;
  LogicalTime logical_time;
};

}  // namespace hardware_lifecycle