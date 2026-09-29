#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Durable format versions are separate from the library version. A reader
// accepts exactly the ranges declared here and rejects everything else with
// UnsupportedFormatVersion; it never guesses at a newer layout.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle {

/// Version of the append-only journal segment format.
inline constexpr std::uint16_t kJournalFormatVersion = 1;
inline constexpr std::uint16_t kJournalFormatVersionMinReadable = 1;

/// Version of the store manifest format.
inline constexpr std::uint16_t kManifestFormatVersion = 1;
inline constexpr std::uint16_t kManifestFormatVersionMinReadable = 1;

/// Version of the export and import document format.
inline constexpr std::uint16_t kExportFormatVersion = 1;
inline constexpr std::uint16_t kExportFormatVersionMinReadable = 1;

/// One durable format this build knows about.
struct FormatRange {
  std::string_view name;
  std::uint16_t current = 0;
  std::uint16_t min_readable = 0;
};

/// Every durable format this build reads and writes.
[[nodiscard]] HARDWARE_LIFECYCLE_API const FormatRange* format_ranges(std::size_t& count) noexcept;

/// True when version is inside the inclusive range [min_readable, current].
[[nodiscard]] HARDWARE_LIFECYCLE_API bool is_supported(std::uint16_t current, std::uint16_t min_readable,
                                                       std::uint16_t version) noexcept;

/// Rejects anything outside the declared range with UnsupportedFormatVersion.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> check_supported(std::string_view name, std::uint16_t current,
                                                                  std::uint16_t min_readable,
                                                                  std::uint16_t version);

/// Deterministic human-readable matrix, used by the CLI and the README.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string compatibility_matrix();

}  // namespace hardware_lifecycle
