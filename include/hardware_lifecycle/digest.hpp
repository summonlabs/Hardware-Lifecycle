#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SHA-256 is the authoritative binding primitive: generations, evidence,
// receipts, history chain links and state images are bound by digest. CRC-32 is
// used only as cheap record framing where a local, non-adversarial corruption
// check is enough; it never carries authority.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

/// A 256-bit digest. The all-zero digest means "not computed" and is never
/// accepted where a real digest is required.
class HARDWARE_LIFECYCLE_API Digest {
 public:
  static constexpr std::size_t kSize = 32;

  Digest() noexcept = default;

  [[nodiscard]] static Digest from_bytes(const std::array<std::uint8_t, kSize>& bytes) noexcept;

  /// Strict 64 hex character parse; every other length is rejected.
  [[nodiscard]] static Result<Digest> from_hex(std::string_view text);

  [[nodiscard]] const std::array<std::uint8_t, kSize>& bytes() const noexcept { return bytes_; }

  /// Lowercase hex, 64 characters.
  [[nodiscard]] std::string hex() const;

  [[nodiscard]] bool is_zero() const noexcept;

  friend bool operator==(const Digest& left, const Digest& right) noexcept { return left.bytes_ == right.bytes_; }
  friend bool operator!=(const Digest& left, const Digest& right) noexcept { return !(left == right); }
  friend bool operator<(const Digest& left, const Digest& right) noexcept { return left.bytes_ < right.bytes_; }

 private:
  std::array<std::uint8_t, kSize> bytes_{};
};

/// Incremental SHA-256 (FIPS 180-4).
class HARDWARE_LIFECYCLE_API Sha256 {
 public:
  Sha256() noexcept;

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept;

  /// Finalises a copy of the running state; the hasher stays usable.
  [[nodiscard]] Digest finish() const noexcept;

  [[nodiscard]] static Digest hash(const void* data, std::size_t size) noexcept;
  [[nodiscard]] static Digest hash(std::string_view text) noexcept;

 private:
  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), used for record
/// framing only.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::uint32_t crc32_ieee(const void* data, std::size_t size) noexcept;

[[nodiscard]] HARDWARE_LIFECYCLE_API std::uint32_t crc32_ieee_update(std::uint32_t seed, const void* data,
                                                                     std::size_t size) noexcept;

}  // namespace hardware_lifecycle
