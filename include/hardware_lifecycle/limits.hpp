#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every bound the parser, codec, journal and in-memory model enforce. Bounds are
// rejected explicitly and reported as LimitExceeded; nothing is truncated,
// clamped or silently dropped.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle::limits {

/// Longest accepted textual identifier, in bytes.
inline constexpr std::size_t kMaxIdentifierBytes = 128;

/// Longest accepted free-form text field (evidence source, wall clock, ...).
inline constexpr std::size_t kMaxTextBytes = 4096;

/// Most evidence references a single request may carry.
inline constexpr std::size_t kMaxEvidencePerRequest = 64;

/// Most hardware objects a single store may hold.
inline constexpr std::size_t kMaxObjects = 1'000'000;

/// Most history entries a single hardware object may accumulate.
inline constexpr std::size_t kMaxHistoryEntriesPerObject = 1'000'000;

/// Most applied plans the idempotency ledger may hold.
inline constexpr std::size_t kMaxAppliedPlans = 4'000'000;

/// Largest single journal record payload, in bytes.
inline constexpr std::size_t kMaxRecordPayloadBytes = 1u << 20;

/// Largest accepted journal segment file, in bytes.
inline constexpr std::uint64_t kMaxSegmentBytes = 1ull << 34;

/// Largest accepted exported document, in bytes.
inline constexpr std::size_t kMaxExportBytes = 256u << 20;

/// Deepest accepted JSON nesting.
inline constexpr std::size_t kMaxJsonDepth = 32;

/// Longest accepted path, in UTF-16 code units, before platform prefixing.
inline constexpr std::size_t kMaxPathUnits = 4096;

/// Longest acceptable replacement lineage chain.
inline constexpr std::size_t kMaxLineageDepth = 4096;

/// Largest number of objects one export or import may describe.
inline constexpr std::size_t kMaxSnapshotObjects = kMaxObjects;

/// True when value is at most maximum.
[[nodiscard]] HARDWARE_LIFECYCLE_API bool within_limit(std::size_t value, std::size_t maximum) noexcept;

/// Rejects a value above the bound with LimitExceeded, naming the field. The
/// value is never clamped.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> check_limit(std::string_view field, std::size_t value,
                                                              std::size_t maximum);

/// Rejects a value below the bound with LimitExceeded, naming the field.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> check_minimum(std::string_view field, std::size_t value,
                                                                std::size_t minimum);

/// Deterministic rendering of every bound, used by the command line tools.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string limits_report();

}  // namespace hardware_lifecycle::limits
