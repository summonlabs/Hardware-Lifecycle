#pragma once

// Hardware Lifecycle - the documented on disk layout, as the durability tests
// see it.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// These tests deliberately know the byte layout. A durability claim is only
// meaningful if the test can damage a specific field and check that the reader
// refuses it, so the offsets below are part of what is being proved: the first
// case of the corruption suite checks that the real manifest actually has this
// shape.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <hardware_lifecycle/digest.hpp>

namespace hl_store {

using hardware_lifecycle::Digest;
using hardware_lifecycle::Sha256;
using hardware_lifecycle::crc32_ieee;

inline constexpr std::size_t kManifestBytes = 512;
inline constexpr std::size_t kManifestCrcOffset = 128;
inline constexpr std::size_t kManifestLogDigestOffset = 64;
inline constexpr std::size_t kManifestStateDigestOffset = 96;
inline constexpr std::string_view kManifestMagic = "HLMANIF1";
inline constexpr std::string_view kManifestName = "manifest.hlb";
inline constexpr std::string_view kJournalName = "journal.hlj";
inline constexpr std::size_t kRecordHeaderBytes = 36;
inline constexpr std::string_view kRecordMagic = "HLREC001";

inline std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8) |
                                    static_cast<std::uint16_t>(bytes[offset + 1]));
}

inline std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value = (value << 8) | bytes[offset + static_cast<std::size_t>(index)];
  }
  return value;
}

inline std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8) | bytes[offset + static_cast<std::size_t>(index)];
  }
  return value;
}

inline void write_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  bytes[offset + 1] = static_cast<std::uint8_t>(value & 0xFFu);
}

inline void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((value >> (24 - 8 * index)) & 0xFFu);
  }
}

inline void write_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((value >> (56 - 8 * index)) & 0xFFu);
  }
}

inline std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::uint8_t> bytes;
  if (!stream) {
    return bytes;
  }
  char buffer[4096];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
    const std::streamsize got = stream.gcount();
    for (std::streamsize index = 0; index < got; ++index) {
      bytes.push_back(static_cast<std::uint8_t>(buffer[index]));
    }
    if (!stream) {
      break;
    }
  }
  return bytes;
}

inline bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  stream.flush();
  return static_cast<bool>(stream);
}

inline bool append_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::app);
  if (!stream) {
    return false;
  }
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  stream.flush();
  return static_cast<bool>(stream);
}

inline std::string magic_of(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t size) {
  return std::string(reinterpret_cast<const char*>(bytes.data() + offset), size);
}

/// Recomputes the manifest CRC over its first 128 bytes.
inline void fix_manifest_crc(std::vector<std::uint8_t>& manifest) {
  const std::uint32_t crc = crc32_ieee(manifest.data(), kManifestCrcOffset);
  write_u32(manifest, kManifestCrcOffset, crc);
}

/// Recomputes the manifest's log digest over a journal prefix and stores it.
inline void fix_manifest_log_digest(std::vector<std::uint8_t>& manifest, const std::vector<std::uint8_t>& prefix) {
  const Digest digest = Sha256::hash(prefix.data(), prefix.size());
  const auto& raw = digest.bytes();
  for (std::size_t index = 0; index < Digest::kSize; ++index) {
    manifest[kManifestLogDigestOffset + index] = raw[index];
  }
}

inline Digest manifest_digest(const std::vector<std::uint8_t>& manifest, std::size_t offset) {
  std::array<std::uint8_t, Digest::kSize> raw{};
  for (std::size_t index = 0; index < Digest::kSize; ++index) {
    raw[index] = manifest[offset + index];
  }
  return Digest::from_bytes(raw);
}

/// Copies every regular file of one directory into another, creating it.
inline void copy_tree(const std::filesystem::path& source, const std::filesystem::path& target) {
  std::error_code error;
  std::filesystem::create_directories(target, error);
  for (const auto& entry : std::filesystem::directory_iterator(source, error)) {
    if (entry.is_regular_file(error)) {
      write_bytes(target / entry.path().filename(), read_bytes(entry.path()));
    }
  }
}

}  // namespace hl_store
