#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

inline constexpr std::uint32_t kLibraryVersionMajor = 1;
inline constexpr std::uint32_t kLibraryVersionMinor = 0;
inline constexpr std::uint32_t kLibraryVersionPatch = 0;

/// Semantic version of the library implementation. It is deliberately separate
/// from the durable format versions in compatibility.hpp: a build may change
/// without changing the on-disk layout.
struct Version {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;
};

[[nodiscard]] constexpr Version library_version() noexcept {
  return Version{kLibraryVersionMajor, kLibraryVersionMinor, kLibraryVersionPatch};
}

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view library_version_string() noexcept;

/// Canonical "major.minor.patch" rendering.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string to_string(Version version);

/// Strict parse of "major.minor.patch". Rejects empty parts, leading zeroes,
/// a leading plus, surrounding whitespace, trailing characters and components
/// above 2^32-1.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<Version> parse_version(std::string_view text);

/// Returns -1, 0 or 1.
[[nodiscard]] HARDWARE_LIFECYCLE_API int compare(Version left, Version right) noexcept;

}  // namespace hardware_lifecycle
