#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This is the base header of the public interface: it carries the shared
// library linkage marker, the stable error taxonomy and the deterministic
// validation precedence table. Every other public header includes it.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Linkage marker. It is applied to free functions and to classes that have
// out-of-line members, never to plain data structs: value types cross the
// shared-library boundary through the shared C runtime.
#if defined(HARDWARE_LIFECYCLE_SHARED)
#  if defined(_MSC_VER)
#    if defined(HARDWARE_LIFECYCLE_BUILDING)
#      define HARDWARE_LIFECYCLE_API __declspec(dllexport)
#    else
#      define HARDWARE_LIFECYCLE_API __declspec(dllimport)
#    endif
#  else
#    define HARDWARE_LIFECYCLE_API __attribute__((visibility("default")))
#  endif
#else
#  define HARDWARE_LIFECYCLE_API
#endif

namespace hardware_lifecycle {

/// Stable error taxonomy.
///
/// Enumerators are grouped by the check that produces them, and the groups
/// appear in the order of the deterministic validation precedence documented by
/// validation_rank(). Enumerator order is never the precedence source:
/// validation_rank() is the single source of truth, and a test asserts that the
/// two agree for every validation code.
///
/// The order within the first group is deliberate. Whether an input can be
/// considered at all comes before whether it is well formed: a bounded format
/// rejects an over sized or unsupported document before interpreting its
/// contents, so LimitExceeded and UnsupportedFormatVersion are more primary than
/// MalformedRequest. That is also why an over long collection never has its
/// elements examined one by one: the collection is rejected as a whole.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // Structural and decoding checks, most primary first.
  LimitExceeded,
  UnsupportedFormatVersion,
  MalformedRequest,
  TrailingBytes,
  IntegrityFailure,

  // Identity checks.
  UnknownAsset,
  ObjectExists,
  StaleHardwareGeneration,
  PlanConflict,

  // Authority and fencing checks.
  StaleAuthority,
  StaleLifecycleGeneration,
  StaleRevision,
  StateMismatch,

  // Transition legality checks.
  IllegalTransition,
  TerminalState,
  InsufficientAuthority,
  MissingEvidence,
  MissingLocation,
  MissingSuccessor,
  InvalidSuccessor,
  SelfReplacement,
  LineageCycle,

  // Environment, durability and I/O failures.
  StoreLocked,
  StoreCorrupt,
  StoreUninitialized,
  StoreClosed,
  ReadOnlyAuthority,
  ObjectNotFound,
  IoFailure,
  PermissionDenied,
  PathTraversal,
  ReparsePointRejected,
  AlternateDataStream,
  UnsupportedOperation,
  CounterOverflow,
  Internal,
};

/// Stable, machine-readable, snake_case spelling of an error code. This spelling
/// is part of the command line interface contract.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(ErrorCode code) noexcept;

/// True when retrying the identical request could plausibly succeed after the
/// caller refreshes its view of authority or state.
[[nodiscard]] HARDWARE_LIFECYCLE_API bool is_retryable(ErrorCode code) noexcept;

/// True when the code reports a rejection produced by the deterministic
/// validation sequence. Exactly these codes have a non-zero precedence rank.
[[nodiscard]] HARDWARE_LIFECYCLE_API bool is_validation_code(ErrorCode code) noexcept;

/// Position of a validation code in the deterministic validation sequence.
/// Lower ranks win: when one request violates several rules at once the runtime
/// reports the lowest-ranking violation as the primary error, regardless of map
/// iteration order, thread scheduling or unrelated state. Returns 0 for codes
/// that request validation never produces.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::uint32_t validation_rank(ErrorCode code) noexcept;

/// The more primary of two validation outcomes, as ranked above.
[[nodiscard]] HARDWARE_LIFECYCLE_API ErrorCode primary_error(ErrorCode left, ErrorCode right) noexcept;

/// True when the code reports a durable store or file system failure.
[[nodiscard]] HARDWARE_LIFECYCLE_API bool is_store_failure(ErrorCode code) noexcept;

/// True when the code reports corrupt, truncated or otherwise unverifiable
/// persisted data. These codes are never downgraded to an empty or fresh state.
[[nodiscard]] HARDWARE_LIFECYCLE_API bool is_integrity_failure(ErrorCode code) noexcept;

/// One structured field note that travels with an error.
struct FieldNote {
  std::string field;
  std::string detail;
};

/// A rejection, with an optional trail of structured notes. The message is for
/// humans; the code is the contract.
///
/// The class is exported rather than the individual members because make() and
/// with() are defined out of line and a downstream consumer calls both.
struct HARDWARE_LIFECYCLE_API Error {
  ErrorCode code = ErrorCode::Internal;
  std::string message;
  std::vector<FieldNote> notes;

  [[nodiscard]] static Error make(ErrorCode code, std::string message);
  [[nodiscard]] Error& with(std::string field, std::string detail);
};

/// Single-line rendering: "code: message (field: detail; field: detail)".
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string describe(const Error& error);

/// Convenience constructor shared by the validation and persistence layers.
[[nodiscard]] HARDWARE_LIFECYCLE_API Error make_error(ErrorCode code, std::string message);

}  // namespace hardware_lifecycle
