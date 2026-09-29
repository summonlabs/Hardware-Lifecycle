// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320). This checksum only
// frames records: it detects accidental local corruption and carries no
// authority, because it is trivially forgeable.

#include "hardware_lifecycle/digest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace hardware_lifecycle {
namespace {

/// Reflected lookup table for the 0xEDB88320 polynomial, built at compile time
/// so the checksum is identical on every platform and in every build.
constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::size_t index = 0; index < table.size(); ++index) {
    std::uint32_t value = static_cast<std::uint32_t>(index);
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32Table = make_crc32_table();

/// Raw reflected update: continues from a running register value and returns the
/// running register value. No initial or final inversion happens here.
std::uint32_t crc32_raw(std::uint32_t state, const std::uint8_t* bytes, std::size_t size) noexcept {
  std::uint32_t crc = state;
  for (std::size_t index = 0; index < size; ++index) {
    const std::uint32_t slot = (crc ^ static_cast<std::uint32_t>(bytes[index])) & 0xFFu;
    crc = kCrc32Table[static_cast<std::size_t>(slot)] ^ (crc >> 8);
  }
  return crc;
}

}  // namespace

std::uint32_t crc32_ieee(const void* data, std::size_t size) noexcept { return crc32_ieee_update(0u, data, size); }

std::uint32_t crc32_ieee_update(std::uint32_t seed, const void* data, std::size_t size) noexcept {
  if (data == nullptr || size == 0) {
    return seed;
  }
  // A full CRC value is a finalised register, so continuing from it means
  // undoing the final inversion, updating, and inverting again. This makes
  // crc32_ieee_update(crc32_ieee(a), b) equal crc32_ieee(a || b).
  const std::uint32_t register_value = seed ^ 0xFFFFFFFFu;
  return crc32_raw(register_value, static_cast<const std::uint8_t*>(data), size) ^ 0xFFFFFFFFu;
}

}  // namespace hardware_lifecycle
