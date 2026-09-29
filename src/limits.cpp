// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Enforcement and reporting of every declared bound. A value above its bound is
// rejected with LimitExceeded and a note that carries the offending value and
// the bound; nothing is clamped, truncated or dropped.

#include "hardware_lifecycle/limits.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/result.hpp"

namespace hardware_lifecycle::limits {
namespace {

/// One "name = value" line, newline terminated. Values are rendered in decimal
/// so the report is locale independent and byte for byte reproducible.
void append_limit_line(std::string& out, std::string_view name, std::uint64_t value) {
  out.append(name);
  out.append(" = ");
  out.append(std::to_string(value));
  out.push_back('\n');
}

}  // namespace

bool within_limit(std::size_t value, std::size_t maximum) noexcept { return value <= maximum; }

Result<void> check_limit(std::string_view field, std::size_t value, std::size_t maximum) {
  if (within_limit(value, maximum)) {
    return ok();
  }
  return Error::make(ErrorCode::LimitExceeded, std::string(field) + " is above its maximum")
      .with(std::string(field),
            "value " + std::to_string(value) + " exceeds maximum " + std::to_string(maximum));
}

Result<void> check_minimum(std::string_view field, std::size_t value, std::size_t minimum) {
  if (value >= minimum) {
    return ok();
  }
  return Error::make(ErrorCode::LimitExceeded, std::string(field) + " is below its minimum")
      .with(std::string(field),
            "value " + std::to_string(value) + " is below minimum " + std::to_string(minimum));
}

std::string limits_report() {
  // Declaration order of limits.hpp, one line per constant, each line newline
  // terminated including the last.
  std::string rendered;
  append_limit_line(rendered, "kMaxIdentifierBytes", static_cast<std::uint64_t>(kMaxIdentifierBytes));
  append_limit_line(rendered, "kMaxTextBytes", static_cast<std::uint64_t>(kMaxTextBytes));
  append_limit_line(rendered, "kMaxEvidencePerRequest", static_cast<std::uint64_t>(kMaxEvidencePerRequest));
  append_limit_line(rendered, "kMaxObjects", static_cast<std::uint64_t>(kMaxObjects));
  append_limit_line(rendered, "kMaxHistoryEntriesPerObject", static_cast<std::uint64_t>(kMaxHistoryEntriesPerObject));
  append_limit_line(rendered, "kMaxAppliedPlans", static_cast<std::uint64_t>(kMaxAppliedPlans));
  append_limit_line(rendered, "kMaxRecordPayloadBytes", static_cast<std::uint64_t>(kMaxRecordPayloadBytes));
  append_limit_line(rendered, "kMaxSegmentBytes", kMaxSegmentBytes);
  append_limit_line(rendered, "kMaxExportBytes", static_cast<std::uint64_t>(kMaxExportBytes));
  append_limit_line(rendered, "kMaxJsonDepth", static_cast<std::uint64_t>(kMaxJsonDepth));
  append_limit_line(rendered, "kMaxPathUnits", static_cast<std::uint64_t>(kMaxPathUnits));
  append_limit_line(rendered, "kMaxLineageDepth", static_cast<std::uint64_t>(kMaxLineageDepth));
  append_limit_line(rendered, "kMaxSnapshotObjects", static_cast<std::uint64_t>(kMaxSnapshotObjects));
  return rendered;
}

}  // namespace hardware_lifecycle::limits
