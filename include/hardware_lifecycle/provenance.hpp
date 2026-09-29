#pragma once

// Hardware Lifecycle - canonical physical hardware lifecycle runtime for the
// Data Center Control Plane (DCCP).
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Who asked, under which authority, against which policy generation, backed by
// which evidence, at which authoritative logical time. Wall clock time is
// carried for humans and is never used for ordering or validation.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hardware_lifecycle/digest.hpp"
#include "hardware_lifecycle/errors.hpp"
#include "hardware_lifecycle/ids.hpp"
#include "hardware_lifecycle/result.hpp"
#include "hardware_lifecycle/text.hpp"

namespace hardware_lifecycle {

// ---------------------------------------------------------------------------
// Actors
// ---------------------------------------------------------------------------

enum class ActorKind : std::uint8_t {
  Unknown = 0,
  Operator,
  Automation,
  Procurement,
  Logistics,
  Installation,
  Commissioning,
  Maintenance,
  Integrity,
  Retirement,
  Decommissioning,
  Replacement,
  Recovery,
};

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(ActorKind kind) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<ActorKind> parse_actor_kind(std::string_view text);

struct Actor {
  ActorId id;
  ActorKind kind = ActorKind::Unknown;

  friend bool operator==(const Actor& left, const Actor& right) noexcept {
    return left.id == right.id && left.kind == right.kind;
  }
};

// ---------------------------------------------------------------------------
// Tagged bit sets
// ---------------------------------------------------------------------------

struct AuthorityMaskTag;
struct EvidenceMaskTag;

/// A tagged bit set.
///
/// Authority scopes, evidence kinds and transition reasons are all 32 bit sets,
/// but confusing one with another is a real defect class: an authority mask used
/// where an evidence mask belongs would silently grant the wrong permission. Each
/// therefore carries its own type and an accidental mix does not compile.
template <class Tag>
class Mask {
 public:
  using tag_type = Tag;
  using value_type = std::uint32_t;

  constexpr Mask() noexcept = default;
  constexpr explicit Mask(value_type bits) noexcept : bits_(bits) {}

  [[nodiscard]] constexpr value_type bits() const noexcept { return bits_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
  [[nodiscard]] constexpr bool contains(Mask other) const noexcept { return (bits_ & other.bits_) == other.bits_; }
  [[nodiscard]] constexpr bool intersects(Mask other) const noexcept { return (bits_ & other.bits_) != 0; }

  friend constexpr Mask operator|(Mask left, Mask right) noexcept { return Mask(left.bits_ | right.bits_); }
  friend constexpr Mask operator&(Mask left, Mask right) noexcept { return Mask(left.bits_ & right.bits_); }
  friend constexpr Mask operator~(Mask value) noexcept { return Mask(~value.bits_); }
  friend constexpr bool operator==(Mask left, Mask right) noexcept { return left.bits_ == right.bits_; }
  friend constexpr bool operator!=(Mask left, Mask right) noexcept { return !(left == right); }

  constexpr Mask& operator|=(Mask other) noexcept {
    bits_ |= other.bits_;
    return *this;
  }
  constexpr Mask& operator&=(Mask other) noexcept {
    bits_ &= other.bits_;
    return *this;
  }

 private:
  value_type bits_ = 0;
};

// ---------------------------------------------------------------------------
// Authority scopes
// ---------------------------------------------------------------------------

/// The capability a caller must hold to perform a class of lifecycle change.
/// Scopes are granted by an external authority system; this runtime only checks
/// that the grant covers what the transition table requires.
enum class AuthorityScope : std::uint8_t {
  Procurement = 0,
  Logistics,
  Installation,
  Commissioning,
  Service,
  Maintenance,
  Integrity,
  Retirement,
  Decommissioning,
  Replacement,
  Recovery,
};

/// Number of AuthorityScope enumerators, so valid values are 0 .. count - 1.
inline constexpr std::size_t kAuthorityScopeCount = 11;

using AuthorityMask = Mask<AuthorityMaskTag>;

[[nodiscard]] constexpr AuthorityMask authority_bit(AuthorityScope scope) noexcept {
  return AuthorityMask(static_cast<AuthorityMask::value_type>(1u) << static_cast<unsigned>(scope));
}

[[nodiscard]] constexpr AuthorityMask authority_all() noexcept {
  return AuthorityMask((static_cast<AuthorityMask::value_type>(1u) << kAuthorityScopeCount) - 1u);
}

/// Stable comma separated rendering in declaration order.
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string to_string(AuthorityMask mask);

[[nodiscard]] HARDWARE_LIFECYCLE_API Result<AuthorityScope> parse_authority_scope(std::string_view text);

[[nodiscard]] HARDWARE_LIFECYCLE_API Result<AuthorityMask> parse_authority_mask(std::string_view text);

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

/// The kind of artifact that justifies a transition. The runtime never
/// interprets the artifact itself: it binds the digest the caller supplies.
enum class EvidenceKind : std::uint8_t {
  Unknown = 0,
  ProcurementRecord,
  CancellationRecord,
  DeliveryReceipt,
  StorageRecord,
  InstallationRecord,
  LocationRecord,
  CommissioningReport,
  ServiceRecord,
  HealthEvidence,
  MaintenanceRecord,
  IntegrityReport,
  DrainRecord,
  RetirementApproval,
  DecommissioningRecord,
  ReplacementAuthorization,
  SuccessorRecord,
  RemovalRecord,
  RecoveryAttestation,
};

/// Number of EvidenceKind enumerators, including Unknown, so valid values are
/// 0 .. count - 1 and the last kind that owns a mask bit is count - 1.
inline constexpr std::size_t kEvidenceKindCount = 19;

using EvidenceMask = Mask<EvidenceMaskTag>;

/// Empty for EvidenceKind::Unknown: an unset kind grants no requirement.
[[nodiscard]] constexpr EvidenceMask evidence_bit(EvidenceKind kind) noexcept {
  if (kind == EvidenceKind::Unknown) {
    return EvidenceMask();
  }
  return EvidenceMask(static_cast<EvidenceMask::value_type>(1u) << (static_cast<unsigned>(kind) - 1u));
}

[[nodiscard]] HARDWARE_LIFECYCLE_API std::string_view to_string(EvidenceKind kind) noexcept;
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<EvidenceKind> parse_evidence_kind(std::string_view text);
[[nodiscard]] HARDWARE_LIFECYCLE_API std::string to_string(EvidenceMask mask);

/// One evidence reference: what kind of artifact, the digest of its exact
/// bytes, where it came from, and which observation sequence produced it.
struct EvidenceRef {
  EvidenceKind kind = EvidenceKind::Unknown;
  Digest digest;
  std::string source;
  ObservationSequence observed_sequence;

  friend bool operator==(const EvidenceRef& left, const EvidenceRef& right) noexcept {
    return left.kind == right.kind && left.digest == right.digest && left.source == right.source &&
           left.observed_sequence == right.observed_sequence;
  }
};

// ---------------------------------------------------------------------------
// Wall clock (advisory)
// ---------------------------------------------------------------------------

/// An RFC 3339 UTC instant, kept only so that humans can read the record. It is
/// never used to order, fence or validate anything: logical time is the
/// authoritative ordering. An unset wall clock is empty, never "now".
class HARDWARE_LIFECYCLE_API WallClock {
 public:
  WallClock() noexcept = default;

  /// Strict "YYYY-MM-DDTHH:MM:SS[.fraction]Z" parse with calendar validation.
  [[nodiscard]] static Result<WallClock> parse(std::string_view text);

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] bool has_value() const noexcept { return !value_.empty(); }

  friend bool operator==(const WallClock& left, const WallClock& right) noexcept { return left.value_ == right.value_; }

 private:
  explicit WallClock(std::string value) : value_(std::move(value)) {}

  std::string value_;
};

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

/// The complete justification attached to one externally meaningful mutation.
struct Provenance {
  Actor actor;
  AuthorityMask authority;
  PolicyGeneration policy_generation;
  PlanId plan;
  AttemptId attempt;
  std::vector<EvidenceRef> evidence;
  LogicalTime logical_time;
  WallClock wall_clock;
};

[[nodiscard]] HARDWARE_LIFECYCLE_API bool has_authority(const Provenance& provenance, AuthorityMask required) noexcept;

[[nodiscard]] HARDWARE_LIFECYCLE_API EvidenceMask evidence_mask(const Provenance& provenance) noexcept;

[[nodiscard]] HARDWARE_LIFECYCLE_API bool has_evidence(const Provenance& provenance, EvidenceMask required) noexcept;

/// Structural validation of provenance on its own terms: actor identity, the
/// plan and attempt identities, evidence count bound, evidence kinds, evidence
/// digests and evidence sources. Records the issues it finds and returns the
/// most primary one.
[[nodiscard]] HARDWARE_LIFECYCLE_API Result<void> validate_provenance(const Provenance& provenance);

}  // namespace hardware_lifecycle