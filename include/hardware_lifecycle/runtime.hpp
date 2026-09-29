#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The runtime is the only supported way to change lifecycle state. It composes
// the validator, the registry, the durable store and the applied plan ledger so
// that each externally meaningful mutation follows exactly one path:
//
//   validate against the exact identity, generations, revision, epoch and
//   evidence the caller planned against
//     -> copy the authoritative state
//     -> apply the mutation to the copy
//     -> commit the record and the resulting state digest durably
//     -> publish the copy as the new authoritative state
//     -> only then issue the receipt
//
// A failure anywhere before publication leaves the previous generation
// authoritative and complete: the receipt is not issued and the in-memory state
// is restored from the copy.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/diff.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/export.hpp"
#include "hardware_lifecycle/health.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/model.hpp"
#include "hardware_lifecycle/persistence.hpp"
#include "hardware_lifecycle/registry.hpp"
#include "hardware_lifecycle/requests.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

/// Whether receipts issued by this runtime describe durable facts.
enum class DurabilityClass : std::uint8_t {
  /// Every receipt follows a published, fsynced manifest.
  Durable = 0,
  /// Nothing survives the process. Used by examples, benchmarks and the
  /// in-memory proofs; never used for a durable claim.
  Ephemeral,
};

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(DurabilityClass durability) noexcept;

/// How many authoritative logical time steps a health observation stays Fresh.
/// Freshness is derived at read time and is never a durable fact.
inline constexpr std::uint64_t kDefaultHealthFreshnessWindow = 256;

struct OpenOptions {
  std::filesystem::path root;
  /// Open without the writer lock and without the ability to commit.
  bool read_only = false;
  /// Create and initialise the store when the directory is empty.
  bool create_if_missing = true;
  /// Optional crash consistency seam. Must outlive the runtime.
  const CommitObserver* observer = nullptr;
  RegistryLimits registry_limits;
  std::uint64_t health_freshness_window = kDefaultHealthFreshnessWindow;
};

/// A complete description of the runtime, including what recovery proved.
struct RuntimeStatus {
  DurabilityClass durability = DurabilityClass::Durable;
  bool read_only = false;
  bool writable = false;
  CommitSequence commit_sequence;
  LogicalTime logical_time;
  ObservationSequence observation_sequence;
  ControlEpoch control_epoch;
  IncarnationId incarnation;
  std::size_t object_count = 0;
  std::size_t history_entry_count = 0;
  std::size_t lineage_links = 0;
  std::size_t applied_plans = 0;
  RecoveryReport recovery;
};

/// Everything the runtime knows about one object. Note that eligibility is a
/// derived projection and health is an observation; neither is lifecycle state.
struct ObjectView {
  HardwareObject object;
  ServiceEligibility eligibility = ServiceEligibility::Unknown;
  HealthSnapshot health;
};

struct HistoryView {
  ObjectKey key;
  std::vector<HistoryEntry> entries;
  Digest chain_head;
};

struct LineageView {
  ObjectKey key;
  std::vector<ObjectKey> ancestors;
  std::vector<ObjectKey> successors;
  std::vector<ReplacementRecord> links;
};

struct ListQuery {
  std::optional<LifecycleState> state;
  std::optional<HardwareKind> kind;
  std::optional<SiteId> site;
  std::optional<ServiceEligibility> eligibility;
};

class HARDWARE_LIFECYCLE_API Runtime {
 public:
  Runtime(Runtime&& other) noexcept;
  Runtime& operator=(Runtime&& other) noexcept;
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  /// Opens the store, verifies and replays it, checks the replayed state digest
  /// against the published one and marks every recovered object as not having
  /// live authority.
  [[nodiscard]] static Result<Runtime> open(const OpenOptions& options);

  /// Opens an existing store without taking writer authority.
  [[nodiscard]] static Result<Runtime> open_read_only(const std::filesystem::path& root);

  /// Non durable runtime for examples, benchmarks and in-memory proofs. Receipts
  /// are marked Ephemeral; nothing is written anywhere.
  [[nodiscard]] static Result<Runtime> open_ephemeral();

  // ---------------------------------------------------------------------
  // Mutations
  // ---------------------------------------------------------------------
  [[nodiscard]] Result<CreateReceipt> create_object(const CreateRequest& request);
  [[nodiscard]] Result<TransitionReceipt> apply_transition(const TransitionRequest& request);
  [[nodiscard]] Result<ReplacementReceipt> link_replacement(const ReplacementRequest& request);
  [[nodiscard]] Result<EligibilityGateReceipt> set_eligibility_gate(const EligibilityGateRequest& request);
  [[nodiscard]] Result<HealthReceipt> observe_health(const HealthObservationRequest& request);
  [[nodiscard]] Result<AttestationReceipt> attest_authority(const AttestationRequest& request);

  /// Replays an exported document through the ordinary commit path. The store
  /// must be empty; a partial or divergent import is refused, never merged.
  [[nodiscard]] Result<ImportReport> import_document(std::string_view document, const ImportOptions& options);

  // ---------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------
  [[nodiscard]] Result<ObjectView> inspect(const ObjectKey& key) const;
  [[nodiscard]] Result<std::vector<ObjectView>> list(const ListQuery& query) const;
  [[nodiscard]] Result<HistoryView> history(const ObjectKey& key) const;
  [[nodiscard]] Result<LineageView> lineage(const ObjectKey& key) const;
  [[nodiscard]] Result<std::vector<ReplacementRecord>> all_links() const;

  /// The exact validator that apply_transition uses, with no mutation.
  [[nodiscard]] Result<PreflightReport> preflight_transition(const TransitionRequest& request) const;
  [[nodiscard]] Result<ValidatedCreate> preflight_create(const CreateRequest& request) const;
  [[nodiscard]] Result<ValidatedLink> preflight_replacement(const ReplacementRequest& request) const;
  [[nodiscard]] Result<ValidatedGate> preflight_gate(const EligibilityGateRequest& request) const;

  [[nodiscard]] Result<RuntimeStatus> status() const;

  /// Full verification: re-reads the published generation, replays it, compares
  /// the state digest, then checks every registry invariant.
  [[nodiscard]] Result<Digest> verify() const;

  [[nodiscard]] Snapshot snapshot(const ExportOptions& options) const;
  [[nodiscard]] Result<std::string> export_document(const ExportOptions& options) const;
  [[nodiscard]] Result<SnapshotDiff> diff_against(const Snapshot& other) const;

  /// What the last open proved about the durable store. All fields are zero for
  /// an ephemeral runtime, which has no published generation at all.
  [[nodiscard]] const RecoveryReport& recovery() const noexcept;

  [[nodiscard]] DurabilityClass durability() const noexcept;
  [[nodiscard]] bool writable() const noexcept;
  [[nodiscard]] const Registry& registry() const noexcept;
  /// Null for an ephemeral runtime.
  [[nodiscard]] const Store* store() const noexcept;
  [[nodiscard]] const std::filesystem::path& root() const noexcept;

 private:
  struct Impl;

  explicit Runtime(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

}  // namespace hardware_lifecycle
