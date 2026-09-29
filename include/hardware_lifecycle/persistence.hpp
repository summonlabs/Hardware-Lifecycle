#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The durable side of the boundary. The store owns exactly one append only
// journal of integrity checked records, one atomically published manifest and
// one operating system level single writer lock. It never interprets a record:
// it orders, frames, verifies and publishes them.
//
// Commit protocol, in order, with the observer called at every stage:
//   encode -> append -> flush to the device -> read back and verify ->
//   publish the manifest by atomic replace -> fencing is already advanced,
//   because the fencing epoch and the incarnation travel inside the manifest
//   that was just published.
//
// There is no window in which a fencing epoch is durable before the state it
// fences. A crash at any stage leaves the previous manifest in place, and the
// unpublished tail of the segment is discarded on the next open and reported.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

/// How the store is opened.
enum class StoreMode : std::uint8_t {
  /// No lock, no writes. Readers see whole published generations only.
  ReadOnly = 0,
  /// The store must already exist and be initialised.
  OpenExisting,
  /// Open an initialised store, otherwise initialise a new one.
  OpenOrCreate,
  /// The store directory must be empty; anything else is refused.
  CreateNew,
};

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(StoreMode mode) noexcept;

/// The kind of durable record. Unknown values in a segment are rejected rather
/// than skipped.
enum class RecordKind : std::uint16_t {
  ObjectRegistered = 1,
  TransitionApplied = 2,
  SuccessorLinked = 3,
  EligibilityGateChanged = 4,
  HealthObserved = 5,
  AuthorityAttested = 6,
};

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(RecordKind kind) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<RecordKind> parse_record_kind(std::uint16_t raw) noexcept;

/// Points inside the commit protocol at which a proof harness can terminate the
/// process. This is a deliberate, documented crash consistency seam: a real
/// process is killed at a real durable step and the store is reopened.
enum class CommitStage : std::uint8_t {
  BeforeAppend = 0,
  AfterAppend,
  AfterFlush,
  AfterVerify,
  BeforePublish,
  AfterPublish,
  AfterFencePublish,
};

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(CommitStage stage) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<CommitStage> parse_commit_stage(std::string_view text);

/// Called synchronously by commit() at each stage. Implementations must not
/// throw, must not call back into the store, and must return promptly.
class HARDWARE_LIFECYCLE_API CommitObserver {
 public:
  CommitObserver() = default;
  CommitObserver(const CommitObserver&) = delete;
  CommitObserver& operator=(const CommitObserver&) = delete;
  virtual ~CommitObserver();

  virtual void on_commit_stage(CommitStage stage, CommitSequence sequence) = 0;
};

struct StoreOptions {
  std::filesystem::path root;
  StoreMode mode = StoreMode::OpenOrCreate;
  const CommitObserver* observer = nullptr;

  /// Digest of the authoritative state of a store this call initialises. It is
  /// used only when the call actually creates the store, and it must not be the
  /// zero digest: a manifest always names the state it publishes. Passing the
  /// digest of the caller's empty state is what makes a freshly created store
  /// verifiable by the same code path as a recovered one.
  Digest initial_state_digest;
};

/// One record as it exists on the wire and in memory. The payload is opaque to
/// the store; the domain codec owns its layout.
struct JournalRecord {
  RecordKind kind = RecordKind::ObjectRegistered;
  CommitSequence sequence;
  LogicalTime logical_time;
  std::vector<std::uint8_t> payload;
};

/// What the last open proved about the store.
struct RecoveryReport {
  CommitSequence commit_sequence;
  LogicalTime logical_time;
  ControlEpoch control_epoch;
  IncarnationId incarnation;
  std::uint64_t record_count = 0;
  std::uint64_t segment_bytes = 0;
  /// Bytes present past the published prefix. They were never published and are
  /// discarded; they are reported, never ignored silently.
  std::uint64_t discarded_tail_bytes = 0;
  Digest log_digest;
  Digest state_digest;
  /// True when this open advanced the fencing epoch.
  bool fence_advanced = false;
  /// True when a previously published generation was recovered.
  bool existing_state = false;
};

struct CommitOutcome {
  CommitSequence sequence;
  LogicalTime logical_time;
  Digest log_digest;
  std::size_t record_bytes = 0;
};

/// The durable journal plus its manifest and writer lock.
class HARDWARE_LIFECYCLE_API Store {
 public:
  Store() noexcept;
  ~Store();

  Store(Store&& other) noexcept;
  Store& operator=(Store&& other) noexcept;

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  [[nodiscard]] static Result<Store> open(const StoreOptions& options);

  [[nodiscard]] const RecoveryReport& recovery() const noexcept;
  [[nodiscard]] const std::filesystem::path& root() const noexcept;
  [[nodiscard]] bool writable() const noexcept;
  [[nodiscard]] CommitSequence commit_sequence() const noexcept;
  [[nodiscard]] LogicalTime logical_time() const noexcept;
  [[nodiscard]] ControlEpoch control_epoch() const noexcept;
  [[nodiscard]] IncarnationId incarnation() const noexcept;

  /// The logical time the next commit will carry. Verified against the value the
  /// caller passes to commit(); a mismatch is an internal invariant failure.
  [[nodiscard]] Result<LogicalTime> next_logical_time() const;

  /// Appends one record and publishes it atomically. state_digest is the digest
  /// of the authoritative state that replaying the log up to and including this
  /// record must produce; it is stored in the manifest and checked on every
  /// subsequent open.
  [[nodiscard]] Result<CommitOutcome> commit(RecordKind kind, std::vector<std::uint8_t> payload,
                                             LogicalTime expected_logical_time, Digest state_digest);

  /// Reads back the published prefix and decodes every record.
  [[nodiscard]] Result<std::vector<JournalRecord>> read_records() const;

  /// Re-verifies the published generation without mutating anything: manifest
  /// integrity, log digest, record framing and record count.
  [[nodiscard]] Result<Digest> verify() const;

  [[nodiscard]] Result<void> close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Digest of the whole durable state image, computed by the domain layer. The
/// store only carries it.
[[nodiscard]] HARDWARE_LIFECYCLE_API Digest state_image_digest(std::string_view canonical_image) noexcept;

}  // namespace hardware_lifecycle
