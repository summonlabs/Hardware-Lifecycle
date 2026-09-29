#pragma once

// Hardware Lifecycle - internal durable file layouts.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The store owns exactly three files below its root:
//
//   manifest.hlb  exactly 512 bytes, replaced atomically, never edited in place
//   journal.hlj   append only records, one after another, never rewritten
//   writer.lock   zero length, held under an exclusive operating system lock
//
// This header is not installed. It carries the byte layouts, their strict
// decoders and the path helpers used by the persistence layer; it owns no
// protocol and no lifecycle.
//
// Manifest, big endian, exactly 512 bytes:
//
//   offset  size  field
//   0       8     ASCII magic "HLMANIF1"
//   8       2     u16 format version, inside [kManifestFormatVersionMinReadable,
//                 kManifestFormatVersion]
//   10      2     u16 header size, exactly 512
//   12      4     u32 flags, every bit zero
//   16      8     u64 control epoch
//   24      8     u64 incarnation
//   32      8     u64 commit sequence of the last published record, 0 when empty
//   40      8     u64 logical time of the last published record, 0 when empty
//   48      8     u64 record count
//   56      8     u64 segment bytes, the published prefix length of the journal
//   64      32    SHA-256 over the published prefix bytes of the journal
//   96      32    state digest supplied by the caller
//   128     4     CRC-32 over bytes [0,128)
//   132     380   reserved, every byte zero
//
// Journal record frame, big endian. A segment is a sequence of frames with no
// padding between them: the only way to find the next frame is the declared
// payload length of the current one.
//
//   offset  size  field
//   0       8     ASCII magic "HLREC001"
//   8       2     u16 format version, exactly kJournalFormatVersion
//   10      2     u16 record kind, one of the RecordKind enumerators
//   12      4     u32 payload bytes, at most limits::kMaxRecordPayloadBytes
//   16      8     u64 sequence, exactly one more than the frame's index
//   24      8     u64 logical time, strictly increasing along the segment
//   32      4     u32 CRC-32 over the 36 header bytes with the CRC field
//                 zeroed, then the payload bytes
//   36      ...   payload
//
// Every decoder here rejects: no size is clamped, no field is repaired, no
// trailing byte is ignored and no unknown enumerator is skipped.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/compatibility.hpp"
#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/limits.hpp"
#include "hardware_lifecycle/persistence.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle::detail {

// ---------------------------------------------------------------------------
// File names and layout constants
// ---------------------------------------------------------------------------

inline constexpr std::string_view kStoreManifestFileName = "manifest.hlb";
inline constexpr std::string_view kStoreJournalFileName = "journal.hlj";
inline constexpr std::string_view kStoreWriterLockFileName = "writer.lock";

inline constexpr std::size_t kManifestBytes = 512;
inline constexpr std::string_view kManifestMagic = "HLMANIF1";
inline constexpr std::size_t kManifestCrcOffset = 128;
inline constexpr std::size_t kManifestCrcBytes = 4;
inline constexpr std::size_t kManifestReservedOffset = 132;
inline constexpr std::size_t kManifestReservedBytes = 380;

/// Sum of every manifest field, asserted against the fixed image size so a
/// layout edit cannot silently change how many bytes are written.
inline constexpr std::size_t kManifestFieldBytes =
    8u + 2u + 2u + 4u + 8u + 8u + 8u + 8u + 8u + 8u + Digest::kSize + Digest::kSize + kManifestCrcBytes +
    kManifestReservedBytes;

static_assert(kManifestMagic.size() == 8);
static_assert(kManifestFieldBytes == kManifestBytes);
static_assert(kManifestReservedOffset + kManifestReservedBytes == kManifestBytes);
static_assert(kManifestCrcOffset + kManifestCrcBytes == kManifestReservedOffset);

inline constexpr std::size_t kRecordHeaderBytes = 36;
inline constexpr std::string_view kRecordMagic = "HLREC001";
inline constexpr std::size_t kRecordCrcOffset = 32;
inline constexpr std::size_t kRecordCrcBytes = 4;

static_assert(kRecordMagic.size() == 8);
static_assert(kRecordCrcOffset + kRecordCrcBytes == kRecordHeaderBytes);

// ---------------------------------------------------------------------------
// Store file locations
// ---------------------------------------------------------------------------

/// The three files of one store, joined onto a root directory.
struct StorePaths {
  std::filesystem::path root;
  std::filesystem::path manifest;
  std::filesystem::path journal;
  std::filesystem::path lock;
};

/// Joins the three fixed file names onto root. Every name is validated as a
/// path segment, so a store can never be redirected out of its root.
[[nodiscard]] Result<StorePaths> store_paths(const std::filesystem::path& root);

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

/// One decoded manifest image. Counters are raw because this struct mirrors the
/// bytes; the persistence layer owns their meaning.
struct ManifestData {
  std::uint16_t format_version = kManifestFormatVersion;
  std::uint16_t header_size = static_cast<std::uint16_t>(kManifestBytes);
  std::uint32_t flags = 0;
  std::uint64_t control_epoch = 0;
  std::uint64_t incarnation = 0;
  std::uint64_t commit_sequence = 0;
  std::uint64_t logical_time = 0;
  std::uint64_t record_count = 0;
  std::uint64_t segment_bytes = 0;
  Digest log_digest;
  Digest state_digest;
};

/// Encodes exactly one manifest image. The caller owns the cross field
/// invariants; encode writes the fields verbatim and the reserved area as zero.
[[nodiscard]] std::array<std::uint8_t, kManifestBytes> encode_manifest(const ManifestData& manifest) noexcept;

/// Decodes one manifest image of exactly kManifestBytes bytes and rejects, in
/// this order: a wrong size, a wrong magic, a bad CRC, a format version outside
/// the readable range (UnsupportedFormatVersion), a header size other than 512,
/// non-zero flags, any non-zero reserved byte, a commit sequence that differs
/// from the record count, exactly one of {commit sequence, segment bytes} being
/// zero, and a record count whose emptiness differs from the sequence's.
[[nodiscard]] Result<ManifestData> decode_manifest(std::span<const std::uint8_t> bytes);

// ---------------------------------------------------------------------------
// Journal record frames
// ---------------------------------------------------------------------------

/// One decoded frame header. The payload follows the header and is not part of
/// this struct.
struct RecordHeader {
  std::uint16_t format_version = 0;
  RecordKind kind = RecordKind::ObjectRegistered;
  std::uint32_t payload_bytes = 0;
  std::uint64_t sequence = 0;
  std::uint64_t logical_time = 0;
  std::uint32_t crc = 0;
};

/// CRC-32 of one record frame: the first kRecordHeaderBytes bytes of header with
/// the CRC field treated as zero, followed by payload. header must hold at least
/// kRecordHeaderBytes bytes; the CRC field inside it is ignored, not required to
/// be zero.
[[nodiscard]] std::uint32_t record_crc(std::span<const std::uint8_t> header,
                                       std::span<const std::uint8_t> payload) noexcept;

/// Encodes one complete frame. A payload above limits::kMaxRecordPayloadBytes is
/// a LimitExceeded, never a truncated length field.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_record(RecordKind kind, std::uint64_t sequence,
                                                              std::uint64_t logical_time,
                                                              std::span<const std::uint8_t> payload);

/// Decodes the header at the start of bytes, which must hold at least
/// kRecordHeaderBytes bytes. Rejects, in this order: a wrong magic, an
/// unsupported format version, an unknown record kind, a payload length above
/// limits::kMaxRecordPayloadBytes (LimitExceeded) and a truncated header.
[[nodiscard]] Result<RecordHeader> decode_record_header(std::span<const std::uint8_t> bytes);

/// Decodes exactly one frame, header and payload. Rejects a frame that is not
/// exactly header plus declared payload, and a frame whose CRC does not match.
[[nodiscard]] Result<JournalRecord> decode_record(std::span<const std::uint8_t> frame);

/// Decodes a whole published prefix: every frame in order, with a sequence that
/// is exactly the frame index plus one and a logical time that strictly
/// increases. A partial frame or a payload that runs past the end of the prefix
/// is an IntegrityFailure.
[[nodiscard]] Result<std::vector<JournalRecord>> decode_record_prefix(std::span<const std::uint8_t> prefix);

}  // namespace hardware_lifecycle::detail
