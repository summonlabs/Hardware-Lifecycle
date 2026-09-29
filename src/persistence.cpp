// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The durable store protocol: open, commit, read back, verify and close. The
// byte layouts live in store_files.hpp; this file owns the order of the steps
// and what every failure leaves behind.
//
// Invariants, in one place:
//   * The manifest on disk is the only description of a published generation.
//     Journal bytes past manifest.segment_bytes were never published, so they
//     carry no authority and are discarded on the next write mode open.
//   * A commit appends, flushes, reads the bytes back and verifies them before
//     it publishes. A failure after the append restores the previous prefix, so
//     the published generation is exactly what it was before the call.
//   * Fencing advances only in open, only after the state it fences is already
//     durable, and only by publishing a new manifest: there is no window in
//     which an epoch is durable before the state it fences.
//   * In memory watermarks move only after the matching manifest is durable, so
//     a store never claims a generation the disk does not have.

#include "hardware_lifecycle/persistence.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fs_platform.hpp"
#include "hardware_lifecycle/compatibility.hpp"
#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/result.hpp"
#include "store_files.hpp"

namespace hardware_lifecycle {

// ---------------------------------------------------------------------------
// Enum spellings
// ---------------------------------------------------------------------------

std::string_view to_string(StoreMode mode) noexcept {
  switch (mode) {
    case StoreMode::ReadOnly:
      return "read_only";
    case StoreMode::OpenExisting:
      return "open_existing";
    case StoreMode::OpenOrCreate:
      return "open_or_create";
    case StoreMode::CreateNew:
      return "create_new";
  }
  return "unknown";
}

std::string_view to_string(RecordKind kind) noexcept {
  switch (kind) {
    case RecordKind::ObjectRegistered:
      return "object_registered";
    case RecordKind::TransitionApplied:
      return "transition_applied";
    case RecordKind::SuccessorLinked:
      return "successor_linked";
    case RecordKind::EligibilityGateChanged:
      return "eligibility_gate_changed";
    case RecordKind::HealthObserved:
      return "health_observed";
    case RecordKind::AuthorityAttested:
      return "authority_attested";
  }
  return "unknown";
}

Result<RecordKind> parse_record_kind(std::uint16_t raw) noexcept {
  switch (static_cast<RecordKind>(raw)) {
    case RecordKind::ObjectRegistered:
      return RecordKind::ObjectRegistered;
    case RecordKind::TransitionApplied:
      return RecordKind::TransitionApplied;
    case RecordKind::SuccessorLinked:
      return RecordKind::SuccessorLinked;
    case RecordKind::EligibilityGateChanged:
      return RecordKind::EligibilityGateChanged;
    case RecordKind::HealthObserved:
      return RecordKind::HealthObserved;
    case RecordKind::AuthorityAttested:
      return RecordKind::AuthorityAttested;
  }
  return Error::make(ErrorCode::MalformedRequest, "unknown record kind")
      .with("record kind", std::to_string(raw));
}

std::string_view to_string(CommitStage stage) noexcept {
  switch (stage) {
    case CommitStage::BeforeAppend:
      return "before_append";
    case CommitStage::AfterAppend:
      return "after_append";
    case CommitStage::AfterFlush:
      return "after_flush";
    case CommitStage::AfterVerify:
      return "after_verify";
    case CommitStage::BeforePublish:
      return "before_publish";
    case CommitStage::AfterPublish:
      return "after_publish";
    case CommitStage::AfterFencePublish:
      return "after_fence_publish";
  }
  return "unknown";
}

Result<CommitStage> parse_commit_stage(std::string_view text) {
  // A fixed order, so the scan is deterministic and independent of any
  // container ordering rule.
  constexpr std::array<CommitStage, 7> kStages = {CommitStage::BeforeAppend,   CommitStage::AfterAppend,
                                                  CommitStage::AfterFlush,     CommitStage::AfterVerify,
                                                  CommitStage::BeforePublish,  CommitStage::AfterPublish,
                                                  CommitStage::AfterFencePublish};
  for (const CommitStage stage : kStages) {
    if (text == to_string(stage)) {
      return stage;
    }
  }
  return Error::make(ErrorCode::MalformedRequest, "invalid commit stage spelling")
      .with("commit stage", std::string(text));
}

CommitObserver::~CommitObserver() = default;

Digest state_image_digest(std::string_view canonical_image) noexcept { return Sha256::hash(canonical_image); }

namespace {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

/// Greatest value a durable counter can hold. Advancing past it is reported as
/// CounterOverflow and never wrapped.
constexpr std::uint64_t kCounterMax = (std::numeric_limits<std::uint64_t>::max)();

/// Deterministic UTF-8 rendering of a path for diagnostics. The process locale
/// never decides how a path is spelled.
std::string render_path(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  std::string rendered;
  rendered.reserve(utf8.size());
  for (const char8_t unit : utf8) {
    rendered.push_back(static_cast<char>(unit));
  }
  return rendered;
}

const RecoveryReport& empty_recovery() noexcept {
  static const RecoveryReport report;
  return report;
}

const std::filesystem::path& empty_root() noexcept {
  static const std::filesystem::path path;
  return path;
}

/// Calls the observer only when one is installed. A stage the store never
/// reached is never reported. StoreOptions carries the observer as a pointer to
/// const for ownership and lifetime reasons while the callback itself is a
/// non-const member, so the store calls it through the pointer it was handed and
/// never mutates the observer any other way.
void notify_stage(const CommitObserver* observer, CommitStage stage, CommitSequence sequence) {
  if (observer != nullptr) {
    const_cast<CommitObserver*>(observer)->on_commit_stage(stage, sequence);
  }
}

/// True when the enumerator is one this build defines. A cast of an arbitrary
/// integer to RecordKind is rejected here rather than framed into the journal.
bool is_known_record_kind(RecordKind kind) noexcept {
  switch (kind) {
    case RecordKind::ObjectRegistered:
    case RecordKind::TransitionApplied:
    case RecordKind::SuccessorLinked:
    case RecordKind::EligibilityGateChanged:
    case RecordKind::HealthObserved:
    case RecordKind::AuthorityAttested:
      return true;
  }
  return false;
}

/// Field for field comparison of two manifest images.
bool same_manifest(const detail::ManifestData& left, const detail::ManifestData& right) noexcept {
  return left.format_version == right.format_version && left.header_size == right.header_size &&
         left.flags == right.flags && left.control_epoch == right.control_epoch &&
         left.incarnation == right.incarnation && left.commit_sequence == right.commit_sequence &&
         left.logical_time == right.logical_time && left.record_count == right.record_count &&
         left.segment_bytes == right.segment_bytes && left.log_digest == right.log_digest &&
         left.state_digest == right.state_digest;
}

/// Reads and decodes the manifest image at path. missing_code is the code for a
/// manifest that is not there at all: while a read only store is being opened
/// that is StoreUninitialized, while an open store is re-verified it is
/// StoreCorrupt, because a store that lost its manifest is damaged rather than
/// uninitialised.
Result<detail::ManifestData> read_manifest(const std::filesystem::path& path, ErrorCode missing_code) {
  const Result<bool> exists = detail::path_exists(path);
  if (!exists.has_value()) {
    return exists.error();
  }
  if (!exists.value()) {
    return Error::make(missing_code, "the store manifest is missing").with("manifest", render_path(path));
  }
  const Result<std::uint64_t> size = detail::file_size(path);
  if (!size.has_value()) {
    return size.error();
  }
  if (size.value() != detail::kManifestBytes) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest is not exactly 512 bytes")
        .with("manifest", render_path(path))
        .with("bytes", std::to_string(size.value()));
  }
  const Result<std::vector<std::uint8_t>> bytes = detail::read_file(path, detail::kManifestBytes);
  if (!bytes.has_value()) {
    return bytes.error();
  }
  return detail::decode_manifest(bytes.value());
}

/// Refuses a store file that is a reparse point: the store never operates
/// through a link it cannot fence.
Result<void> check_not_reparse(const std::filesystem::path& path, std::string_view what) {
  const Result<bool> exists = detail::path_exists(path);
  if (!exists.has_value()) {
    return exists.error();
  }
  if (!exists.value()) {
    return ok();
  }
  const Result<bool> reparse = detail::is_reparse_point(path);
  if (!reparse.has_value()) {
    return reparse.error();
  }
  if (reparse.value()) {
    return Error::make(ErrorCode::ReparsePointRejected, std::string(what) + " is a reparse point")
        .with(std::string(what), render_path(path));
  }
  return ok();
}

/// Verifies a published prefix against the manifest that names it: the SHA-256
/// must match and every record must decode in order. The decoded record count is
/// checked as well, so a prefix can never look complete while the manifest
/// publishes more records than it holds.
Result<std::vector<JournalRecord>> verify_prefix(const detail::ManifestData& manifest,
                                                 std::span<const std::uint8_t> bytes) {
  if (bytes.size() != manifest.segment_bytes) {
    return Error::make(ErrorCode::StoreCorrupt, "the published prefix is not the length the manifest publishes")
        .with("published prefix", std::to_string(manifest.segment_bytes))
        .with("read bytes", std::to_string(bytes.size()));
  }
  const Digest digest = Sha256::hash(bytes.data(), bytes.size());
  if (digest != manifest.log_digest) {
    return Error::make(ErrorCode::StoreCorrupt, "the published prefix does not match the manifest log digest")
        .with("manifest digest", manifest.log_digest.hex())
        .with("computed digest", digest.hex());
  }
  const Result<std::vector<JournalRecord>> records = detail::decode_record_prefix(bytes);
  if (!records.has_value()) {
    return records.error();
  }
  if (records.value().size() != manifest.record_count) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest publishes a different record count")
        .with("published records", std::to_string(manifest.record_count))
        .with("decoded records", std::to_string(records.value().size()));
  }
  return records;
}

/// Confirms that the bytes read back are the record that was just appended:
/// frame, magic, version, kind, checksum, sequence, logical time, payload and
/// length.
Result<void> check_readback(std::span<const std::uint8_t> readback, std::span<const std::uint8_t> expected,
                            RecordKind kind, std::uint64_t sequence, std::uint64_t logical_time,
                            std::span<const std::uint8_t> payload) {
  const Result<JournalRecord> decoded = detail::decode_record(readback);
  if (!decoded.has_value()) {
    return decoded.error();
  }
  if (decoded.value().kind != kind) {
    return Error::make(ErrorCode::IntegrityFailure, "the appended record read back with a different kind")
        .with("record kind", std::to_string(static_cast<std::uint16_t>(decoded.value().kind)));
  }
  if (decoded.value().sequence.value() != sequence) {
    return Error::make(ErrorCode::IntegrityFailure, "the appended record read back with a different sequence")
        .with("sequence", std::to_string(decoded.value().sequence.value()))
        .with("expected", std::to_string(sequence));
  }
  if (decoded.value().logical_time.value() != logical_time) {
    return Error::make(ErrorCode::IntegrityFailure, "the appended record read back with a different logical time")
        .with("logical time", std::to_string(decoded.value().logical_time.value()))
        .with("expected", std::to_string(logical_time));
  }
  const std::span<const std::uint8_t> decoded_payload(decoded.value().payload);
  if (decoded_payload.size() != payload.size() ||
      !std::equal(decoded_payload.begin(), decoded_payload.end(), payload.begin())) {
    return Error::make(ErrorCode::IntegrityFailure, "the appended record read back with a different payload")
        .with("payload bytes", std::to_string(decoded_payload.size()))
        .with("expected", std::to_string(payload.size()));
  }
  if (readback.size() != expected.size() || !std::equal(readback.begin(), readback.end(), expected.begin())) {
    return Error::make(ErrorCode::IntegrityFailure, "the appended record did not read back byte for byte")
        .with("read bytes", std::to_string(readback.size()))
        .with("written bytes", std::to_string(expected.size()));
  }
  return ok();
}

}  // namespace

// ---------------------------------------------------------------------------
// Store::Impl
// ---------------------------------------------------------------------------

struct Store::Impl {
  std::filesystem::path root;
  detail::StorePaths paths;
  StoreMode mode = StoreMode::OpenOrCreate;
  bool writable = false;
  bool closed = false;
  const CommitObserver* observer = nullptr;

  /// The manifest image this process last published. It is the in memory truth
  /// about the durable generation and always matches the file on disk.
  detail::ManifestData published;
  RecoveryReport report;

  /// Running SHA-256 over the published prefix, seeded at open and advanced by
  /// every commit, so a commit never rehashes the whole journal.
  Sha256 running;
  detail::AppendFile journal;
  detail::ExclusiveFileLock writer_lock;

  [[nodiscard]] Result<void> open_read_only();
  [[nodiscard]] Result<void> open_write(const StoreOptions& options);
  [[nodiscard]] Result<void> recover();
  [[nodiscard]] Result<std::vector<std::uint8_t>> read_prefix(const detail::ManifestData& manifest) const;
  [[nodiscard]] Result<void> publish(const detail::ManifestData& manifest) const;
  [[nodiscard]] Result<void> fence_advance();
  [[nodiscard]] Result<void> check_writer_generation(const detail::ManifestData& on_disk) const;
  void adopt(const detail::ManifestData& manifest);
};

Result<void> Store::Impl::publish(const detail::ManifestData& manifest) const {
  const std::array<std::uint8_t, detail::kManifestBytes> bytes = detail::encode_manifest(manifest);
  return detail::write_file_atomic(paths.manifest, bytes, true);
}

void Store::Impl::adopt(const detail::ManifestData& manifest) {
  published = manifest;
  report.commit_sequence = CommitSequence::from_value(manifest.commit_sequence);
  report.logical_time = LogicalTime::from_value(manifest.logical_time);
  report.control_epoch = ControlEpoch::from_value(manifest.control_epoch);
  report.incarnation = IncarnationId::from_value(manifest.incarnation);
  report.record_count = manifest.record_count;
  report.segment_bytes = manifest.segment_bytes;
  report.log_digest = manifest.log_digest;
  report.state_digest = manifest.state_digest;
}

Result<std::vector<std::uint8_t>> Store::Impl::read_prefix(const detail::ManifestData& manifest) const {
  const std::uint64_t segment_bytes = manifest.segment_bytes;
  if (segment_bytes > limits::kMaxSegmentBytes) {
    return Error::make(ErrorCode::LimitExceeded, "the published prefix is above the segment maximum")
        .with("published prefix", std::to_string(segment_bytes))
        .with("maximum", std::to_string(limits::kMaxSegmentBytes));
  }
  if (writable) {
    const Result<std::vector<std::uint8_t>> bytes = journal.read_range(0, static_cast<std::size_t>(segment_bytes));
    if (!bytes.has_value()) {
      return bytes.error();
    }
    if (bytes.value().size() != segment_bytes) {
      return Error::make(ErrorCode::StoreCorrupt, "the journal is shorter than the published prefix")
          .with("journal bytes", std::to_string(bytes.value().size()))
          .with("published prefix", std::to_string(segment_bytes));
    }
    return bytes;
  }

  // Read only: no handle is held open, so the file is read whole and the
  // published prefix is taken from its front. A longer file holds unpublished
  // bytes, which this mode reports and never truncates.
  const Result<bool> exists = detail::path_exists(paths.journal);
  if (!exists.has_value()) {
    return exists.error();
  }
  if (!exists.value()) {
    if (segment_bytes == 0) {
      return std::vector<std::uint8_t>();
    }
    return Error::make(ErrorCode::StoreCorrupt, "the journal is missing while the manifest publishes records")
        .with("journal", render_path(paths.journal))
        .with("published prefix", std::to_string(segment_bytes));
  }
  const Result<std::uint64_t> size = detail::file_size(paths.journal);
  if (!size.has_value()) {
    return size.error();
  }
  if (size.value() > limits::kMaxSegmentBytes) {
    return Error::make(ErrorCode::LimitExceeded, "the journal file is above the segment maximum")
        .with("journal", render_path(paths.journal))
        .with("bytes", std::to_string(size.value()))
        .with("maximum", std::to_string(limits::kMaxSegmentBytes));
  }
  if (size.value() < segment_bytes) {
    return Error::make(ErrorCode::StoreCorrupt, "the journal is shorter than the published prefix")
        .with("journal bytes", std::to_string(size.value()))
        .with("published prefix", std::to_string(segment_bytes));
  }
  const Result<std::vector<std::uint8_t>> all = detail::read_file(paths.journal, size.value());
  if (!all.has_value()) {
    return all.error();
  }
  if (all.value().size() < segment_bytes) {
    return Error::make(ErrorCode::StoreCorrupt, "the journal is shorter than the published prefix")
        .with("journal bytes", std::to_string(all.value().size()))
        .with("published prefix", std::to_string(segment_bytes));
  }
  return std::vector<std::uint8_t>(all.value().begin(),
                                   all.value().begin() + static_cast<std::ptrdiff_t>(segment_bytes));
}

Result<void> Store::Impl::check_writer_generation(const detail::ManifestData& on_disk) const {
  if (!writable) {
    return ok();
  }
  // The writer lock excludes every other writer, so a manifest on disk that is
  // not the one this process published is corruption, not competition.
  if (!same_manifest(published, on_disk)) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest on disk is not the generation this writer published")
        .with("manifest", render_path(paths.manifest));
  }
  const Result<std::uint64_t> bytes = journal.size();
  if (!bytes.has_value()) {
    return bytes.error();
  }
  if (bytes.value() != on_disk.segment_bytes) {
    return Error::make(ErrorCode::StoreCorrupt, "the journal holds bytes past the published prefix")
        .with("journal bytes", std::to_string(bytes.value()))
        .with("published prefix", std::to_string(on_disk.segment_bytes));
  }
  return ok();
}

Result<void> Store::Impl::fence_advance() {
  if (published.control_epoch == kCounterMax || published.incarnation == kCounterMax) {
    return Error::make(ErrorCode::CounterOverflow, "the fencing counters cannot be advanced")
        .with("control epoch", std::to_string(published.control_epoch))
        .with("incarnation", std::to_string(published.incarnation));
  }
  detail::ManifestData advanced = published;
  advanced.control_epoch += 1;
  advanced.incarnation += 1;
  const Result<void> written = publish(advanced);
  if (!written.has_value()) {
    return written.error();
  }
  adopt(advanced);
  report.fence_advanced = true;
  return ok();
}

Result<void> Store::Impl::open_read_only() {
  const Result<detail::ManifestData> on_disk = read_manifest(paths.manifest, ErrorCode::StoreUninitialized);
  if (!on_disk.has_value()) {
    const ErrorCode code = on_disk.error().code;
    if (code == ErrorCode::StoreUninitialized || code == ErrorCode::IoFailure ||
        code == ErrorCode::PermissionDenied) {
      return Error::make(ErrorCode::StoreUninitialized, "the store manifest is missing or unreadable")
          .with("manifest", render_path(paths.manifest))
          .with("cause", std::string(to_string(code)));
    }
    return on_disk.error();
  }
  if (on_disk.value().segment_bytes > limits::kMaxSegmentBytes) {
    return Error::make(ErrorCode::LimitExceeded, "the published prefix is above the segment maximum")
        .with("published prefix", std::to_string(on_disk.value().segment_bytes))
        .with("maximum", std::to_string(limits::kMaxSegmentBytes));
  }

  const Result<std::vector<std::uint8_t>> bytes = read_prefix(on_disk.value());
  if (!bytes.has_value()) {
    return bytes.error();
  }
  const Result<std::vector<JournalRecord>> records = verify_prefix(on_disk.value(), bytes.value());
  if (!records.has_value()) {
    return records.error();
  }

  // Bytes past the published prefix were never published. A reader reports
  // them and truncates nothing.
  std::uint64_t tail_bytes = 0;
  const Result<bool> exists = detail::path_exists(paths.journal);
  if (!exists.has_value()) {
    return exists.error();
  }
  if (exists.value()) {
    const Result<std::uint64_t> size = detail::file_size(paths.journal);
    if (!size.has_value()) {
      return size.error();
    }
    if (size.value() > limits::kMaxSegmentBytes) {
      return Error::make(ErrorCode::LimitExceeded, "the journal file is above the segment maximum")
          .with("journal", render_path(paths.journal))
          .with("bytes", std::to_string(size.value()))
          .with("maximum", std::to_string(limits::kMaxSegmentBytes));
    }
    if (size.value() < on_disk.value().segment_bytes) {
      return Error::make(ErrorCode::StoreCorrupt, "the journal is shorter than the published prefix")
          .with("journal bytes", std::to_string(size.value()))
          .with("published prefix", std::to_string(on_disk.value().segment_bytes));
    }
    tail_bytes = size.value() - on_disk.value().segment_bytes;
  }

  adopt(on_disk.value());
  report.discarded_tail_bytes = tail_bytes;
  report.fence_advanced = false;
  report.existing_state = true;
  return ok();
}

Result<void> Store::Impl::recover() {
  const Result<detail::ManifestData> on_disk = read_manifest(paths.manifest, ErrorCode::StoreCorrupt);
  if (!on_disk.has_value()) {
    return on_disk.error();
  }
  if (on_disk.value().segment_bytes > limits::kMaxSegmentBytes) {
    return Error::make(ErrorCode::LimitExceeded, "the published prefix is above the segment maximum")
        .with("published prefix", std::to_string(on_disk.value().segment_bytes))
        .with("maximum", std::to_string(limits::kMaxSegmentBytes));
  }

  // The journal must already cover everything the manifest publishes. A shorter
  // file means published bytes are gone; nothing here repairs that.
  const Result<bool> journal_exists = detail::path_exists(paths.journal);
  if (!journal_exists.has_value()) {
    return journal_exists.error();
  }
  std::uint64_t journal_bytes = 0;
  if (journal_exists.value()) {
    const Result<std::uint64_t> size = detail::file_size(paths.journal);
    if (!size.has_value()) {
      return size.error();
    }
    journal_bytes = size.value();
    if (journal_bytes > limits::kMaxSegmentBytes) {
      return Error::make(ErrorCode::LimitExceeded, "the journal file is above the segment maximum")
          .with("journal", render_path(paths.journal))
          .with("bytes", std::to_string(journal_bytes))
          .with("maximum", std::to_string(limits::kMaxSegmentBytes));
    }
  }
  if (journal_bytes < on_disk.value().segment_bytes) {
    return Error::make(ErrorCode::StoreCorrupt, "the journal does not cover the published prefix")
        .with("journal bytes", std::to_string(journal_bytes))
        .with("published prefix", std::to_string(on_disk.value().segment_bytes));
  }

  Result<detail::AppendFile> opened = detail::AppendFile::open(paths.journal);
  if (!opened.has_value()) {
    return opened.error();
  }
  journal = std::move(opened.value());

  // A stale partial append is dropped before anything else looks at the file,
  // so it can never be mistaken for a published record by a later read.
  const Result<std::uint64_t> handle_bytes = journal.size();
  if (!handle_bytes.has_value()) {
    return handle_bytes.error();
  }
  if (handle_bytes.value() < on_disk.value().segment_bytes) {
    return Error::make(ErrorCode::StoreCorrupt, "the journal does not cover the published prefix")
        .with("journal bytes", std::to_string(handle_bytes.value()))
        .with("published prefix", std::to_string(on_disk.value().segment_bytes));
  }
  const std::uint64_t discarded = handle_bytes.value() - on_disk.value().segment_bytes;
  if (discarded > 0) {
    const Result<void> truncated = journal.truncate_to(on_disk.value().segment_bytes);
    if (!truncated.has_value()) {
      return truncated.error();
    }
  }

  const Result<std::vector<std::uint8_t>> bytes = read_prefix(on_disk.value());
  if (!bytes.has_value()) {
    return bytes.error();
  }
  const Result<std::vector<JournalRecord>> records = verify_prefix(on_disk.value(), bytes.value());
  if (!records.has_value()) {
    return records.error();
  }

  running = Sha256();
  running.update(bytes.value().data(), bytes.value().size());
  adopt(on_disk.value());
  report.discarded_tail_bytes = discarded;
  report.fence_advanced = false;
  report.existing_state = true;
  return fence_advance();
}

Result<void> Store::Impl::open_write(const StoreOptions& options) {
  // CreateNew refuses anything that is not an empty directory. The check runs
  // before the lock is acquired, because acquiring it creates a file and that
  // file is directory content like any other.
  if (mode == StoreMode::CreateNew) {
    const Result<std::vector<std::string>> entries = detail::list_directory(root);
    if (!entries.has_value()) {
      return entries.error();
    }
    if (!entries.value().empty()) {
      return Error::make(ErrorCode::ObjectExists, "the store directory is not empty")
          .with("root", render_path(root))
          .with("entries", std::to_string(entries.value().size()))
          .with("first entry", entries.value().front());
    }
  }

  Result<detail::ExclusiveFileLock> acquired = detail::ExclusiveFileLock::acquire(paths.lock);
  if (!acquired.has_value()) {
    return acquired.error();
  }
  writer_lock = std::move(acquired.value());

  const Result<bool> manifest_exists = detail::path_exists(paths.manifest);
  if (!manifest_exists.has_value()) {
    return manifest_exists.error();
  }
  if (manifest_exists.value()) {
    return recover();
  }

  // No manifest. A journal that holds bytes is state no manifest ever
  // published: that is damage, never an empty store.
  std::uint64_t journal_bytes = 0;
  const Result<bool> journal_exists = detail::path_exists(paths.journal);
  if (!journal_exists.has_value()) {
    return journal_exists.error();
  }
  if (journal_exists.value()) {
    const Result<std::uint64_t> size = detail::file_size(paths.journal);
    if (!size.has_value()) {
      return size.error();
    }
    journal_bytes = size.value();
    if (journal_bytes > limits::kMaxSegmentBytes) {
      return Error::make(ErrorCode::LimitExceeded, "the journal file is above the segment maximum")
          .with("journal", render_path(paths.journal))
          .with("bytes", std::to_string(journal_bytes))
          .with("maximum", std::to_string(limits::kMaxSegmentBytes));
    }
  }
  if (journal_bytes > 0) {
    return Error::make(ErrorCode::StoreCorrupt, "the journal holds bytes that no manifest publishes")
        .with("journal", render_path(paths.journal))
        .with("journal bytes", std::to_string(journal_bytes));
  }
  if (mode == StoreMode::OpenExisting) {
    return Error::make(ErrorCode::StoreUninitialized, "the store has not been initialised")
        .with("root", render_path(root));
  }
  if (options.initial_state_digest.is_zero()) {
    return Error::make(ErrorCode::MalformedRequest, "a new store must name the digest of its initial state")
        .with("root", render_path(root));
  }

  Result<detail::AppendFile> opened = detail::AppendFile::open(paths.journal);
  if (!opened.has_value()) {
    return opened.error();
  }
  journal = std::move(opened.value());

  detail::ManifestData fresh;
  fresh.format_version = kManifestFormatVersion;
  fresh.header_size = static_cast<std::uint16_t>(detail::kManifestBytes);
  fresh.flags = 0;
  fresh.control_epoch = 1;
  fresh.incarnation = 1;
  fresh.commit_sequence = 0;
  fresh.logical_time = 0;
  fresh.record_count = 0;
  fresh.segment_bytes = 0;
  fresh.log_digest = Sha256().finish();  // SHA-256 over zero bytes.
  fresh.state_digest = options.initial_state_digest;

  const Result<void> created = publish(fresh);
  if (!created.has_value()) {
    return created.error();
  }
  adopt(fresh);
  report.discarded_tail_bytes = 0;
  report.fence_advanced = false;
  report.existing_state = false;
  running = Sha256();

  // The initial manifest is durable, so this session fences it exactly as a
  // recovered generation is fenced. A write session therefore never runs under
  // an epoch that was published before the state it fences, whether that state
  // was created here or recovered from disk.
  return fence_advance();
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

Store::Store() noexcept = default;

Store::~Store() = default;

Store::Store(Store&& other) noexcept : impl_(std::move(other.impl_)) {}

Store& Store::operator=(Store&& other) noexcept {
  if (this != &other) {
    // Destroying the previous implementation releases its writer lock and closes
    // its journal handle.
    impl_ = std::move(other.impl_);
  }
  return *this;
}

Result<Store> Store::open(const StoreOptions& options) {
  const Result<detail::StorePaths> paths = detail::store_paths(options.root);
  if (!paths.has_value()) {
    return paths.error();
  }

  auto impl = std::make_unique<Store::Impl>();
  impl->root = options.root;
  impl->paths = paths.value();
  impl->mode = options.mode;
  impl->observer = options.observer;
  impl->writable = options.mode != StoreMode::ReadOnly;

  const Result<bool> root_exists = detail::path_exists(impl->root);
  if (!root_exists.has_value()) {
    return root_exists.error();
  }
  if (options.mode == StoreMode::ReadOnly) {
    if (!root_exists.value()) {
      return Error::make(ErrorCode::StoreUninitialized, "the store root does not exist")
          .with("root", render_path(impl->root));
    }
  } else {
    const Result<void> created = detail::ensure_directory(impl->root);
    if (!created.has_value()) {
      return created.error();
    }
  }

  const Result<bool> root_is_directory = detail::is_directory(impl->root);
  if (!root_is_directory.has_value()) {
    return root_is_directory.error();
  }
  if (!root_is_directory.value()) {
    return Error::make(ErrorCode::StoreCorrupt, "the store root is not a directory")
        .with("root", render_path(impl->root));
  }
  const Result<bool> root_reparse = detail::is_reparse_point(impl->root);
  if (!root_reparse.has_value()) {
    return root_reparse.error();
  }
  if (root_reparse.value()) {
    return Error::make(ErrorCode::ReparsePointRejected, "the store root is a reparse point")
        .with("root", render_path(impl->root));
  }
  const Result<void> manifest_link = check_not_reparse(impl->paths.manifest, "manifest");
  if (!manifest_link.has_value()) {
    return manifest_link.error();
  }
  const Result<void> journal_link = check_not_reparse(impl->paths.journal, "journal");
  if (!journal_link.has_value()) {
    return journal_link.error();
  }

  const Result<void> opened =
      options.mode == StoreMode::ReadOnly ? impl->open_read_only() : impl->open_write(options);
  if (!opened.has_value()) {
    return opened.error();
  }

  Store store;
  store.impl_ = std::move(impl);
  return store;
}

const RecoveryReport& Store::recovery() const noexcept {
  return impl_ != nullptr ? impl_->report : empty_recovery();
}

const std::filesystem::path& Store::root() const noexcept {
  return impl_ != nullptr ? impl_->root : empty_root();
}

bool Store::writable() const noexcept { return impl_ != nullptr && impl_->writable && !impl_->closed; }

CommitSequence Store::commit_sequence() const noexcept {
  return impl_ != nullptr ? impl_->report.commit_sequence : CommitSequence();
}

LogicalTime Store::logical_time() const noexcept {
  return impl_ != nullptr ? impl_->report.logical_time : LogicalTime();
}

ControlEpoch Store::control_epoch() const noexcept {
  return impl_ != nullptr ? impl_->report.control_epoch : ControlEpoch();
}

IncarnationId Store::incarnation() const noexcept {
  return impl_ != nullptr ? impl_->report.incarnation : IncarnationId();
}

Result<LogicalTime> Store::next_logical_time() const {
  if (impl_ == nullptr || impl_->closed) {
    return Error::make(ErrorCode::StoreClosed, "the store is closed");
  }
  if (impl_->published.logical_time == kCounterMax) {
    return Error::make(ErrorCode::CounterOverflow, "the logical clock is exhausted");
  }
  return LogicalTime::from_value(impl_->published.logical_time + 1);
}

Result<CommitOutcome> Store::commit(RecordKind kind, std::vector<std::uint8_t> payload,
                                    LogicalTime expected_logical_time, Digest state_digest) {
  if (impl_ == nullptr || impl_->closed) {
    return Error::make(ErrorCode::StoreClosed, "the store is closed");
  }
  if (!impl_->writable) {
    return Error::make(ErrorCode::ReadOnlyAuthority, "a read only store cannot commit");
  }
  if (!is_known_record_kind(kind)) {
    return Error::make(ErrorCode::MalformedRequest, "unknown record kind")
        .with("record kind", std::to_string(static_cast<std::uint16_t>(kind)));
  }
  if (payload.size() > limits::kMaxRecordPayloadBytes) {
    return Error::make(ErrorCode::LimitExceeded, "the record payload is above its maximum")
        .with("payload bytes", std::to_string(payload.size()))
        .with("maximum", std::to_string(limits::kMaxRecordPayloadBytes));
  }
  if (state_digest.is_zero()) {
    return Error::make(ErrorCode::MalformedRequest, "a published record must name the state digest it produces");
  }

  const std::uint64_t clock = impl_->published.logical_time;
  if (clock == kCounterMax) {
    return Error::make(ErrorCode::CounterOverflow, "the logical clock is exhausted");
  }
  const std::uint64_t next_time = clock + 1;
  if (expected_logical_time.value() != next_time) {
    return Error::make(ErrorCode::Internal, "the expected logical time is not the next logical time")
        .with("expected", std::to_string(expected_logical_time.value()))
        .with("next", std::to_string(next_time));
  }
  const std::uint64_t last_sequence = impl_->published.commit_sequence;
  if (last_sequence == kCounterMax) {
    return Error::make(ErrorCode::CounterOverflow, "the commit sequence is exhausted");
  }
  const std::uint64_t next_sequence = last_sequence + 1;

  const Result<std::vector<std::uint8_t>> encoded = detail::encode_record(kind, next_sequence, next_time, payload);
  if (!encoded.has_value()) {
    return encoded.error();
  }
  const std::vector<std::uint8_t>& frame = encoded.value();
  const std::uint64_t prefix = impl_->published.segment_bytes;
  if (frame.size() > limits::kMaxSegmentBytes - prefix) {
    return Error::make(ErrorCode::LimitExceeded, "the journal segment is above its maximum")
        .with("segment bytes", std::to_string(prefix + frame.size()))
        .with("maximum", std::to_string(limits::kMaxSegmentBytes));
  }
  const std::uint64_t expected_bytes = prefix + frame.size();
  const CommitSequence sequence = CommitSequence::from_value(next_sequence);

  // Best effort restore of the published prefix. A truncation that fails here
  // is reported by the next open as a discarded tail, never ignored.
  const auto rollback = [this, prefix]() noexcept { static_cast<void>(impl_->journal.truncate_to(prefix)); };

  notify_stage(impl_->observer, CommitStage::BeforeAppend, sequence);
  {
    const Result<void> appended = impl_->journal.append(frame);
    if (!appended.has_value()) {
      rollback();
      return appended.error();
    }
  }
  notify_stage(impl_->observer, CommitStage::AfterAppend, sequence);
  {
    const Result<void> flushed = impl_->journal.flush_to_device();
    if (!flushed.has_value()) {
      rollback();
      return flushed.error();
    }
  }
  notify_stage(impl_->observer, CommitStage::AfterFlush, sequence);
  {
    const Result<std::vector<std::uint8_t>> readback = impl_->journal.read_range(prefix, frame.size());
    if (!readback.has_value()) {
      rollback();
      return readback.error();
    }
    const Result<void> checked = check_readback(readback.value(), frame, kind, next_sequence, next_time, payload);
    if (!checked.has_value()) {
      rollback();
      return checked.error();
    }
    const Result<std::uint64_t> size_now = impl_->journal.size();
    if (!size_now.has_value()) {
      rollback();
      return size_now.error();
    }
    if (size_now.value() != expected_bytes) {
      rollback();
      return Error::make(ErrorCode::IntegrityFailure, "the journal length is not the appended length")
          .with("journal bytes", std::to_string(size_now.value()))
          .with("expected", std::to_string(expected_bytes));
    }
  }
  notify_stage(impl_->observer, CommitStage::AfterVerify, sequence);

  Sha256 advanced = impl_->running;
  advanced.update(frame.data(), frame.size());
  const Digest log_digest = advanced.finish();
  detail::ManifestData next = impl_->published;
  next.commit_sequence = next_sequence;
  next.logical_time = next_time;
  next.record_count = impl_->published.record_count + 1;
  next.segment_bytes = expected_bytes;
  next.log_digest = log_digest;
  next.state_digest = state_digest;

  notify_stage(impl_->observer, CommitStage::BeforePublish, sequence);
  {
    const Result<void> written = impl_->publish(next);
    if (!written.has_value()) {
      rollback();
      return written.error();
    }
  }
  notify_stage(impl_->observer, CommitStage::AfterPublish, sequence);

  // The manifest is durable, so the in memory generation may now move. Nothing
  // before this point changes a watermark.
  impl_->running = advanced;
  impl_->adopt(next);

  notify_stage(impl_->observer, CommitStage::AfterFencePublish, sequence);

  CommitOutcome outcome;
  outcome.sequence = sequence;
  outcome.logical_time = LogicalTime::from_value(next_time);
  outcome.log_digest = log_digest;
  outcome.record_bytes = frame.size();
  return outcome;
}

Result<std::vector<JournalRecord>> Store::read_records() const {
  if (impl_ == nullptr || impl_->closed) {
    return Error::make(ErrorCode::StoreClosed, "the store is closed");
  }
  const Result<detail::ManifestData> on_disk = read_manifest(impl_->paths.manifest, ErrorCode::StoreCorrupt);
  if (!on_disk.has_value()) {
    return on_disk.error();
  }
  const Result<void> generation = impl_->check_writer_generation(on_disk.value());
  if (!generation.has_value()) {
    return generation.error();
  }
  const Result<std::vector<std::uint8_t>> bytes = impl_->read_prefix(on_disk.value());
  if (!bytes.has_value()) {
    return bytes.error();
  }
  return verify_prefix(on_disk.value(), bytes.value());
}

Result<Digest> Store::verify() const {
  if (impl_ == nullptr || impl_->closed) {
    return Error::make(ErrorCode::StoreClosed, "the store is closed");
  }
  const Result<detail::ManifestData> on_disk = read_manifest(impl_->paths.manifest, ErrorCode::StoreCorrupt);
  if (!on_disk.has_value()) {
    return on_disk.error();
  }
  const Result<void> generation = impl_->check_writer_generation(on_disk.value());
  if (!generation.has_value()) {
    return generation.error();
  }
  const Result<std::vector<std::uint8_t>> bytes = impl_->read_prefix(on_disk.value());
  if (!bytes.has_value()) {
    return bytes.error();
  }
  const Result<std::vector<JournalRecord>> records = verify_prefix(on_disk.value(), bytes.value());
  if (!records.has_value()) {
    return records.error();
  }
  return on_disk.value().log_digest;
}

Result<void> Store::close() {
  if (impl_ == nullptr || impl_->closed) {
    return ok();
  }
  // Closing is idempotent, releases the writer lock and closes the journal
  // handle. The files themselves stay exactly as they are.
  impl_->closed = true;
  impl_->journal = detail::AppendFile();
  impl_->writer_lock = detail::ExclusiveFileLock();
  return ok();
}

}  // namespace hardware_lifecycle
