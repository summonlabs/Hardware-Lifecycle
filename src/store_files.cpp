// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The durable byte layouts: the 512 byte manifest image, the framed journal
// records and the three file names a store is made of. Every decoder here is
// strict on purpose. A wrong size, a wrong magic, a bad checksum, an unknown
// enumerator, a non-zero reserved byte, a sequence that is not the frame index
// plus one and a logical time that does not strictly increase are all rejected,
// and nothing is ever repaired, clamped or truncated.
//
// Fields are read and written through the canonical binary codec in codec.hpp.
// Only the checksum fields are positional, so only those are written by offset.

#include "store_files.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "codec.hpp"
#include "fs_platform.hpp"
#include "hardware_lifecycle/compatibility.hpp"
#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle::detail {
namespace {

/// Writes one big endian u32 at a fixed offset. Both checksum fields sit at a
/// known position rather than at the end of the encoded sequence.
void store_u32_be(std::uint8_t* bytes, std::uint32_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
  bytes[1] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  bytes[2] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  bytes[3] = static_cast<std::uint8_t>(value & 0xFFu);
}

bool all_zero(std::span<const std::uint8_t> bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0u; });
}

}  // namespace

// ---------------------------------------------------------------------------
// Store file locations
// ---------------------------------------------------------------------------

Result<StorePaths> store_paths(const std::filesystem::path& root) {
  const Result<std::filesystem::path> manifest = safe_child_path(root, kStoreManifestFileName);
  if (!manifest.has_value()) {
    return manifest.error();
  }
  const Result<std::filesystem::path> journal = safe_child_path(root, kStoreJournalFileName);
  if (!journal.has_value()) {
    return journal.error();
  }
  const Result<std::filesystem::path> lock = safe_child_path(root, kStoreWriterLockFileName);
  if (!lock.has_value()) {
    return lock.error();
  }
  StorePaths paths;
  paths.root = root;
  paths.manifest = manifest.value();
  paths.journal = journal.value();
  paths.lock = lock.value();
  return paths;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

std::array<std::uint8_t, kManifestBytes> encode_manifest(const ManifestData& manifest) noexcept {
  Writer writer;
  writer.raw(kManifestMagic.data(), kManifestMagic.size());
  writer.u16(manifest.format_version);
  writer.u16(manifest.header_size);
  writer.u32(manifest.flags);
  writer.u64(manifest.control_epoch);
  writer.u64(manifest.incarnation);
  writer.u64(manifest.commit_sequence);
  writer.u64(manifest.logical_time);
  writer.u64(manifest.record_count);
  writer.u64(manifest.segment_bytes);
  writer.digest(manifest.log_digest);
  writer.digest(manifest.state_digest);
  writer.u32(0u);  // The CRC field, filled in by offset below.
  const std::array<std::uint8_t, kManifestReservedBytes> reserved{};
  writer.raw(reserved.data(), reserved.size());

  std::array<std::uint8_t, kManifestBytes> bytes{};
  const std::vector<std::uint8_t>& encoded = writer.data();
  // kManifestFieldBytes is asserted equal to kManifestBytes, so the field list
  // above fills the whole image. The copy is bounded by both sizes so a layout
  // edit can never run past the array, and the reserved bytes stay zero because
  // the image was value initialised.
  const std::size_t copied = (std::min)(encoded.size(), bytes.size());
  for (std::size_t index = 0; index < copied; ++index) {
    bytes[index] = encoded[index];
  }
  store_u32_be(bytes.data() + kManifestCrcOffset, crc32_ieee(bytes.data(), kManifestCrcOffset));
  return bytes;
}

Result<ManifestData> decode_manifest(std::span<const std::uint8_t> bytes) {
  if (bytes.size() != kManifestBytes) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest is not exactly 512 bytes")
        .with("bytes", std::to_string(bytes.size()));
  }
  if (!std::equal(kManifestMagic.begin(), kManifestMagic.end(), bytes.begin())) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest magic is not HLMANIF1");
  }
  const std::uint32_t computed_crc = crc32_ieee(bytes.data(), kManifestCrcOffset);

  Reader reader(bytes, ErrorCode::StoreCorrupt);
  reader.raw(kManifestMagic.size());
  ManifestData manifest;
  manifest.format_version = reader.u16();
  manifest.header_size = reader.u16();
  manifest.flags = reader.u32();
  manifest.control_epoch = reader.u64();
  manifest.incarnation = reader.u64();
  manifest.commit_sequence = reader.u64();
  manifest.logical_time = reader.u64();
  manifest.record_count = reader.u64();
  manifest.segment_bytes = reader.u64();
  manifest.log_digest = reader.digest();
  manifest.state_digest = reader.digest();
  const std::uint32_t stored_crc = reader.u32();
  const std::span<const std::uint8_t> reserved = reader.raw(kManifestReservedBytes);
  if (!reader.ok()) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest is shorter than its declared layout");
  }
  if (stored_crc != computed_crc) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest CRC does not cover its first 128 bytes")
        .with("stored crc", std::to_string(stored_crc))
        .with("computed crc", std::to_string(computed_crc));
  }
  if (!is_supported(kManifestFormatVersion, kManifestFormatVersionMinReadable, manifest.format_version)) {
    return Error::make(ErrorCode::UnsupportedFormatVersion, "the manifest format version is not readable")
        .with("version", std::to_string(manifest.format_version))
        .with("readable range",
              std::to_string(kManifestFormatVersionMinReadable) + ".." + std::to_string(kManifestFormatVersion));
  }
  if (manifest.header_size != kManifestBytes) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest header size is not 512")
        .with("header size", std::to_string(manifest.header_size));
  }
  if (manifest.flags != 0u) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest flags are not zero")
        .with("flags", std::to_string(manifest.flags));
  }
  if (!all_zero(reserved)) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest reserved area is not zero")
        .with("reserved offset", std::to_string(kManifestReservedOffset));
  }
  if (manifest.commit_sequence != manifest.record_count) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest commit sequence is not its record count")
        .with("commit sequence", std::to_string(manifest.commit_sequence))
        .with("record count", std::to_string(manifest.record_count));
  }
  const bool sequence_empty = manifest.commit_sequence == 0u;
  const bool segment_empty = manifest.segment_bytes == 0u;
  if (sequence_empty != segment_empty) {
    return Error::make(ErrorCode::StoreCorrupt,
                       "exactly one of the manifest commit sequence and segment bytes is zero")
        .with("commit sequence", std::to_string(manifest.commit_sequence))
        .with("segment bytes", std::to_string(manifest.segment_bytes));
  }
  if ((manifest.record_count == 0u) != sequence_empty) {
    return Error::make(ErrorCode::StoreCorrupt, "the manifest record count and commit sequence disagree on emptiness")
        .with("record count", std::to_string(manifest.record_count))
        .with("commit sequence", std::to_string(manifest.commit_sequence));
  }
  return manifest;
}

// ---------------------------------------------------------------------------
// Journal record frames
// ---------------------------------------------------------------------------

std::uint32_t record_crc(std::span<const std::uint8_t> header, std::span<const std::uint8_t> payload) noexcept {
  std::array<std::uint8_t, kRecordHeaderBytes> canonical{};
  const std::size_t copied = (std::min)(header.size(), canonical.size());
  for (std::size_t index = 0; index < copied; ++index) {
    canonical[index] = header[index];
  }
  for (std::size_t index = 0; index < kRecordCrcBytes; ++index) {
    canonical[kRecordCrcOffset + index] = 0u;
  }
  const std::uint32_t header_crc = crc32_ieee(canonical.data(), canonical.size());
  return crc32_ieee_update(header_crc, payload.data(), payload.size());
}

Result<std::vector<std::uint8_t>> encode_record(RecordKind kind, std::uint64_t sequence, std::uint64_t logical_time,
                                                std::span<const std::uint8_t> payload) {
  if (payload.size() > limits::kMaxRecordPayloadBytes) {
    return Error::make(ErrorCode::LimitExceeded, "the record payload is above its maximum")
        .with("payload bytes", std::to_string(payload.size()))
        .with("maximum", std::to_string(limits::kMaxRecordPayloadBytes));
  }
  Writer writer;
  writer.raw(kRecordMagic.data(), kRecordMagic.size());
  writer.u16(kJournalFormatVersion);
  writer.u16(static_cast<std::uint16_t>(kind));
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.u64(sequence);
  writer.u64(logical_time);
  writer.u32(0u);  // The CRC field, filled in by offset below.
  writer.raw(payload);

  std::vector<std::uint8_t> frame = writer.take();
  store_u32_be(frame.data() + kRecordCrcOffset, record_crc(frame, payload));
  return frame;
}

Result<RecordHeader> decode_record_header(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < kRecordHeaderBytes) {
    return Error::make(ErrorCode::IntegrityFailure, "a record header is shorter than 36 bytes")
        .with("bytes", std::to_string(bytes.size()));
  }
  if (!std::equal(kRecordMagic.begin(), kRecordMagic.end(), bytes.begin())) {
    return Error::make(ErrorCode::IntegrityFailure, "the record magic is not HLREC001");
  }

  Reader reader(bytes.first(kRecordHeaderBytes), ErrorCode::IntegrityFailure);
  reader.raw(kRecordMagic.size());
  RecordHeader header;
  header.format_version = reader.u16();
  const std::uint16_t raw_kind = reader.u16();
  header.payload_bytes = reader.u32();
  header.sequence = reader.u64();
  header.logical_time = reader.u64();
  header.crc = reader.u32();
  if (!reader.ok()) {
    return Error::make(ErrorCode::IntegrityFailure, "the record header is truncated");
  }
  if (header.format_version != kJournalFormatVersion) {
    return Error::make(ErrorCode::UnsupportedFormatVersion, "the record format version is not writable by this build")
        .with("version", std::to_string(header.format_version));
  }
  const Result<RecordKind> kind = parse_record_kind(raw_kind);
  if (!kind.has_value()) {
    return Error::make(ErrorCode::IntegrityFailure, "the record kind is not one this build defines")
        .with("record kind", std::to_string(raw_kind));
  }
  header.kind = kind.value();
  if (static_cast<std::uint64_t>(header.payload_bytes) > limits::kMaxRecordPayloadBytes) {
    return Error::make(ErrorCode::LimitExceeded, "the record payload length is above its maximum")
        .with("payload bytes", std::to_string(header.payload_bytes))
        .with("maximum", std::to_string(limits::kMaxRecordPayloadBytes));
  }
  return header;
}

Result<JournalRecord> decode_record(std::span<const std::uint8_t> frame) {
  const Result<RecordHeader> header = decode_record_header(frame);
  if (!header.has_value()) {
    return header.error();
  }
  const std::uint64_t declared = static_cast<std::uint64_t>(header.value().payload_bytes);
  if (frame.size() != kRecordHeaderBytes + declared) {
    return Error::make(ErrorCode::IntegrityFailure, "a record frame is not its header plus its declared payload")
        .with("frame bytes", std::to_string(frame.size()))
        .with("declared payload", std::to_string(declared));
  }
  const std::span<const std::uint8_t> body = frame.subspan(kRecordHeaderBytes);
  const std::uint32_t computed_crc = record_crc(frame.first(kRecordHeaderBytes), body);
  if (computed_crc != header.value().crc) {
    return Error::make(ErrorCode::IntegrityFailure, "the record CRC does not cover its frame")
        .with("stored crc", std::to_string(header.value().crc))
        .with("computed crc", std::to_string(computed_crc));
  }
  JournalRecord record;
  record.kind = header.value().kind;
  record.sequence = CommitSequence::from_value(header.value().sequence);
  record.logical_time = LogicalTime::from_value(header.value().logical_time);
  record.payload.assign(body.begin(), body.end());
  return record;
}

Result<std::vector<JournalRecord>> decode_record_prefix(std::span<const std::uint8_t> prefix) {
  std::vector<JournalRecord> records;
  std::size_t offset = 0;
  std::uint64_t expected_sequence = 1;
  std::uint64_t previous_time = 0;
  while (offset < prefix.size()) {
    const std::span<const std::uint8_t> rest = prefix.subspan(offset);
    if (rest.size() < kRecordHeaderBytes) {
      return Error::make(ErrorCode::IntegrityFailure, "the published prefix ends inside a record header")
          .with("offset", std::to_string(offset))
          .with("remaining bytes", std::to_string(rest.size()));
    }
    const Result<RecordHeader> header = decode_record_header(rest);
    if (!header.has_value()) {
      return header.error();
    }
    const std::size_t frame_bytes = kRecordHeaderBytes + static_cast<std::size_t>(header.value().payload_bytes);
    if (frame_bytes > rest.size()) {
      return Error::make(ErrorCode::IntegrityFailure, "a record payload extends past the published prefix")
          .with("offset", std::to_string(offset))
          .with("declared frame bytes", std::to_string(frame_bytes))
          .with("remaining bytes", std::to_string(rest.size()));
    }
    const Result<JournalRecord> decoded = decode_record(rest.first(frame_bytes));
    if (!decoded.has_value()) {
      return decoded.error();
    }
    if (decoded.value().sequence.value() != expected_sequence) {
      return Error::make(ErrorCode::IntegrityFailure, "a record sequence is not its index plus one")
          .with("sequence", std::to_string(decoded.value().sequence.value()))
          .with("expected", std::to_string(expected_sequence));
    }
    if (decoded.value().logical_time.value() <= previous_time) {
      return Error::make(ErrorCode::IntegrityFailure, "a record logical time does not strictly increase")
          .with("logical time", std::to_string(decoded.value().logical_time.value()))
          .with("previous logical time", std::to_string(previous_time));
    }
    previous_time = decoded.value().logical_time.value();
    records.push_back(decoded.value());
    offset += frame_bytes;
    expected_sequence += 1;
  }
  return records;
}

}  // namespace hardware_lifecycle::detail
