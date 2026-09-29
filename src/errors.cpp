// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The stable error taxonomy: machine-readable spellings, retry and durability
// classification, and the deterministic validation precedence table.
// validation_rank() is the single source of truth for precedence; the
// enumerator order in errors.hpp mirrors the table below but never defines it.

#include "hardware_lifecycle/errors.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace hardware_lifecycle {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "ok";
    case ErrorCode::MalformedRequest:
      return "malformed_request";
    case ErrorCode::LimitExceeded:
      return "limit_exceeded";
    case ErrorCode::UnsupportedFormatVersion:
      return "unsupported_format_version";
    case ErrorCode::IntegrityFailure:
      return "integrity_failure";
    case ErrorCode::TrailingBytes:
      return "trailing_bytes";
    case ErrorCode::UnknownAsset:
      return "unknown_asset";
    case ErrorCode::ObjectExists:
      return "object_exists";
    case ErrorCode::StaleHardwareGeneration:
      return "stale_hardware_generation";
    case ErrorCode::PlanConflict:
      return "plan_conflict";
    case ErrorCode::StaleAuthority:
      return "stale_authority";
    case ErrorCode::StaleLifecycleGeneration:
      return "stale_lifecycle_generation";
    case ErrorCode::StaleRevision:
      return "stale_revision";
    case ErrorCode::StateMismatch:
      return "state_mismatch";
    case ErrorCode::IllegalTransition:
      return "illegal_transition";
    case ErrorCode::TerminalState:
      return "terminal_state";
    case ErrorCode::InsufficientAuthority:
      return "insufficient_authority";
    case ErrorCode::MissingEvidence:
      return "missing_evidence";
    case ErrorCode::MissingLocation:
      return "missing_location";
    case ErrorCode::MissingSuccessor:
      return "missing_successor";
    case ErrorCode::InvalidSuccessor:
      return "invalid_successor";
    case ErrorCode::SelfReplacement:
      return "self_replacement";
    case ErrorCode::LineageCycle:
      return "lineage_cycle";
    case ErrorCode::StoreLocked:
      return "store_locked";
    case ErrorCode::StoreCorrupt:
      return "store_corrupt";
    case ErrorCode::StoreUninitialized:
      return "store_uninitialized";
    case ErrorCode::StoreClosed:
      return "store_closed";
    case ErrorCode::ReadOnlyAuthority:
      return "read_only_authority";
    case ErrorCode::ObjectNotFound:
      return "object_not_found";
    case ErrorCode::IoFailure:
      return "io_failure";
    case ErrorCode::PermissionDenied:
      return "permission_denied";
    case ErrorCode::PathTraversal:
      return "path_traversal";
    case ErrorCode::ReparsePointRejected:
      return "reparse_point_rejected";
    case ErrorCode::AlternateDataStream:
      return "alternate_data_stream";
    case ErrorCode::UnsupportedOperation:
      return "unsupported_operation";
    case ErrorCode::CounterOverflow:
      return "counter_overflow";
    case ErrorCode::Internal:
      return "internal";
  }
  // A value outside the declared enumerator set is an internal inconsistency; it
  // is reported with the catch-all spelling rather than an empty or invented one.
  return "internal";
}

bool is_retryable(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::StaleAuthority:
    case ErrorCode::StaleHardwareGeneration:
    case ErrorCode::StaleLifecycleGeneration:
    case ErrorCode::StaleRevision:
    case ErrorCode::StateMismatch:
    case ErrorCode::PlanConflict:
    case ErrorCode::StoreLocked:
      return true;
    default:
      return false;
  }
}

std::uint32_t validation_rank(ErrorCode code) noexcept {
  // Lower rank wins when a request violates several rules at once: a request is
  // judged by the earliest rule of the deterministic validation sequence it
  // breaks, so the reported error never depends on evaluation order, container
  // iteration order, thread scheduling or unrelated state. Whether an input can
  // be considered at all - its bounds and its format version - comes before
  // whether it is well formed, which comes before identity, which comes before
  // authority and fencing, which comes before transition legality. The ranks are
  // spaced so later checks can be inserted without renumbering the table. Codes
  // that request validation never produces - the environment, durability and I/O
  // failures, and Internal - rank zero.
  switch (code) {
    case ErrorCode::LimitExceeded:
      return 5;
    case ErrorCode::UnsupportedFormatVersion:
      return 8;
    case ErrorCode::MalformedRequest:
      return 10;
    case ErrorCode::TrailingBytes:
      return 15;
    case ErrorCode::IntegrityFailure:
      return 18;
    case ErrorCode::UnknownAsset:
      return 100;
    case ErrorCode::ObjectExists:
      return 105;
    case ErrorCode::StaleHardwareGeneration:
      return 110;
    case ErrorCode::PlanConflict:
      return 120;
    case ErrorCode::StaleAuthority:
      return 200;
    case ErrorCode::StaleLifecycleGeneration:
      return 210;
    case ErrorCode::StaleRevision:
      return 220;
    case ErrorCode::StateMismatch:
      return 230;
    case ErrorCode::IllegalTransition:
      return 300;
    case ErrorCode::TerminalState:
      return 305;
    case ErrorCode::InsufficientAuthority:
      return 310;
    case ErrorCode::MissingEvidence:
      return 320;
    case ErrorCode::MissingLocation:
      return 325;
    case ErrorCode::MissingSuccessor:
      return 330;
    case ErrorCode::InvalidSuccessor:
      return 335;
    case ErrorCode::SelfReplacement:
      return 340;
    case ErrorCode::LineageCycle:
      return 345;
    default:
      return 0;
  }
}

bool is_validation_code(ErrorCode code) noexcept { return validation_rank(code) != 0; }

ErrorCode primary_error(ErrorCode left, ErrorCode right) noexcept {
  const std::uint32_t left_rank = validation_rank(left);
  const std::uint32_t right_rank = validation_rank(right);
  if (left_rank == 0) {
    return right_rank == 0 ? left : right;
  }
  if (right_rank == 0) {
    return left;
  }
  // Equal ranks are impossible for distinct codes; identical codes resolve to
  // the left operand so the result is a pure function of the pair.
  return left_rank <= right_rank ? left : right;
}

bool is_store_failure(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::StoreLocked:
    case ErrorCode::StoreCorrupt:
    case ErrorCode::StoreUninitialized:
    case ErrorCode::StoreClosed:
    case ErrorCode::IoFailure:
    case ErrorCode::PermissionDenied:
    case ErrorCode::PathTraversal:
    case ErrorCode::ReparsePointRejected:
    case ErrorCode::AlternateDataStream:
      return true;
    default:
      return false;
  }
}

bool is_integrity_failure(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::IntegrityFailure:
    case ErrorCode::TrailingBytes:
    case ErrorCode::StoreCorrupt:
    case ErrorCode::UnsupportedFormatVersion:
      return true;
    default:
      return false;
  }
}

Error Error::make(ErrorCode code, std::string message) {
  Error error;
  error.code = code;
  error.message = std::move(message);
  return error;
}

Error& Error::with(std::string field, std::string detail) {
  notes.push_back(FieldNote{std::move(field), std::move(detail)});
  return *this;
}

std::string describe(const Error& error) {
  std::string rendered(to_string(error.code));
  rendered.append(": ");
  rendered.append(error.message);
  if (!error.notes.empty()) {
    rendered.append(" (");
    for (std::size_t index = 0; index < error.notes.size(); ++index) {
      if (index != 0) {
        rendered.append("; ");
      }
      rendered.append(error.notes[index].field);
      rendered.append(": ");
      rendered.append(error.notes[index].detail);
    }
    rendered.append(")");
  }
  return rendered;
}

Error make_error(ErrorCode code, std::string message) { return Error::make(code, std::move(message)); }

}  // namespace hardware_lifecycle
